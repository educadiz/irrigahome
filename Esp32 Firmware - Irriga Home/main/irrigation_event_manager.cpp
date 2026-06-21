// Responsabilidade: implementar monitoramento de eventos de irrigação.
// O que faz: detectar início/fim, gerar timestamps ISO 8601, persistir estado,
// fila de eventos MQTT e integração com comunicação remota.

#include "irrigation_event_manager.h"
#include "config.h"
#include "firmware_logger.h"
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include "flow_meter_manager.h"
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Nota: a instância global `irrigationEventManager` é definida em `main.ino`.
// Evita redefinição múltipla durante o link (ver: erro de "multiple definition").

// Constantes
static const char* STATE_FILE = "/irrg_state.json";
static const char* HISTORY_FILE = "/irrg_history.jsonl";
static const unsigned long RECOVERY_CHECK_INTERVAL_MS = 5000UL;
static unsigned long lastRecoveryCheck = 0;

static bool appendHistoryLine(const IrrigationHistoryEntry& entry) {
    File f = LittleFS.open(HISTORY_FILE, "a");
    if (!f) {
        fwLogLine("ERROR", "IRR", "Erro ao abrir historico para append");
        return false;
    }

    char line[384];
    snprintf(line, sizeof(line),
             "{\"eventId\":\"%s\",\"startAt\":%ld,\"endAt\":%ld,"
             "\"durationSec\":%d,\"trigger\":%d,\"stopReason\":%d,"
             "\"totalPulses\":%u,\"totalVolumeLiters\":%.3f,\"avgFlowRateLpm\":%.3f,"
             "\"totalVolumeMl\":%.1f,\"accountingVolumeMl\":%.1f,\"nominalFlowRateMlPerMin\":%.1f,"
             "\"success\":%s,\"sentToFirebase\":%s}",
             entry.eventId,
             (long)entry.startAt,
             (long)entry.endAt,
             entry.durationSec,
             (int)entry.trigger,
             (int)entry.stopReason,
             entry.totalPulses,
             entry.totalVolumeLiters,
             entry.avgFlowRateLpm,
             entry.totalVolumeMl,
             entry.accountingVolumeMl,
             entry.nominalFlowRateMlPerMin,
             entry.isSuccess() ? "true" : "false",
             entry.sentToFirebase ? "true" : "false");
    f.println(line);
    f.close();
    return true;
}

// Initialize irrigation event manager: LittleFS, state, and queue.
// Safe to call multiple times; logs status for debugging.
void IrrigationEventManager::begin() {
    // Inicializar LittleFS para persistência
    if (!LittleFS.begin()) {
        fwLogLine("WARN", "IRR", "Falha ao montar LittleFS; tentando formatar");
        if (!LittleFS.begin(true)) {
            fwLogLine("ERROR", "IRR", "LittleFS nao pode ser inicializado");
        } else {
            fwLogLine("INFO", "IRR", "LittleFS montado apos formatacao");
        }
    }

    // Inicializar estado ativo
    activeState.isActive = false;
    activeState.startAt = 0;
    activeState.trigger = TRIGGER_UNKNOWN;
    memset(activeState.eventId, 0, sizeof(activeState.eventId));

    // Inicializar fila de eventos
    for (int i = 0; i < MAX_IRRIGATION_QUEUE; i++) {
        eventQueue[i].used = false;
        memset(eventQueue[i].eventId, 0, sizeof(eventQueue[i].eventId));
        eventQueue[i].startAt = 0;
        eventQueue[i].endAt = 0;
        eventQueue[i].durationSec = 0;
        eventQueue[i].trigger = TRIGGER_UNKNOWN;
        eventQueue[i].stopReason = STOP_REASON_COMPLETED;
        eventQueue[i].totalPulses = 0;
        eventQueue[i].totalVolumeLiters = 0.0;
        eventQueue[i].avgFlowRateLpm = 0.0;
        eventQueue[i].totalVolumeMl = 0.0f;
        eventQueue[i].accountingVolumeMl = 0.0f;
        eventQueue[i].nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
        eventQueue[i].flowDetected = false;
        memset(eventQueue[i].flowStatus, 0, sizeof(eventQueue[i].flowStatus));
    }

    // Inicializar conjunto de IDs enviados
    sentIdsCount = 0;
    memset(sentIds, 0, sizeof(sentIds));

    // Criar mutex para proteger historyBuffer e LittleFS entre Core 0 (firebaseTask) e Core 1
    if (_historyMutex == nullptr) {
        _historyMutex = xSemaphoreCreateMutex();
        if (_historyMutex == nullptr) {
            fwLogLine("ERROR", "IRR", "Falha ao criar mutex do historico");
        }
    }

    // Inicializar histórico persistido
    historyCount = 0;
    for (int i = 0; i < MAX_IRRIGATION_HISTORY; i++) {
        memset(historyBuffer[i].eventId, 0, sizeof(historyBuffer[i].eventId));
        historyBuffer[i].startAt = 0;
        historyBuffer[i].endAt = 0;
        historyBuffer[i].durationSec = 0;
        historyBuffer[i].trigger = TRIGGER_UNKNOWN;
        historyBuffer[i].stopReason = STOP_REASON_COMPLETED;
        historyBuffer[i].totalPulses = 0;
        historyBuffer[i].totalVolumeLiters = 0.0;
        historyBuffer[i].avgFlowRateLpm = 0.0;
        historyBuffer[i].totalVolumeMl = 0.0f;
        historyBuffer[i].accountingVolumeMl = 0.0f;
        historyBuffer[i].nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
    }

    // Tentar carregar estado anterior (após reboot inesperado)
    loadState();
    loadHistory();

    Serial.print("💧 IrrigationEventManager iniciado. Estado ativo: ");
    Serial.println(activeState.isActive ? "SIM" : "NÃO");
}

