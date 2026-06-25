// Responsabilidade: implementar controle da bomba e persistencia de configuracoes locais.
// O que faz: liga/desliga atuador, salva modo e parametros na NVS, armazena/remocao
// de agendamentos e verifica disparos por horario com janela de tolerancia.

#include "actuator_manager.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static Preferences preferences;
static const char* SCHEDULES_STORAGE_KEY = "schedules";
static const char* KEY_MODO_AUTO = "modo_auto";
static const char* KEY_DURACAO = "duracao_s";
static const char* KEY_THRESHOLD = "threshold";
static const char* KEY_COOLDOWN = "cooldown_s";

static int countUsedSchedules(const IrrigationSchedule* schedules, int maxSchedules) {
    int total = 0;
    for (int i = 0; i < maxSchedules; i++) {
        if (schedules[i].used) {
            total++;
        }
    }
    return total;
}

static bool dayMatchesMask(int weekday, uint8_t mask) {
    if (weekday < 0 || weekday > 6) {
        return false;
    }

    return (mask & (1U << weekday)) != 0;
}

static bool isDueWithGrace(const IrrigationSchedule& item, time_t nowTs, const tm& nowTm, time_t* outScheduledTs) {
    tm scheduledTm = nowTm;
    scheduledTm.tm_hour = item.hour;
    scheduledTm.tm_min = item.minute;
    scheduledTm.tm_sec = 0;

    time_t scheduledTs = mktime(&scheduledTm);
    if (scheduledTs <= 0) {
        return false;
    }

    const time_t triggerGraceSeconds = 90;
    if (nowTs < scheduledTs || nowTs > (scheduledTs + triggerGraceSeconds)) {
        return false;
    }

    *outScheduledTs = scheduledTs;
    return true;
}

// Initialize actuator pins and load persisted configuration (Preferences/NVS).
// Keeps defaults when stored values invalid. Called once from `setup()`.
void ActuatorManager::begin() {
    pinMode(PUMP_PIN, OUTPUT);
    digitalWrite(PUMP_PIN, PUMP_OFF);

    pinMode(PUMP_LED, OUTPUT);
    digitalWrite(PUMP_LED, PUMP_LED_OFF);

    preferences.begin("irrigahome", false);
    modoAuto = preferences.getBool(KEY_MODO_AUTO, false);
    duracaoSegundos = preferences.getInt(KEY_DURACAO, DURACAO_IRRIGACAO_PADRAO);
    thresholdUmidade = preferences.getInt(KEY_THRESHOLD, THRESHOLD_UMIDADE_PADRAO);
    cooldownSegundos = preferences.getInt(KEY_COOLDOWN, COOLDOWN_IRRIGACAO_PADRAO);

    if (duracaoSegundos <= 0) {
        duracaoSegundos = DURACAO_IRRIGACAO_PADRAO;
    }

    if (thresholdUmidade <= 0) {
        thresholdUmidade = THRESHOLD_UMIDADE_PADRAO;
    }

    if (cooldownSegundos <= 0) {
        cooldownSegundos = COOLDOWN_IRRIGACAO_PADRAO;
    }

    loadSchedulesFromStorage();

    Serial.print("⚙️ Modo carregado da memoria: ");
    Serial.println(modoAuto ? "AUTO" : "MANUAL");
    Serial.print("⚙️ Duracao carregada: ");
    Serial.print(duracaoSegundos);
    Serial.println("s");
    Serial.print("⚙️ Threshold carregado: ");
    Serial.println(thresholdUmidade);
    Serial.print("⚙️ Cooldown carregado: ");
    Serial.print(cooldownSegundos);
    Serial.println("s");
}

// Load persisted schedules from Preferences. Validates stored size and resets to empty on mismatch.
void ActuatorManager::loadSchedulesFromStorage() {
    size_t expectedSize = sizeof(schedules);
    size_t storedSize = preferences.getBytesLength(SCHEDULES_STORAGE_KEY);

    if (storedSize == expectedSize) {
        preferences.getBytes(SCHEDULES_STORAGE_KEY, schedules, expectedSize);
        int total = countUsedSchedules(schedules, MAX_SCHEDULES);
        Serial.print("[SCHEDULE] carregados da memoria: ");
        Serial.println(total);
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            if (!schedules[i].used) continue;
            Serial.println("-----------------------------");
            Serial.print("[SCHEDULE] slot=");      Serial.println(i);
            Serial.print("[SCHEDULE] id=");        Serial.println(schedules[i].id);
            Serial.print("[SCHEDULE] ativo=");     Serial.println(schedules[i].ativo ? "true" : "false");
            Serial.print("[SCHEDULE] hora=");
            if (schedules[i].hour < 10) Serial.print("0");
            Serial.print(schedules[i].hour);
            Serial.print(":");
            if (schedules[i].minute < 10) Serial.print("0");
            Serial.println(schedules[i].minute);
            Serial.print("[SCHEDULE] duracao=");   Serial.print(schedules[i].durationSeconds); Serial.println("s");
            Serial.print("[SCHEDULE] diasMask=");  Serial.println(schedules[i].diasMask, BIN);
            Serial.print("[SCHEDULE] createdAt="); Serial.println(schedules[i].createdAt[0] != '\0' ? schedules[i].createdAt : "n/a");
        }
        Serial.println("-----------------------------");
        return;
    }

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = IrrigationSchedule();
    }

    if (storedSize > 0) {
        Serial.println("[SCHEDULE] dados persistidos ignorados por tamanho invalido");
    }
}