// Called when irrigation starts: sets active state, generates event id and persists state.
// Non-blocking and safe to call from actuator control paths.
void IrrigationEventManager::onIrrigationStart(IrrigationTriggerType trigger) {
    // Evitar duplicata: só registra se não estiver ativo
    if (activeState.isActive) {
        fwLogLine("WARN", "IRR", "onIrrigationStart chamado mas a irrigacao ja estava ativa");
        return;
    }

    activeState.isActive = true;
    activeState.startAt = time(nullptr);

    // Guard: timestamp inválido indica NTP ainda não sincronizado.
    // Um valor abaixo de 1_000_000_000 (ano 2001) é claramente inválido no ESP32.
    // Mantemos o estado ativo mas marcamos o startAt como zero para sinalizar
    // que o ISO 8601 gerado será inútil — o caller verá nas mensagens de log.
    if (activeState.startAt < 1000000000L) {
        fwLogLine("WARN", "IRR", "Clock NTP invalido no inicio da irrigacao; timestamp pode ficar impreciso");
        // Não bloqueia o acionamento; a irrigação ocorre normalmente.
        // O historico registrará startAt=0, que o app pode tratar como "timestamp desconhecido".
    }
    activeState.trigger = trigger;

    generateNewEventId();

    // Iniciar medicao de vazao para registrar volume do evento
#ifdef FLOW_SENSOR_PIN
    extern FlowMeterManager flowMeter;
    Serial.println("[IRRG] Iniciando medição de vazão do sensor...");
    flowMeter.startMeasurement();
#endif

    // Salvar estado para recuperação após reboot.
    // Protegido pelo mutex: evita race com removeFromHistory() (Core 0)
    // que pode chamar saveHistory() simultaneamente via LittleFS.
    if (_historyMutex) xSemaphoreTake(_historyMutex, portMAX_DELAY);
    saveState();
    if (_historyMutex) xSemaphoreGive(_historyMutex);

    char isoTime[32];
    formatIso8601(activeState.startAt, isoTime, sizeof(isoTime));

    fwLogf("INFO", "IRR", "Irrigacao iniciada | id=%s | tipo=%s | hora=%s", activeState.eventId, getTriggerTypeString(trigger), isoTime);
}

// Called when irrigation ends: builds event, enqueues for MQTT and clears active state.
// Designed to be resilient to full queue (drops oldest) and non-blocking.
void IrrigationEventManager::onIrrigationEnd(IrrigationStopReason reason) {
    // Evitar duplicata: só registra se estiver ativo
    if (!activeState.isActive) {
        fwLogLine("WARN", "IRR", "onIrrigationEnd chamado mas a irrigacao nao estava ativa");
        return;
    }

    time_t endTime = time(nullptr);
    int durationSeconds = (int)(endTime - activeState.startAt);

    // Criar evento completo
    IrrigationEvent event;
    event.used = true;
    event.sentToMqtt = false;
    strncpy(event.eventId, activeState.eventId, sizeof(event.eventId) - 1);
    event.startAt = activeState.startAt;
    event.endAt = endTime;
    event.durationSec = durationSeconds > 0 ? durationSeconds : 0;
    event.trigger = activeState.trigger;
    event.stopReason = reason;

    // Coletar dados do sensor de vazao ao final do evento
#ifdef FLOW_SENSOR_PIN
    extern FlowMeterManager flowMeter;
    // Parar medição de pulso (se vinculada) — NÃO usar contagem de pulsos
    // para calcular volume. A regra atual: cada 6 segundos de funcionamento
    // produz 72 mL. Portanto, volume medido = durationSeconds * (72/6).
    fwLogLine("INFO", "FLOW", "Parando medicao de vazao; volume final calculado por tempo");
    flowMeter.stopMeasurement();

    // Registrar a contagem de pulsos para visualização/debug, mas NÃO a
    // utilizamos nos cálculos de volume/contabilidade.
    event.totalPulses = flowMeter.getTotalPulses();
    float measuredMl = ((float)event.durationSec) * (72.0f / 6.0f); // 12 mL por segundo
    if (measuredMl < 0.0f) measuredMl = 0.0f;
    event.totalVolumeMl = measuredMl;
    event.totalVolumeLiters = event.totalVolumeMl / 1000.0f;
    if (event.durationSec > 0) {
        event.avgFlowRateLpm = event.totalVolumeLiters / ((float)event.durationSec / 60.0f);
    } else {
        event.avgFlowRateLpm = 0.0f;
    }

    // Ainda usamos o sensor para sinalizar se houve fluxo (status), se disponível.
    event.flowDetected = flowMeter.hasFlowDetected();
    if (event.flowDetected) {
        strncpy(event.flowStatus, "OK", sizeof(event.flowStatus) - 1);
    } else {
        strncpy(event.flowStatus, "ZERO_FLOW", sizeof(event.flowStatus) - 1);
    }

    // Para fins de relatório e contabilidade, usamos o volume medido por tempo.
    event.nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
    event.accountingVolumeMl = event.totalVolumeMl;
    fwLogf("INFO", "IRR", "Dados por tempo | volume=%.3fL | volume_mL=%.1f | taxa=%.3fL/min | status=%s", event.totalVolumeLiters, event.totalVolumeMl, event.avgFlowRateLpm, event.flowStatus);
#else
    event.totalPulses = 0;
    event.totalVolumeLiters = 0.0;
    event.avgFlowRateLpm = 0.0;
    event.totalVolumeMl = 0.0f;
    event.nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
    event.accountingVolumeMl = ((float)event.durationSec * event.nominalFlowRateMlPerMin) / 60.0f;
    event.flowDetected = false;
    strncpy(event.flowStatus, "N/A", sizeof(event.flowStatus) - 1);
#endif

    // Adicionar à fila (procurar slot livre)
    bool enqueued = false;
    for (int i = 0; i < MAX_IRRIGATION_QUEUE; i++) {
        if (!eventQueue[i].used) {
            eventQueue[i] = event;
            enqueued = true;
            fwLogf("INFO", "IRR", "Evento adicionado na fila | slot=%d | duracao=%ds | motivo=%s", i, durationSeconds, getStopReasonString(reason));
            break;
        }
    }

    if (!enqueued) {
        fwLogLine("ERROR", "IRR", "Fila de eventos cheia; evento descartado");
    }

    // Registrar o evento concluído no histórico persistido
    addHistoryEvent(event);

    // Resetar estado ativo e persistência.
    // Protegido pelo mutex: clearState() e addHistoryEvent() (que chama saveHistory /
    // appendHistoryLine) devem ser atômicos em relação à firebaseTask (Core 0).
    if (_historyMutex) xSemaphoreTake(_historyMutex, portMAX_DELAY);
    activeState.isActive = false;
    activeState.startAt = 0;
    activeState.trigger = TRIGGER_UNKNOWN;
    memset(activeState.eventId, 0, sizeof(activeState.eventId));
    clearState();
    if (_historyMutex) xSemaphoreGive(_historyMutex);
}

// Periodic housekeeping: recovery checks and background processing of queued events.
void IrrigationEventManager::update() {
    // Processamento periódico: verificar recuperação de estado após reboot
    unsigned long now = millis();
    if ((now - lastRecoveryCheck) >= RECOVERY_CHECK_INTERVAL_MS) {
        lastRecoveryCheck = now;

        // Detecta estado órfão: activeState.isActive ficou verdadeiro (carregado
        // do arquivo de persistência após reboot) mas a bomba está fisicamente
        // desligada. Isso ocorre quando o dispositivo reinicia durante uma
        // irrigação ativa — sem esta verificação o flag permanece travado para
        // sempre, bloqueando qualquer novo acionamento até reinicialização manual.
        if (activeState.isActive && activeState.startAt > 0 && digitalRead(PUMP_PIN) == LOW) {
            fwLogLine("WARN", "IRR", "Estado orfao detectado pos-reboot; encerrando irrigacao pendente");
            onIrrigationEnd(STOP_REASON_COMPLETED);
        }
    }
}

bool IrrigationEventManager::isIrrigationActive() {
    return activeState.isActive;
}

int IrrigationEventManager::getActiveIrrigationSeconds() {
    if (!activeState.isActive || activeState.startAt == 0) {
        return 0;
    }

    time_t now = time(nullptr);
    int seconds = (int)(now - activeState.startAt);
    return seconds > 0 ? seconds : 0;
}