// Persist current schedules into Preferences. Non-blocking small write; logs on failure.
void ActuatorManager::saveSchedulesToStorage() {
    size_t bytesWritten = preferences.putBytes(SCHEDULES_STORAGE_KEY, schedules, sizeof(schedules));
    if (bytesWritten != sizeof(schedules)) {
        Serial.println("[SCHEDULE] falha ao persistir agendamentos");
    }
}

// Turn pump ON: set physical pins, update state and log event.
void ActuatorManager::ligar() {

    digitalWrite(PUMP_PIN, PUMP_ON);
    digitalWrite(PUMP_LED, PUMP_LED_ON);

    pumpStartedAtMs = millis();
    bombaLigada = true;
    Serial.println("🚿 Bomba ON");
}

// Turn pump OFF: clear pins, update state and log event.
void ActuatorManager::desligar() {
    digitalWrite(PUMP_PIN, PUMP_OFF);
    digitalWrite(PUMP_LED, PUMP_LED_OFF);

    pumpStartedAtMs = 0;
    bombaLigada = false;
    Serial.println("🛑 Bomba OFF");
}

bool ActuatorManager::status() {
    return bombaLigada;
}

bool ActuatorManager::isLigado() {
    return status();
}

void ActuatorManager::setModoAuto(bool enabled) {
    bool changed = (modoAuto != enabled);
    modoAuto = enabled;
    preferences.putBool(KEY_MODO_AUTO, modoAuto);

    if (enabled) {
        lastAutoTrigger = millis() - ((unsigned long)cooldownSegundos * 1000UL);
    }

    if (changed) {
        Serial.print("🧠 Modo salvo na memoria: ");
        Serial.println(modoAuto ? "AUTO" : "MANUAL");
    }
}

bool ActuatorManager::isModoAuto() {
    return modoAuto;
}

void ActuatorManager::setDuracaoSegundos(int seconds) {
    if (seconds <= 0) {
        duracaoSegundos = DURACAO_IRRIGACAO_PADRAO;
    } else if (seconds > DURACAO_IRRIGACAO_MAXIMA) {
        duracaoSegundos = DURACAO_IRRIGACAO_MAXIMA;
    } else {
        duracaoSegundos = seconds;
    }
    _nvsDirty = true; // consolidado em flushConfig()
}

int ActuatorManager::getDuracaoSegundos() {
    return duracaoSegundos;
}

void ActuatorManager::setThresholdUmidade(int threshold) {
    thresholdUmidade = threshold > 0 ? threshold : THRESHOLD_UMIDADE_PADRAO;
    _nvsDirty = true; // consolidado em flushConfig()
}

int ActuatorManager::getThresholdUmidade() {
    return thresholdUmidade;
}

void ActuatorManager::setCooldownSegundos(int cooldown) {
    cooldownSegundos = cooldown > 0 ? cooldown : COOLDOWN_IRRIGACAO_PADRAO;
    _nvsDirty = true; // consolidado em flushConfig()
}

int ActuatorManager::getCooldownSegundos() {
    return cooldownSegundos;
}

// Persiste duracao, threshold e cooldown em uma unica passagem pela NVS.
// Deve ser chamado pelo caller (mqtt_manager) ao final do processamento de
// um comando setConfig, nunca dentro dos setters individuais.
// Nao-op se nenhum setter foi chamado desde o ultimo flush (_nvsDirty == false).
void ActuatorManager::flushConfig() {
    if (!_nvsDirty) {
        return;
    }
    preferences.putInt(KEY_DURACAO,    duracaoSegundos);
    preferences.putInt(KEY_THRESHOLD,  thresholdUmidade);
    preferences.putInt(KEY_COOLDOWN,   cooldownSegundos);
    _nvsDirty = false;
    Serial.println("[CONFIG] NVS atualizada (batch: duracao/threshold/cooldown)");
}

void ActuatorManager::setLastAutoTrigger(unsigned long time) {
    lastAutoTrigger = time;
}

unsigned long ActuatorManager::getLastAutoTrigger() {
    return lastAutoTrigger;
}

void ActuatorManager::setLastScheduleTrigger(unsigned long time) {
    lastScheduleTrigger = time;
}

unsigned long ActuatorManager::getLastScheduleTrigger() {
    return lastScheduleTrigger;
}

void ActuatorManager::setActiveUntil(unsigned long time) {
    activeUntil = time;
}

unsigned long ActuatorManager::getActiveUntil() {
    return activeUntil;
}