const char* IrrigationEventManager::getTriggerTypeString(IrrigationTriggerType trigger) {
    switch (trigger) {
        case TRIGGER_MANUAL:
            return "manual";
        case TRIGGER_AUTOMATIC:
            return "automatic";
        case TRIGGER_SCHEDULE:
            return "schedule";
        default:
            return "unknown";
    }
}

const char* IrrigationEventManager::getStopReasonString(IrrigationStopReason reason) {
    switch (reason) {
        case STOP_REASON_COMPLETED:
            return "completed";
        case STOP_REASON_MANUAL:
            return "manual";
        case STOP_REASON_NO_WATER:
            return "no_water";
        case STOP_REASON_NO_FLOW:
            return "no_flow";
        default:
            return "unknown";
    }
}

int IrrigationEventManager::getPendingEventCount() {
    int count = 0;
    for (int i = 0; i < MAX_IRRIGATION_QUEUE; i++) {
        if (eventQueue[i].used && !eventQueue[i].sentToMqtt) {
            count++;
        }
    }
    return count;
}

bool IrrigationEventManager::getNextPendingEvent(IrrigationEvent* outEvent) {
    if (!outEvent) {
        return false;
    }

    for (int i = 0; i < MAX_IRRIGATION_QUEUE; i++) {
        if (eventQueue[i].used && !eventQueue[i].sentToMqtt) {
            *outEvent = eventQueue[i];
            return true;
        }
    }

    return false;
}

void IrrigationEventManager::markEventSent(const char* eventId) {
    if (!eventId) {
        return;
    }

    for (int i = 0; i < MAX_IRRIGATION_QUEUE; i++) {
        if (eventQueue[i].used && strcmp(eventQueue[i].eventId, eventId) == 0) {
            // Libera o slot para evitar lotacao silenciosa da fila circular.
            eventQueue[i].used = false;
            eventQueue[i].sentToMqtt = true;
            memset(eventQueue[i].eventId, 0, sizeof(eventQueue[i].eventId));
            eventQueue[i].startAt = 0;
            eventQueue[i].endAt = 0;
            eventQueue[i].durationSec = 0;
            eventQueue[i].trigger = TRIGGER_UNKNOWN;
            eventQueue[i].stopReason = STOP_REASON_COMPLETED;
            eventQueue[i].totalPulses = 0;
            eventQueue[i].totalVolumeLiters = 0.0f;
            eventQueue[i].avgFlowRateLpm = 0.0f;
            eventQueue[i].totalVolumeMl = 0.0f;
            eventQueue[i].accountingVolumeMl = 0.0f;
            eventQueue[i].nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
            eventQueue[i].flowDetected = false;
            memset(eventQueue[i].flowStatus, 0, sizeof(eventQueue[i].flowStatus));
            // NÃO chama markIdAsSent() — sentIds[] é RAM volátil e zera no reboot.
            // O controle persistente é o campo sentToFirebase no JSONL.
            fwLogf("INFO", "IRR", "Evento MQTT enviado e removido da fila RAM: %s", eventId);
            return;
        }
    }
}

int IrrigationEventManager::getHistoryCount() {
    return historyCount;
}

bool IrrigationEventManager::getHistoryEvent(int index, IrrigationHistoryEntry* outEvent) {
    if (!outEvent || index < 0 || index >= historyCount) {
        return false;
    }

    *outEvent = historyBuffer[index];
    return true;
}

// Convert Unix timestamp to ISO 8601 (UTC-local) string. Returns false on invalid args.
bool IrrigationEventManager::formatIso8601(time_t timestamp, char* outBuffer, int bufferSize) {
    if (!outBuffer || bufferSize < 27) {
        return false;
    }

    struct tm* timeinfo = gmtime(&timestamp);
    if (!timeinfo) {
        return false;
    }

    // Formato UTC: 2024-02-22T17:35:45Z
    strftime(outBuffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", timeinfo);
    return true;
}

// Generate a short event id based on timestamp + random suffix. Ensures null-termination.
char* IrrigationEventManager::generateEventId(char* outBuffer, int bufferSize) {
    if (!outBuffer || bufferSize < 24) {
        return nullptr;
    }

    time_t now = time(nullptr);
    uint16_t random_part = (uint16_t)random(0, 65535);

    snprintf(outBuffer, bufferSize, "%ld-%04x", now, random_part);
    return outBuffer;
}

void IrrigationEventManager::generateNewEventId() {
    generateEventId(activeState.eventId, sizeof(activeState.eventId));
}

// Extrai o valor inteiro de uma chave JSON numerica dentro de uma linha JSONL.
// Busca "key": e le os digitos seguintes, sem depender de offsets fixos.
// Retorna true e preenche *outValue em caso de sucesso; false se chave nao encontrada.
static bool extractIntField(const String& line, const char* key, long* outValue) {
    String token = String("\"") + key + "\":";
    int keyPos = line.indexOf(token);
    if (keyPos < 0) return false;

    int valStart = keyPos + (int)token.length();
    // Pular espacos opcionais apos ':'
    while (valStart < (int)line.length() && line[valStart] == ' ') valStart++;

    bool negative = false;
    if (valStart < (int)line.length() && line[valStart] == '-') {
        negative = true;
        valStart++;
    }

    long value = 0;
    bool hasDigit = false;
    while (valStart < (int)line.length()) {
        char c = line[valStart];
        if (c < '0' || c > '9') break;
        hasDigit = true;
        value = value * 10 + (c - '0');
        valStart++;
    }

    if (!hasDigit) return false;
    *outValue = negative ? -value : value;
    return true;
}

// Extrai o valor float de uma chave JSON numerica dentro de uma linha JSONL.
// Retorna true quando a chave existe e o parse teve sucesso.
static bool extractFloatField(const String& line, const char* key, float* outValue) {
    if (!outValue) return false;

    String token = String("\"") + key + "\":";
    int keyPos = line.indexOf(token);
    if (keyPos < 0) return false;

    int valStart = keyPos + (int)token.length();
    while (valStart < (int)line.length() && line[valStart] == ' ') valStart++;

    int valEnd = valStart;
    if (valEnd < (int)line.length() && (line[valEnd] == '-' || line[valEnd] == '+')) valEnd++;

    bool hasDigit = false;
    bool dotSeen = false;
    while (valEnd < (int)line.length()) {
        char c = line[valEnd];
        if (c >= '0' && c <= '9') {
            hasDigit = true;
            valEnd++;
            continue;
        }
        if (c == '.' && !dotSeen) {
            dotSeen = true;
            valEnd++;
            continue;
        }
        break;
    }

    if (!hasDigit) return false;
    *outValue = line.substring(valStart, valEnd).toFloat();
    return true;
}

static bool parseHistoryLine(const String& line, IrrigationHistoryEntry* entry) {
    if (!entry) return false;

    // --- eventId (string) ---
    int idKeyPos = line.indexOf("\"eventId\":\"");
    if (idKeyPos < 0) return false;
    int idStart = idKeyPos + 11; // len("\"eventId\":\"") == 11
    int idEnd   = line.indexOf('"', idStart);
    if (idEnd < 0 || idEnd == idStart) return false;

    memset(entry->eventId, 0, sizeof(entry->eventId));
    strncpy(entry->eventId, line.substring(idStart, idEnd).c_str(), sizeof(entry->eventId) - 1);

    // --- campos numericos via helper robusto ---
    long v = 0;

    if (!extractIntField(line, "startAt", &v)) return false;
    entry->startAt = (time_t)v;

    if (!extractIntField(line, "endAt", &v)) return false;
    entry->endAt = (time_t)v;

    if (!extractIntField(line, "durationSec", &v)) return false;
    entry->durationSec = (int)v;

    if (!extractIntField(line, "trigger", &v)) return false;
    entry->trigger = (IrrigationTriggerType)(int)v;

    // stopReason: opcional para compatibilidade com entradas antigas sem o campo
    if (extractIntField(line, "stopReason", &v)) {
        entry->stopReason = (IrrigationStopReason)(int)v;
    } else {
        entry->stopReason = STOP_REASON_COMPLETED;
    }

    // sentToFirebase: entradas antigas sem o campo assumem false (ainda pendentes)
    entry->sentToFirebase = (line.indexOf("\"sentToFirebase\":true") >= 0);

    // Campos de fluxo + contabilidade (opcionais para compatibilidade retroativa)
    if (extractIntField(line, "totalPulses", &v)) {
        entry->totalPulses = (uint32_t)v;
    } else {
        entry->totalPulses = 0;
    }

    float fv = 0.0f;
    if (extractFloatField(line, "totalVolumeLiters", &fv)) {
        entry->totalVolumeLiters = fv;
    } else {
        entry->totalVolumeLiters = 0.0f;
    }
    if (extractFloatField(line, "avgFlowRateLpm", &fv)) {
        entry->avgFlowRateLpm = fv;
    } else {
        entry->avgFlowRateLpm = 0.0f;
    }
    if (extractFloatField(line, "totalVolumeMl", &fv)) {
        entry->totalVolumeMl = fv;
    } else {
        entry->totalVolumeMl = entry->totalVolumeLiters * 1000.0f;
    }
    if (extractFloatField(line, "nominalFlowRateMlPerMin", &fv)) {
        entry->nominalFlowRateMlPerMin = fv;
    } else {
        entry->nominalFlowRateMlPerMin = PUMP_NOMINAL_FLOW_ML_PER_MIN;
    }
    if (extractFloatField(line, "accountingVolumeMl", &fv)) {
        entry->accountingVolumeMl = fv;
    } else {
        entry->accountingVolumeMl = ((float)entry->durationSec * entry->nominalFlowRateMlPerMin) / 60.0f;
    }

    // Validacao minima: startAt invalido indica linha corrompida que nao deve
    // ser recarregada na fila (valor zero seria tratado como epoch 1970).
    if (entry->startAt < 1000000000L) {
        Serial.print("[IRRG] linha descartada: startAt invalido para eventId=");
        Serial.println(entry->eventId);
        return false;
    }

    return true;
}

bool IrrigationEventManager::loadHistory() {
    if (!LittleFS.exists(HISTORY_FILE)) {
        return false;
    }

    File f = LittleFS.open(HISTORY_FILE, "r");
    if (!f) {
        return false;
    }

    historyCount = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            continue;
        }

        IrrigationHistoryEntry entry;
        memset(&entry, 0, sizeof(entry));

        if (!parseHistoryLine(line, &entry)) {
            continue;
        }

        // Evento já confirmado pelo Firebase: não recarregar no buffer.
        // Será removido fisicamente na próxima reescrita do JSONL.
        if (entry.sentToFirebase) {
            continue;
        }

        if (historyCount < MAX_IRRIGATION_HISTORY) {
            historyBuffer[historyCount++] = entry;
        } else {
            for (int i = 1; i < MAX_IRRIGATION_HISTORY; i++) {
                historyBuffer[i - 1] = historyBuffer[i];
            }
            historyBuffer[MAX_IRRIGATION_HISTORY - 1] = entry;
        }
    }

    f.close();

    // Reescreve o JSONL contendo apenas os eventos pendentes (remove linhas
    // com sentToFirebase=true que foram filtradas acima).
    saveHistory();

    return true;
}