unsigned long ActuatorManager::getPumpStartedAtMs() {
    return pumpStartedAtMs;
}

int ActuatorManager::getRemainingSeconds(unsigned long now) {
    if (activeUntil <= now) {
        return 0;
    }

    unsigned long remainingMillis = activeUntil - now;
    return (int)((remainingMillis + 999UL) / 1000UL);
}

int ActuatorManager::getScheduleCount() {
    return countUsedSchedules(schedules, MAX_SCHEDULES);
}

const char* ActuatorManager::getScheduleId(int index) const {
    if (index < 0 || index >= MAX_SCHEDULES) return nullptr;
    if (!schedules[index].used) return nullptr;
    return schedules[index].id;
}

void ActuatorManager::clearSchedules() {
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        schedules[i] = IrrigationSchedule();
    }

    preferences.remove(SCHEDULES_STORAGE_KEY);
    Serial.println("[SCHEDULE] todos os agendamentos foram limpos da memoria");
}

// Insert or update a schedule by ID. Validates inputs and persists on success.
bool ActuatorManager::upsertSchedule(const char* id, bool ativo, uint8_t diasMask, int hour, int minute, int durationSeconds, const char* createdAtIso) {
    if (id == nullptr || id[0] == '\0') {
        return false;
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || durationSeconds <= 0) {
        return false;
    }
    if (durationSeconds > DURACAO_IRRIGACAO_MAXIMA) {
        durationSeconds = DURACAO_IRRIGACAO_MAXIMA;
    }

    int slot = -1;
    bool existingSchedule = false;
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (schedules[i].used && strcmp(schedules[i].id, id) == 0) {
            slot = i;
            existingSchedule = true;
            break;
        }

        if (!schedules[i].used && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        Serial.print("[SCHEDULE] ERRO: capacidade maxima atingida (");
        Serial.print(MAX_SCHEDULES);
        Serial.println(" agendas). Agenda nao armazenada. Sinalize o app via topico de status.");
        return false;
    }

    schedules[slot].used = true;
    strncpy(schedules[slot].id, id, sizeof(schedules[slot].id) - 1);
    schedules[slot].id[sizeof(schedules[slot].id) - 1] = '\0';

    if (createdAtIso != nullptr && createdAtIso[0] != '\0') {
        strncpy(schedules[slot].createdAt, createdAtIso, sizeof(schedules[slot].createdAt) - 1);
        schedules[slot].createdAt[sizeof(schedules[slot].createdAt) - 1] = '\0';
    } else if (!existingSchedule) {
        schedules[slot].createdAt[0] = '\0';
    }

    schedules[slot].ativo = ativo;
    schedules[slot].diasMask = diasMask;
    schedules[slot].hour = hour;
    schedules[slot].minute = minute;
    schedules[slot].durationSeconds = durationSeconds;
    saveSchedulesToStorage();

    return true;
}

bool ActuatorManager::removeSchedule(const char* id) {
    if (id == nullptr || id[0] == '\0') {
        return false;
    }

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!schedules[i].used) {
            continue;
        }

        if (strcmp(schedules[i].id, id) == 0) {
            schedules[i] = IrrigationSchedule();
            saveSchedulesToStorage();
            return true;
        }
    }

    return false;
}

bool ActuatorManager::getStoredScheduleAtivo(const char* id, bool* outAtivo) const {
    if (id == nullptr || id[0] == '\0' || outAtivo == nullptr) {
        return false;
    }
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (schedules[i].used && strcmp(schedules[i].id, id) == 0) {
            *outAtivo = schedules[i].ativo;
            return true;
        }
    }
    return false;
}

// Check if any schedule is due considering day mask and grace window.
// If due, updates lastTriggerAt to prevent duplicates and returns true with schedule id.
bool ActuatorManager::checkDueSchedule(time_t nowTs, int* outDurationSeconds, const char** outScheduleId) {
    if (outDurationSeconds == nullptr || outScheduleId == nullptr || nowTs <= 0) {
        return false;
    }

    struct tm nowTm;
    localtime_r(&nowTs, &nowTm);

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        IrrigationSchedule& item = schedules[i];

        if (!item.used || !item.ativo) {
            continue;
        }

        if (!dayMatchesMask(nowTm.tm_wday, item.diasMask)) {
            continue;
        }

        time_t scheduledTs = 0;
        if (!isDueWithGrace(item, nowTs, nowTm, &scheduledTs)) {
            continue;
        }

        if (item.lastTriggerAt >= scheduledTs) {
            continue;
        }

        item.lastTriggerAt = nowTs;
        *outDurationSeconds = item.durationSeconds;
        *outScheduleId = item.id;
        return true;
    }

    return false;
}

// ==================== RASTREAMENTO DE TRIGGER ====================

void ActuatorManager::setCurrentTrigger(IrrigationTriggerType trigger) {
    currentTrigger = trigger;
}

IrrigationTriggerType ActuatorManager::getCurrentTrigger() {
    return currentTrigger;
}