// Regrava o JSONL completo a partir do historyBuffer.
// Deve ser chamada sempre dentro de uma seção protegida pelo _historyMutex
// (ou antes da firebaseTask ser criada, como em loadHistory() no begin()).
void IrrigationEventManager::saveHistory() {
    if (historyCount == 0) {
        if (LittleFS.exists(HISTORY_FILE)) {
            LittleFS.remove(HISTORY_FILE);
        }
        return;
    }

    File f = LittleFS.open(HISTORY_FILE, "w");
    if (!f) {
        fwLogLine("ERROR", "IRR", "Erro ao abrir arquivo de historico para escrita");
        return;
    }

    for (int i = 0; i < historyCount; i++) {
        char line[384];
        snprintf(line, sizeof(line),
                 "{\"eventId\":\"%s\",\"startAt\":%ld,\"endAt\":%ld,"
                 "\"durationSec\":%d,\"trigger\":%d,\"stopReason\":%d,"
             "\"totalPulses\":%u,\"totalVolumeLiters\":%.3f,\"avgFlowRateLpm\":%.3f,"
             "\"totalVolumeMl\":%.1f,\"accountingVolumeMl\":%.1f,\"nominalFlowRateMlPerMin\":%.1f,"
             "\"success\":%s,\"sentToFirebase\":%s}",
                 historyBuffer[i].eventId,
                 (long)historyBuffer[i].startAt,
                 (long)historyBuffer[i].endAt,
                 historyBuffer[i].durationSec,
                 (int)historyBuffer[i].trigger,
                 (int)historyBuffer[i].stopReason,
             historyBuffer[i].totalPulses,
             historyBuffer[i].totalVolumeLiters,
             historyBuffer[i].avgFlowRateLpm,
             historyBuffer[i].totalVolumeMl,
             historyBuffer[i].accountingVolumeMl,
             historyBuffer[i].nominalFlowRateMlPerMin,
                 historyBuffer[i].isSuccess() ? "true" : "false",
                 historyBuffer[i].sentToFirebase ? "true" : "false");
        f.println(line);
    }

    f.close();
}

void IrrigationEventManager::addHistoryEvent(const IrrigationEvent& event) {
    IrrigationHistoryEntry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.eventId, event.eventId, sizeof(entry.eventId) - 1);
    entry.startAt        = event.startAt;
    entry.endAt          = event.endAt;
    entry.durationSec    = event.durationSec;
    entry.trigger        = event.trigger;
    entry.stopReason     = event.stopReason;
    entry.sentToFirebase = false; // pendente até HTTP 200 confirmado
    entry.totalPulses     = event.totalPulses;
    entry.totalVolumeLiters = event.totalVolumeLiters;
    entry.avgFlowRateLpm    = event.avgFlowRateLpm;
    entry.totalVolumeMl = event.totalVolumeMl;
    entry.accountingVolumeMl = event.accountingVolumeMl;
    entry.nominalFlowRateMlPerMin = event.nominalFlowRateMlPerMin;

    if (_historyMutex) xSemaphoreTake(_historyMutex, portMAX_DELAY);

    if (historyCount < MAX_IRRIGATION_HISTORY) {
        historyBuffer[historyCount++] = entry;
    } else {
        for (int i = 1; i < MAX_IRRIGATION_HISTORY; i++) {
            historyBuffer[i - 1] = historyBuffer[i];
        }
        historyBuffer[MAX_IRRIGATION_HISTORY - 1] = entry;
    }

    // Regrava o arquivo inteiro para manter apenas os 10 eventos mais recentes.
    saveHistory();

    if (_historyMutex) xSemaphoreGive(_historyMutex);
}

// ==================== REMOCAO DO HISTORICO (pós-confirmação Firebase) ====================

// Chamado pela firebaseTask (Core 0) após HTTP 200.
// Remove o evento do historyBuffer e regrava o JSONL sem ele.
// Thread-safe via _historyMutex.
void IrrigationEventManager::removeFromHistory(const char* eventId) {
    if (!eventId || eventId[0] == '\0') return;

    if (_historyMutex) xSemaphoreTake(_historyMutex, portMAX_DELAY);

    for (int i = 0; i < historyCount; i++) {
        if (strcmp(historyBuffer[i].eventId, eventId) == 0) {
            // Compacta o buffer removendo esta entrada
            for (int j = i; j < historyCount - 1; j++) {
                historyBuffer[j] = historyBuffer[j + 1];
            }
            memset(&historyBuffer[historyCount - 1], 0, sizeof(IrrigationHistoryEntry));
            historyCount--;
            break;
        }
    }

    // Regrava JSONL sem o evento (ou apaga arquivo se vazio)
    saveHistory();

    if (_historyMutex) xSemaphoreGive(_historyMutex);

    Serial.print("[IRRG] Evento removido do JSONL apos confirmacao Firebase: ");
    Serial.println(eventId);
}

// ==================== PERSISTÊNCIA ====================

// Load persisted active irrigation state from LittleFS; returns true if loaded.
//
// Robustez contra arquivo truncado (queda de energia durante saveState):
//   - Cada campo é extraído com helper que retorna false se a chave não for encontrada
//     ou se o resultado for vazio/inválido — qualquer falha descarta o arquivo inteiro
//     via clearState() e retorna false, evitando carregar lixo de memória.
//   - startAt é validado contra o limiar de timestamp Unix válido (>= 1 000 000 000,
//     equivalente a set/2001) — valor menor indica arquivo gravado antes do NTP
//     sincronizar ou truncamento que zerou o campo.
//   - trigger é validado contra os valores conhecidos do enum; qualquer inteiro fora
//     do range resulta em descarte do arquivo.
//   - eventId é validado quanto ao comprimento mínimo (> 0 caracteres após trim).
bool IrrigationEventManager::loadState() {
    if (!LittleFS.exists(STATE_FILE)) {
        return false;
    }

    File f = LittleFS.open(STATE_FILE, "r");
    if (!f) {
        return false;
    }

    String content = f.readString();
    f.close();

    // ── Macro local de descarte ───────────────────────────────────────────────
    // Evita repetição: limpa o arquivo corrompido, loga o motivo e sai.
    // Usamos um lambda via auto para manter o código linear sem goto.
    // (Lambda captura 'this' e é invocado imediatamente em cada ponto de falha.)
    auto discardCorrupted = [&](const char* reason) -> bool {
        Serial.print("[IRRG] loadState: arquivo de estado descartado — ");
        Serial.println(reason);
        clearState();
        return false;
    };

    // ── Validação mínima de estrutura ─────────────────────────────────────────
    if (content.length() < 20) {
        return discardCorrupted("arquivo truncado");
    }

    if (content.indexOf("\"isActive\":true") < 0) {
        // Arquivo existe mas irrigação não estava ativa — remoção normal.
        clearState();
        return false;
    }

    // ── Extração de startAt ───────────────────────────────────────────────────
    long parsedStartAt = 0;
    {
        const String key = "\"startAt\":";
        int keyPos = content.indexOf(key);
        if (keyPos < 0) return discardCorrupted("startAt ausente");

        int valStart = keyPos + key.length();
        // Pular espaços opcionais
        while (valStart < (int)content.length() && content[valStart] == ' ') valStart++;

        int valEnd = valStart;
        if (valEnd < (int)content.length() && content[valEnd] == '-') valEnd++; // sinal
        while (valEnd < (int)content.length() &&
               content[valEnd] >= '0' && content[valEnd] <= '9') valEnd++;

        if (valEnd == valStart) return discardCorrupted("startAt sem digitos");

        parsedStartAt = content.substring(valStart, valEnd).toInt();
    }

    // Timestamp válido: >= 1 000 000 000 (01/09/2001 01:46:40 UTC).
    // Valor menor indica NTP não sincronizado no momento do acionamento
    // ou arquivo gravado parcialmente — não é recuperável com utilidade.
    if (parsedStartAt < 1000000000L) {
        return discardCorrupted("startAt invalido (NTP nao sincronizado ou truncamento)");
    }

    // ── Extração de trigger ───────────────────────────────────────────────────
    int parsedTrigger = -1;
    {
        const String key = "\"trigger\":";
        int keyPos = content.indexOf(key);
        if (keyPos < 0) return discardCorrupted("trigger ausente");

        int valStart = keyPos + key.length();
        while (valStart < (int)content.length() && content[valStart] == ' ') valStart++;

        int valEnd = valStart;
        while (valEnd < (int)content.length() &&
               content[valEnd] >= '0' && content[valEnd] <= '9') valEnd++;

        if (valEnd == valStart) return discardCorrupted("trigger sem digitos");

        parsedTrigger = (int)content.substring(valStart, valEnd).toInt();
    }

    // Valida contra valores conhecidos do enum IrrigationTriggerType.
    if (parsedTrigger < TRIGGER_UNKNOWN || parsedTrigger > TRIGGER_SCHEDULE) {
        return discardCorrupted("trigger fora do range do enum");
    }

    // ── Extração de eventId ───────────────────────────────────────────────────
    char parsedEventId[24] = {0};
    {
        const String key = "\"eventId\":\"";
        int keyPos = content.indexOf(key);
        if (keyPos < 0) return discardCorrupted("eventId ausente");

        int idStart = keyPos + key.length();
        int idEnd   = content.indexOf("\"", idStart);
        if (idEnd < 0 || idEnd == idStart) return discardCorrupted("eventId vazio ou sem fechamento");

        int idLen = idEnd - idStart;
        if (idLen >= (int)sizeof(parsedEventId)) {
            return discardCorrupted("eventId excede tamanho maximo");
        }

        content.substring(idStart, idEnd).toCharArray(parsedEventId, sizeof(parsedEventId));
        if (parsedEventId[0] == '\0') return discardCorrupted("eventId string vazia");
    }

    // ── Todos os campos válidos: aplica ao estado ativo ───────────────────────
    activeState.isActive = true;
    activeState.startAt  = (time_t)parsedStartAt;
    activeState.trigger  = (IrrigationTriggerType)parsedTrigger;
    strncpy(activeState.eventId, parsedEventId, sizeof(activeState.eventId) - 1);
    activeState.eventId[sizeof(activeState.eventId) - 1] = '\0';

    Serial.print("🔄 Estado de irrigacao recuperado: id=");
    Serial.print(activeState.eventId);
    Serial.print(" | iniciado ha ");
    Serial.print((long)(time(nullptr) - activeState.startAt));
    Serial.println("s");

    return true;
}

// Persist current active irrigation state to LittleFS. Called on start/end to aid recovery.
void IrrigationEventManager::saveState() {
    if (!activeState.isActive) {
        clearState();
        return;
    }

    // Criar JSON simples para persistência
    String json = "{\"isActive\":true,\"startAt\":";
    json += (long)activeState.startAt;
    json += ",\"trigger\":";
    json += (int)activeState.trigger;
    json += ",\"eventId\":\"";
    json += activeState.eventId;
    json += "\"}";

    File f = LittleFS.open(STATE_FILE, "w");
    if (!f) {
        fwLogLine("ERROR", "IRR", "Erro ao abrir arquivo de estado para escrita");
        return;
    }

    f.print(json);
    f.close();
}

void IrrigationEventManager::clearState() {
    if (LittleFS.exists(STATE_FILE)) {
        LittleFS.remove(STATE_FILE);
    }
}

// ==================== CONTROLE DE IDs ENVIADOS ====================

bool IrrigationEventManager::isAlreadySent(const char* eventId) const {
    if (!eventId) return false;
    for (int i = 0; i < sentIdsCount; i++) {
        if (strcmp(sentIds[i], eventId) == 0) return true;
    }
    return false;
}

void IrrigationEventManager::markIdAsSent(const char* eventId) {
    if (!eventId || isAlreadySent(eventId)) return;
    if (sentIdsCount < MAX_SENT_IDS) {
        strncpy(sentIds[sentIdsCount], eventId, sizeof(sentIds[0]) - 1);
        sentIds[sentIdsCount][sizeof(sentIds[0]) - 1] = '\0';
        sentIdsCount++;
    } else {
        // Desloca para liberar espaço (descarta o mais antigo)
        for (int i = 1; i < MAX_SENT_IDS; i++) {
            memcpy(sentIds[i - 1], sentIds[i], sizeof(sentIds[0]));
        }
        strncpy(sentIds[MAX_SENT_IDS - 1], eventId, sizeof(sentIds[0]) - 1);
        sentIds[MAX_SENT_IDS - 1][sizeof(sentIds[0]) - 1] = '\0';
    }
}
