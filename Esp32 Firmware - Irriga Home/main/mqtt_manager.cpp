// Responsabilidade: implementar integracao MQTT com telemetria, comandos e agendamentos.
// O que faz: conecta ao broker, publica status/telemetria por delta, processa comandos
// (modo, irrigacao, configuracao) e aplica eventos de schedule recebidos.
#include "mqtt_manager.h"
#include "config.h"
#include "sensor_manager.h"
#include "actuator_manager.h"
#include "irrigation_event_manager.h"
#include "flow_meter_manager.h"
#include "firmware_logger.h"
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>

WiFiClientSecure espClient;
PubSubClient client(espClient);

String clientId;
static char cachedDeviceId[32] = {0};
static char cachedTelemetryTopic[64] = {0};
static char cachedStatusTopic[64] = {0};
static char cachedCommandsTopic[64] = {0};
static const char* HISTORY_COLLECTION_NAME = "events";
static char pendingStatusPayload[160] = {0};
static bool pendingStatusPublish = false;

// Controle delta
static int lastSolo = -1;
static int lastTemp = -1000;
static int lastUmidAr = -1000;
static bool lastNivelAgua = true;
static bool lastBomba = false;
static const unsigned long TELEMETRY_STABLE_WINDOW_MS = 3000UL;
static bool pendingSensorStability = false;
static unsigned long sensorStabilitySince = 0;
static int candidateSolo = -1;
static int candidateTemp = -1000;
static int candidateUmidAr = -1000;

static unsigned long lastReconnectAttempt = 0;
static unsigned long lastHeartbeat = 0;
static unsigned long reconnectBackoffMs = 5000UL;
static bool mqttWasConnected = false;
static MqttManager* mqttManagerInstance = nullptr;
// Ponteiro para o IrrigationEventManager injetado em begin().
// Usado pelos helpers estaticos (callback, connectMQTT) que nao recebem 'this'.
static IrrigationEventManager* g_eventMgr = nullptr;

// Conexão assíncrona para evitar bloqueio do loop principal (task watchdog)
// Precisa ser visível entre Core 0 e Core 1 sem cache agressivo.
static volatile bool connectTaskRunning = false;
static volatile bool mqttConnectingInProgress = false;  // Flag para indicar que client.connect() está em andamento
// Sinaliza ao loop() (Core 1) que a conexão foi bem-sucedida e ele deve
// executar subscribe/publish/reset-de-estado em seu próprio contexto.
// Nunca acessar sensores/atuador/client a partir do Core 0 diretamente.
static volatile bool mqttJustConnected = false;
static void mqttConnectTask(void* pvParameters);
// TLS handshake do WiFiClientSecure pode consumir stack acima de 16 KB.
// 32 KB evita overflow intermitente e corrupção de heap durante connect().
static constexpr uint32_t MQTT_CONNECT_TASK_STACK_BYTES = 32768;
static portMUX_TYPE mqttConnectMux = portMUX_INITIALIZER_UNLOCKED;

static void refreshMqttTopicCache() {
    String normalizedDeviceId = getDeviceIdFromMac();
    normalizedDeviceId.toCharArray(cachedDeviceId, sizeof(cachedDeviceId));
    snprintf(cachedTelemetryTopic, sizeof(cachedTelemetryTopic), "irrigahome/%s/telemetry", cachedDeviceId);
    snprintf(cachedStatusTopic, sizeof(cachedStatusTopic), "irrigahome/%s/status", cachedDeviceId);
    snprintf(cachedCommandsTopic, sizeof(cachedCommandsTopic), "irrigahome/%s/commands", cachedDeviceId);
}

static bool publishMessage(const char* topic, const char* payload, bool retain);

static void queuePendingStatusPublish(const char* payload) {
    if (payload == nullptr || payload[0] == '\0') {
        return;
    }

    taskENTER_CRITICAL(&mqttConnectMux);
    strncpy(pendingStatusPayload, payload, sizeof(pendingStatusPayload) - 1);
    pendingStatusPayload[sizeof(pendingStatusPayload) - 1] = '\0';
    pendingStatusPublish = true;
    taskEXIT_CRITICAL(&mqttConnectMux);
}

static void flushPendingStatusPublish() {
    taskENTER_CRITICAL(&mqttConnectMux);
    bool shouldFlush = pendingStatusPublish && client.connected();
    if (!shouldFlush) {
        taskEXIT_CRITICAL(&mqttConnectMux);
        return;
    }
    
    // Copiar payload antes de liberar o mutex para evitar TOCTOU
    char payload[160];
    strncpy(payload, pendingStatusPayload, sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = '\0';
    taskEXIT_CRITICAL(&mqttConnectMux);

    if (publishMessage(cachedStatusTopic, payload, false)) {
        taskENTER_CRITICAL(&mqttConnectMux);
        pendingStatusPublish = false;
        pendingStatusPayload[0] = '\0';
        taskEXIT_CRITICAL(&mqttConnectMux);
    }
}

// Referencias externas
extern SensorManager sensores;
extern ActuatorManager atuador;
// IrrigationEventManager e' injetado via MqttManager::begin() — sem extern.

// ==================== FREERTOS: FILA E TASK FIREBASE ====================

// Estrutura que trafega pela fila — todos os campos copiados por valor
// para evitar acesso a memoria liberada quando a task executar.
//
struct FirebaseEventPayload {
    char eventId[24];
    char startAtIso[32];
    char endAtIso[32];
    int  durationSec;
    char triggerType[16];
    char stopReason[16];
    bool success;          // true somente quando stopReason == STOP_REASON_COMPLETED
    int  soilHumidity;
    int  airHumidity;
    float temperature;
    char waterLevel[10];
    // Flow sensor
    uint32_t totalPulses;
    float totalVolumeLiters;
    float avgFlowRateLpm;
    float totalVolumeMl;
    float accountingVolumeMl;
    float nominalFlowRateMlPerMin;
    bool flowDetected;
    char flowStatus[16];
};

static QueueHandle_t firebaseQueue = nullptr;

// Task dedicada ao envio HTTP para o Firebase.
// Roda no Core 0 (protocolo), separado do loop principal (Core 1),
// com stack generosa para TLS + HTTPClient.
//
static void firebaseTask(void* pvParameters) {
    FirebaseEventPayload payload;

    // NAO registrar esta task no Task WDT (esp_task_wdt_add).
    // http.GET() + TLS podem bloquear por varios segundos sem retorno ao codigo;
    // com registro, o TWDT exige esp_task_wdt_reset() dentro do timeout global
    // (ex.: 8 s) e dispara panic mesmo com o loop principal saudavel no Core 1.
    // O watchdog continua sendo alimentado apenas pelo loopTask (main.ino).

    for (;;) {
        if (xQueueReceive(firebaseQueue, &payload, portMAX_DELAY) == pdTRUE) {

            if (WiFi.status() != WL_CONNECTED) {
                // Sem WiFi, o envio ao Firebase fica aguardando um novo ciclo.
                Serial.println("[FIREBASE] sem WiFi, envio adiado (sera retentado apos reconexao)");
                continue;
            }

            String firebaseUrl = String(FIREBASE_PROJECT_URL) + "/saveIrrigationEvent";
            String url = firebaseUrl;
            url += "?deviceId=" + getDeviceIdFromMac();
            url += "&historyCollection=" + String(HISTORY_COLLECTION_NAME);
            url += "&eventId="  + String(payload.eventId);
            url += "&startAt="  + String(payload.startAtIso);

            if (payload.endAtIso[0] != '\0') {
                url += "&endAt=" + String(payload.endAtIso);
            }

            url += "&durationSec=" + String(payload.durationSec);
            url += "&trigger="     + String(payload.triggerType);
            url += "&stopReason="  + String(payload.stopReason);
            url += "&success="     + String(payload.success ? "true" : "false");

            if (payload.soilHumidity > 0) {
                url += "&soilHumidity=" + String(payload.soilHumidity);
            }
            if (payload.airHumidity > 0) {
                url += "&airHumidity=" + String(payload.airHumidity);
            }
            if (payload.temperature > 0.0f) {
                url += "&temperature=" + String(payload.temperature, 2);
            }
            // waterLevel é sempre enviado — "Vazio" é informação crítica para o histórico
            url += "&waterLevel=" + String(payload.waterLevel);

            // Campos contábeis sempre enviados
            url += "&accountingVolumeMl=" + String(payload.accountingVolumeMl, 1);
            url += "&nominalFlowRateMlPerMin=" + String(payload.nominalFlowRateMlPerMin, 1);

            // Flow sensor (quando medido)
            if (payload.totalPulses > 0 || payload.totalVolumeLiters > 0.0f || payload.totalVolumeMl > 0.0f) {
                url += "&totalPulses=" + String(payload.totalPulses);
                url += "&totalVolumeLiters=" + String(payload.totalVolumeLiters, 3);
                url += "&avgFlowRateLpm=" + String(payload.avgFlowRateLpm, 3);
                url += "&totalVolumeMl=" + String(payload.totalVolumeMl, 1);
                url += "&flowDetected=" + String(payload.flowDetected ? "true" : "false");
                if (payload.flowStatus[0] != '\0') {
                    url += "&flowStatus=" + String(payload.flowStatus);
                }
            }

            Serial.print("[FIREBASE] Enviando evento: ");
            Serial.println(url);

            // WiFiClientSecure e HTTPClient sao criados dentro da task
            // para evitar conflitos com o cliente TLS do MQTT (espClient).
            WiFiClientSecure httpsClient;
            httpsClient.setInsecure(); // Para desenvolvimento
            httpsClient.setTimeout(10);

            HTTPClient http;
            http.begin(httpsClient, url);
            // Limite de espera da camada HTTP (TLS pode somar tempo antes disso).
            http.setTimeout(5000);

            int httpCode = http.GET();

            if (httpCode > 0) {
                if (httpCode == 200) {
                    fwLogf("INFO", "FIREBASE", "Evento gravado: %s", payload.eventId);
                    // Remove do JSONL — única ação que impede reenvio após reboot.
                    if (g_eventMgr) {
                        g_eventMgr->removeFromHistory(payload.eventId);
                    }
                } else {
                    fwLogf("WARN", "FIREBASE", "Codigo HTTP: %d", httpCode);
                    String response = http.getString();
                    fwLogf("WARN", "FIREBASE", "Resposta: %s", response.c_str());
                }
            } else {
                fwLogf("ERROR", "FIREBASE", "Erro HTTP: %s", http.errorToString(httpCode).c_str());
            }

            http.end();

            // Yield explicito para o scheduler apos operacao longa
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// Encapsula o envio real: apenas copia o payload para a fila e retorna imediatamente.
// O loop principal nao bloqueia; a firebaseTask cuida do HTTP de forma assincrona.
//
// THREAD-SAFETY: getStopReasonString() e' chamado aqui, no contexto do loop principal
// (Core 1), antes de qualquer enfileiramento. O resultado — uma string literal estatica —
// e' copiado por valor para FirebaseEventPayload. A firebaseTask (Core 0) nunca acessa
// g_eventMgr diretamente; opera apenas sobre a copia local do payload.
void MqttManager::sendIrrigationEventToFirebase(const char* eventId,
                                                 const char* startAtIso,
                                                 const char* endAtIso,
                                                 int durationSec,
                                                 const char* triggerType,
                                                 IrrigationStopReason stopReason,
                                                 int soilHumidity,
                                                 int airHumidity,
                                                 float temperature,
                                                 bool waterLevel,
                                                 uint32_t totalPulses,
                                                 float totalVolumeLiters,
                                                 float avgFlowRateLpm,
                                                 bool flowDetected,
                                                 const char* flowStatus,
                                                 float totalVolumeMl,
                                                 float accountingVolumeMl,
                                                 float nominalFlowRateMlPerMin) {
    if (!eventId || !startAtIso || !triggerType) {
        Serial.println("[FIREBASE] parametros invalidos");
        return;
    }

    if (firebaseQueue == nullptr) {
        Serial.println("[FIREBASE] fila nao inicializada");
        return;
    }

    if (g_eventMgr == nullptr) {
        Serial.println("[FIREBASE] eventMgr nao injetado");
        return;
    }

    FirebaseEventPayload payload = {};
    strncpy(payload.eventId,    eventId,    sizeof(payload.eventId)    - 1);
    strncpy(payload.startAtIso, startAtIso, sizeof(payload.startAtIso) - 1);
    strncpy(payload.endAtIso,   endAtIso ? endAtIso : "", sizeof(payload.endAtIso) - 1);
    strncpy(payload.triggerType, triggerType, sizeof(payload.triggerType) - 1);
    // getStopReasonString() retorna literal estatico — seguro copiar aqui (Core 1)
    // sem que a firebaseTask (Core 0) precise acessar g_eventMgr.
    strncpy(payload.stopReason,
            g_eventMgr->getStopReasonString(stopReason),
            sizeof(payload.stopReason) - 1);
    payload.success      = (stopReason == STOP_REASON_COMPLETED);
    payload.durationSec  = durationSec;
    payload.soilHumidity = soilHumidity;
    payload.airHumidity  = airHumidity;
    payload.temperature  = temperature;
    payload.totalPulses = totalPulses;
    payload.totalVolumeLiters = totalVolumeLiters;
    payload.avgFlowRateLpm = avgFlowRateLpm;
    payload.totalVolumeMl = totalVolumeMl;
    payload.accountingVolumeMl = accountingVolumeMl;
    payload.nominalFlowRateMlPerMin = nominalFlowRateMlPerMin;
    payload.flowDetected = flowDetected;
    strncpy(payload.flowStatus, flowStatus ? flowStatus : "", sizeof(payload.flowStatus) - 1);
    strncpy(payload.waterLevel, waterLevel ? "Cheio" : "Vazio", sizeof(payload.waterLevel) - 1);
    payload.waterLevel[sizeof(payload.waterLevel) - 1] = '\0';
    
    // DEBUG: Log dos dados de vazão sendo enfileirados
    Serial.print("[FIREBASE-QUEUE] Evento enfileirado - ID: ");
    Serial.print(payload.eventId);
    Serial.print(" | Pulsos: ");
    Serial.print(payload.totalPulses);
    Serial.print(" | Volume: ");
    Serial.print(payload.totalVolumeLiters, 3);
    Serial.print("L (" );
    Serial.print(payload.totalVolumeMl, 1);
    Serial.print(" mL) | Contabil: ");
    Serial.print(payload.accountingVolumeMl, 1);
    Serial.print(" mL @ ");
    Serial.print(payload.nominalFlowRateMlPerMin, 1);
    Serial.print(" mL/min | Status: ");
    Serial.println(payload.flowStatus);

    // xQueueSend nao bloqueia (timeout 0): se a fila estiver cheia, loga e descarta.
    if (xQueueSend(firebaseQueue, &payload, 0) != pdTRUE) {
        fwLogLine("WARN", "FIREBASE", "Fila cheia; evento descartado");
    }
}

// ==================== HELPERS DE PARSE ====================

static bool publishMessage(const char* topic, const char* payload, bool retain) {
    bool ok = client.publish(topic, payload, retain);
    
    if (!ok) {
        Serial.println("[MQTT] falha ao publicar");
    }
    return ok;
}

static bool isLikelyJsonObject(const String& msg) {
    int first = 0;
    int last = msg.length() - 1;
    while (first <= last && (msg[first] == ' ' || msg[first] == '\t' || msg[first] == '\r' || msg[first] == '\n')) first++;
    while (last >= first && (msg[last] == ' ' || msg[last] == '\t' || msg[last] == '\r' || msg[last] == '\n')) last--;
    if (first > last || msg[first] != '{' || msg[last] != '}') return false;

    bool inString = false;
    bool escape = false;
    int braceDepth = 0;
    int bracketDepth = 0;
    for (int i = first; i <= last; i++) {
        char c = msg[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '{') braceDepth++;
        else if (c == '}') braceDepth--;
        else if (c == '[') bracketDepth++;
        else if (c == ']') bracketDepth--;

        if (braceDepth < 0 || bracketDepth < 0) return false;
    }

    return !inString && braceDepth == 0 && bracketDepth == 0;
}

// Localiza a PRIMEIRA ocorrência de `key` como chave JSON válida.
// Ao contrário da versão anterior (findUniqueKey), não rejeita payloads com
// chaves duplicadas — comportamento alinhado com a especificação ECMA-404 e
// com o que os parsers reais fazem (primeira ocorrência vence).
// Retorna true e preenche *outKeyPos com o índice do caractere '"' inicial da chave.
static bool findFirstKey(const String& msg, const char* key, int* outKeyPos) {
    String token = String("\"") + key + "\"";
    int searchFrom = 0;
    const int len = msg.length();

    while (true) {
        int pos = msg.indexOf(token, searchFrom);
        if (pos < 0) return false;

        // Verificar que o caractere após a chave + aspas é ':' (ignora valores que
        // usam o mesmo texto como string e não como chave).
        int afterToken = pos + (int)token.length();
        while (afterToken < len &&
               (msg[afterToken] == ' ' || msg[afterToken] == '\t' ||
                msg[afterToken] == '\r' || msg[afterToken] == '\n')) {
            afterToken++;
        }
        if (afterToken < len && msg[afterToken] == ':') {
            if (outKeyPos) *outKeyPos = pos;
            return true;
        }
        searchFrom = pos + 1;
    }
}

static bool parseAction(const String& msg, String& outAction) {
    int keyPos = -1;
    if (!findFirstKey(msg, "action", &keyPos)) return false;
    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return false;
    int firstQuote = msg.indexOf('"', colonPos + 1);
    if (firstQuote < 0) return false;
    // Lê até a próxima aspa não escapada por barra invertida
    outAction = "";
    bool esc = false;
    for (int i = firstQuote + 1; i < (int)msg.length(); i++) {
        char c = msg[i];
        if (esc) { esc = false; outAction += c; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        outAction += c;
    }
    return outAction.length() > 0;
}

static bool hasIrrigateAction(const String& action) { return action == "irrigate"; }
static bool hasSetConfigAction(const String& action) { return action == "setConfig"; }
static bool hasSetModeAction(const String& action) { return action == "setMode"; }

static int parseDuration(const String& msg) {
    int keyPos = -1;
    if (!findFirstKey(msg, "duration", &keyPos)) return 0;

    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return 0;

    int i = colonPos + 1;
    while (i < (int)msg.length() && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '"')) {
        i++;
    }

    bool negative = false;
    if (i < (int)msg.length() && msg[i] == '-') {
        negative = true;
        i++;
    }

    long value = 0;
    bool hasDigit = false;
    while (i < (int)msg.length() && msg[i] >= '0' && msg[i] <= '9') {
        hasDigit = true;
        value = (value * 10) + (msg[i] - '0');
        i++;
    }

    if (!hasDigit) return 0;
    if (negative) value = -value;

    if (value <= 0) return 0;
    if (value > DURACAO_IRRIGACAO_MAXIMA) {
        Serial.print("[MQTT] duration acima do maximo, limitando para ");
        Serial.println(DURACAO_IRRIGACAO_MAXIMA);
        return DURACAO_IRRIGACAO_MAXIMA;
    }
    return (int)value;
}

static bool parseModeAuto(const String& msg, bool* hasMode) {
    int keyPos = -1;
    if (!findFirstKey(msg, "mode", &keyPos)) {
        *hasMode = false;
        return false;
    }

    *hasMode = true;
    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return false;
    int firstQuote = msg.indexOf('"', colonPos + 1);
    if (firstQuote < 0) return false;
    int secondQuote = msg.indexOf('"', firstQuote + 1);
    if (secondQuote < 0) return false;
    String modeValue = msg.substring(firstQuote + 1, secondQuote);
    modeValue.toLowerCase();
    return modeValue == "auto" || modeValue == "automatic";
}

static int parseThreshold(const String& msg) {
    int keyPos = -1;
    if (!findFirstKey(msg, "threshold", &keyPos)) return 0;

    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return 0;

    int i = colonPos + 1;
    while (i < (int)msg.length() && (msg[i] == ' ' || msg[i] == '\t')) {
        i++;
    }

    long value = 0;
    bool hasDigit = false;
    while (i < (int)msg.length() && msg[i] >= '0' && msg[i] <= '9') {
        hasDigit = true;
        value = (value * 10) + (msg[i] - '0');
        i++;
    }

    if (!hasDigit) return 0;
    return value > 0 ? (int)value : 0;
}

static int parseCooldown(const String& msg) {
    int keyPos = -1;
    if (!findFirstKey(msg, "cooldown", &keyPos)) return 0;

    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return 0;

    int i = colonPos + 1;
    while (i < (int)msg.length() && (msg[i] == ' ' || msg[i] == '\t')) {
        i++;
    }

    long value = 0;
    bool hasDigit = false;
    while (i < (int)msg.length() && msg[i] >= '0' && msg[i] <= '9') {
        hasDigit = true;
        value = (value * 10) + (msg[i] - '0');
        i++;
    }

    if (!hasDigit) return 0;
    return value > 0 ? (int)value : 0;
}

static int parseIntByKey(const String& msg, const char* key) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) return 0;

    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return 0;

    int i = colonPos + 1;
    while (i < (int)msg.length() && (msg[i] == ' ' || msg[i] == '\t')) {
        i++;
    }

    bool negative = false;
    if (i < (int)msg.length() && msg[i] == '-') {
        negative = true;
        i++;
    }

    long value = 0;
    bool hasDigit = false;
    while (i < (int)msg.length() && msg[i] >= '0' && msg[i] <= '9') {
        hasDigit = true;
        value = (value * 10) + (msg[i] - '0');
        i++;
    }

    if (!hasDigit) return 0;
    if (negative) value = -value;
    return (int)value;
}

static bool parseStringByKey(const String& msg, const char* key, String& outValue) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) return false;

    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return false;

    int firstQuote = msg.indexOf('"', colonPos + 1);
    if (firstQuote < 0) return false;

    // Lê até a próxima aspa não escapada por barra invertida
    outValue = "";
    bool esc = false;
    for (int i = firstQuote + 1; i < (int)msg.length(); i++) {
        char c = msg[i];
        if (esc) { esc = false; outValue += c; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        outValue += c;
    }
    return true;
}

static bool parseBoolByKey(const String& msg, const char* key, bool* found) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) {
        *found = false;
        return false;
    }

    *found = true;
    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) {
        return false;
    }

    String after = msg.substring(colonPos + 1);
    after.trim();
    after.toLowerCase();
    // Aceita boolean JSON (`true`) ou string (`"true"`), comum em serializers.
    if (after.length() >= 2 && after.charAt(0) == '"') {
        int close = after.indexOf('"', 1);
        if (close > 1) {
            after = after.substring(1, close);
            after.trim();
            after.toLowerCase();
        }
    }
    return after.startsWith("true") || after == "1";
}

// Extrai objeto JSON balanceado a partir do indice do primeiro '{' (ignora { } dentro de strings).
static bool extractBalancedJsonObject(const String& msg, int openBrace, String& outObject) {
    if (openBrace < 0 || openBrace >= (int)msg.length() || msg[openBrace] != '{') {
        return false;
    }
    int depth = 0;
    bool inString = false;
    bool escape = false;
    const int len = msg.length();
    for (int i = openBrace; i < len; i++) {
        char c = msg[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                outObject = msg.substring(openBrace, i + 1);
                return true;
            }
        }
    }
    return false;
}

// Primeira ocorrencia valida de "key" como chave JSON (nao exige unicidade no payload inteiro).
static bool extractJsonObjectByKey(const String& msg, const char* key, String& outObject) {
    String token = String("\"") + key + "\"";
    int searchStart = 0;
    const int len = msg.length();

    while (true) {
        int keyPos = msg.indexOf(token, searchStart);
        if (keyPos < 0) {
            return false;
        }
        int afterKey = keyPos + (int)token.length();
        while (afterKey < len && (msg[afterKey] == ' ' || msg[afterKey] == '\t' || msg[afterKey] == '\r' || msg[afterKey] == '\n')) {
            afterKey++;
        }
        if (afterKey >= len || msg[afterKey] != ':') {
            searchStart = keyPos + 1;
            continue;
        }
        int valStart = afterKey + 1;
        while (valStart < len && (msg[valStart] == ' ' || msg[valStart] == '\t')) {
            valStart++;
        }
        if (valStart < len && msg[valStart] == '{') {
            if (extractBalancedJsonObject(msg, valStart, outObject)) {
                return true;
            }
        }
        searchStart = keyPos + 1;
    }
}

static bool extractScheduleDocumentJson(const String& msg, String& outObject) {
    static const char* const KEYS[] = {"schedule", "data", "payload", "document", "scheduleData"};
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (extractJsonObjectByKey(msg, KEYS[i], outObject) && outObject.length() > 2) {
            return true;
        }
    }
    return false;
}

static bool extractJsonArrayByKey(const String& msg, const char* key, String& outArray) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) return false;

    int openBracket = msg.indexOf('[', keyPos);
    if (openBracket < 0) return false;

    int depth = 0;
    for (int i = openBracket; i < (int)msg.length(); i++) {
        if (msg[i] == '[') {
            depth++;
        } else if (msg[i] == ']') {
            depth--;
            if (depth == 0) {
                outArray = msg.substring(openBracket, i + 1);
                return true;
            }
        }
    }

    return false;
}

static int weekdayFromName(const String& nameRaw) {
    String name = nameRaw;
    name.trim();
    name.toLowerCase();

    // Convenção: domingo=1, segunda=2, ... sábado=7.
    if (name == "0" || name == "1" || name == "sun" || name == "sunday" || name == "domingo") return 0;
    if (name == "2" || name == "mon" || name == "monday" || name == "segunda" || name == "segunda-feira") return 1;
    if (name == "3" || name == "tue" || name == "tuesday" || name == "terca" || name == "terça" || name == "terca-feira" || name == "terça-feira") return 2;
    if (name == "4" || name == "wed" || name == "wednesday" || name == "quarta" || name == "quarta-feira") return 3;
    if (name == "5" || name == "thu" || name == "thursday" || name == "quinta" || name == "quinta-feira") return 4;
    if (name == "6" || name == "fri" || name == "friday" || name == "sexta" || name == "sexta-feira") return 5;
    if (name == "7" || name == "sat" || name == "saturday" || name == "sabado" || name == "sábado") return 6;

    return -1;
}

static bool shouldHandleForThisDevice(const String& deviceIdRaw) {
    if (deviceIdRaw.length() == 0) {
        return true;
    }

    String expected = getDeviceIdFromMac();
    String incoming = deviceIdRaw;
    expected.trim();
    incoming.trim();
    expected.replace(":", "");
    expected.replace("-", "");
    incoming.replace(":", "");
    incoming.replace("-", "");
    expected.toLowerCase();
    incoming.toLowerCase();
    return incoming == expected;
}

static bool shouldHandlePayloadForThisDevice(const String& payloadJson) {
    String deviceId;
    String macAddress;
    bool hasDeviceId = parseStringByKey(payloadJson, "deviceId", deviceId);
    bool hasMacAddress = parseStringByKey(payloadJson, "macAddress", macAddress);

    if (!hasDeviceId && !hasMacAddress) {
        return true;
    }

    if (hasDeviceId && !shouldHandleForThisDevice(deviceId)) {
        return false;
    }

    if (hasMacAddress && !shouldHandleForThisDevice(macAddress)) {
        return false;
    }

    return true;
}

static int parseDurationFromSchedule(const String& scheduleJson) {
    int value = parseIntByKey(scheduleJson, "tempoAcionamento");
    if (value > 0) return value > DURACAO_IRRIGACAO_MAXIMA ? DURACAO_IRRIGACAO_MAXIMA : value;

    value = parseIntByKey(scheduleJson, "duracaoSegundos");
    if (value > 0) return value > DURACAO_IRRIGACAO_MAXIMA ? DURACAO_IRRIGACAO_MAXIMA : value;

    value = parseIntByKey(scheduleJson, "duration");
    if (value > 0) return value > DURACAO_IRRIGACAO_MAXIMA ? DURACAO_IRRIGACAO_MAXIMA : value;

    return 0;
}

// Parseia um valor float para a chave JSON `key`. Retorna true se encontrado.
static bool parseFloatKey(const String& msg, const char* key, float* outVal) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) return false;
    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return false;
    int i = colonPos + 1;
    int len = msg.length();
    // pular espaços e aspas
    while (i < len && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '"')) i++;
    int start = i;
    // aceitar caracteres de número, ponto, sinal, e notação exponencial
    while (i < len) {
        char c = msg[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
            i++; continue;
        }
        break;
    }
    if (i <= start) return false;
    String token = msg.substring(start, i);
    token.trim();
    float v = token.toFloat();
    if (outVal) *outVal = v;
    return true;
}

static bool parseHourMinute(const String& hhmm, int* outHour, int* outMinute) {
    int sep = hhmm.indexOf(':');
    if (sep <= 0) return false;

    int hour = hhmm.substring(0, sep).toInt();
    int minute = hhmm.substring(sep + 1).toInt();

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }

    *outHour = hour;
    *outMinute = minute;
    return true;
}

static uint8_t parseDiasSemanaMask(const String& scheduleJson) {
    int keyPos = scheduleJson.indexOf("\"diasSemana\"");
    if (keyPos < 0) return 0;

    int openBracket = scheduleJson.indexOf('[', keyPos);
    int closeBracket = scheduleJson.indexOf(']', openBracket);
    if (openBracket < 0 || closeBracket < 0 || closeBracket <= openBracket) {
        return 0;
    }

    uint8_t mask = 0;
    int i = openBracket + 1;

    while (i < closeBracket) {
        while (i < closeBracket && (scheduleJson[i] == ' ' || scheduleJson[i] == '\t' || scheduleJson[i] == ',')) {
            i++;
        }

        if (i < closeBracket && scheduleJson[i] == '"') {
            int quoteEnd = scheduleJson.indexOf('"', i + 1);
            if (quoteEnd > i && quoteEnd <= closeBracket) {
                String token = scheduleJson.substring(i + 1, quoteEnd);
                int weekday = weekdayFromName(token);
                if (weekday >= 0) {
                    mask |= (1U << weekday);
                }
                i = quoteEnd + 1;
                continue;
            }
        }

        bool hasDigit = false;
        int value = 0;
        while (i < closeBracket && scheduleJson[i] >= '0' && scheduleJson[i] <= '9') {
            hasDigit = true;
            value = (value * 10) + (scheduleJson[i] - '0');
            i++;
        }

        if (!hasDigit) {
            i++;
            continue;
        }

        int weekday = -1;
        // Convenção do app/Firestore: domingo=1, segunda=2, ... sábado=7.
        if (value >= 1 && value <= 7) {
            weekday = value - 1;
        }

        if (weekday >= 0) {
            mask |= (1U << weekday);
        }
    }

    return mask;
}

static String formatDiasSemanaMask(uint8_t diasMask) {
    const char* dayNames[7] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"};
    String out;

    for (int day = 0; day < 7; day++) {
        if ((diasMask & (1U << day)) == 0) {
            continue;
        }

        if (out.length() > 0) {
            out += ",";
        }

        out += dayNames[day];
    }

    if (out.length() == 0) {
        return "nenhum";
    }

    return out;
}

static bool handleScheduleEvent(const String& msg) {
    String action;
    if (!parseStringByKey(msg, "action", action)) {
        return false;
    }

    if (action != "scheduleCreated" && action != "scheduleUpdated" && action != "scheduleDeleted") {
        return false;
    }

    String scheduleJson;
    if (!extractScheduleDocumentJson(msg, scheduleJson)) {
        Serial.print("[SCHEDULE] evento sem objeto schedule payloadLen=");
        Serial.println(msg.length());
        return true;
    }

    // Alguns backends envolvem o documento: payload.schedule ou payload.data
    String scheduleIdProbe;
    if (!parseStringByKey(scheduleJson, "id", scheduleIdProbe)) {
        String nested;
        if (extractJsonObjectByKey(scheduleJson, "schedule", nested)) {
            scheduleJson = nested;
        } else if (extractJsonObjectByKey(scheduleJson, "data", nested)) {
            scheduleJson = nested;
        }
    }

    String scheduleId;
    parseStringByKey(scheduleJson, "id", scheduleId);
    if (scheduleId.length() == 0) {
        Serial.println("[SCHEDULE] id ausente");
        return true;
    }

    String deviceId;
    parseStringByKey(scheduleJson, "deviceId", deviceId);
    if (deviceId.length() == 0) {
        parseStringByKey(scheduleJson, "macAddress", deviceId);
    }
    if (!shouldHandleForThisDevice(deviceId)) {
        Serial.print("[SCHEDULE] ignorado para outro device. esperado=");
        Serial.print(getDeviceIdFromMac());
        Serial.print(" recebido=");
        Serial.println(deviceId);
        return true;
    }

    if (action == "scheduleDeleted") {
        bool removed = atuador.removeSchedule(scheduleId.c_str());
        Serial.print("[SCHEDULE] deleted id=");
        Serial.print(scheduleId);
        Serial.print(" ok=");
        Serial.println(removed ? "true" : "false");
        return true;
    }

    String hourText;
    if (!parseStringByKey(scheduleJson, "horaAcionamento", hourText)) {
        parseStringByKey(scheduleJson, "hora", hourText);
    }

    int hour = 0;
    int minute = 0;
    if (!parseHourMinute(hourText, &hour, &minute)) {
        Serial.println("[SCHEDULE] horario invalido");
        return true;
    }

    bool foundAtivo = false;
    bool ativo = parseBoolByKey(scheduleJson, "ativo", &foundAtivo);
    if (!foundAtivo) {
        if (action == "scheduleCreated") {
            ativo = true;
        } else if (action == "scheduleUpdated") {
            bool stored = false;
            if (atuador.getStoredScheduleAtivo(scheduleId.c_str(), &stored)) {
                ativo = stored;
            } else {
                ativo = true;
            }
        } else {
            ativo = false;
        }
    }

    int duration = parseDurationFromSchedule(scheduleJson);
    if (duration <= 0) {
        duration = atuador.getDuracaoSegundos();
    }

    uint8_t diasMask = parseDiasSemanaMask(scheduleJson);
    String createdAt;
    parseStringByKey(scheduleJson, "createdAt", createdAt);

    bool ok = atuador.upsertSchedule(scheduleId.c_str(), ativo, diasMask, hour, minute, duration, createdAt.c_str());

    // Feedback ao app: se a agenda foi rejeitada por falta de slot, publica no tópico de status
    // para que o app possa avisar o usuário.
    // Usa atuador.getMaxSchedules() para evitar literal hardcoded desatualizado.
    if (!ok && (action == "scheduleCreated")) {
        char errPayload[128];
        snprintf(errPayload, sizeof(errPayload),
                 "{\"error\":\"schedule_limit_reached\",\"maxSlots\":%d,\"rejectedId\":\"%s\"}",
                 atuador.getMaxSchedules(), scheduleId.c_str());
        queuePendingStatusPublish(errPayload);
        Serial.print("[SCHEDULE] agenda rejeitada por limite de slots. Feedback publicado em ");
        Serial.println(cachedStatusTopic);
    }

    if (ok && diasMask == 0) {
        Serial.println("[SCHEDULE] aviso: diasMask=0 — nenhum dia da semana; disparo nunca ocorre (revise diasSemana no JSON)");
    }

    String changedFields;
    bool hasChangedFields = extractJsonArrayByKey(msg, "changedFields", changedFields);

    if (action == "scheduleCreated") {
        Serial.println("[SCHEDULE] created");
        Serial.print("[SCHEDULE] diasSemana=");
        Serial.println(formatDiasSemanaMask(diasMask));
        Serial.print("[SCHEDULE] horaAcionamento=");
        if (hour < 10) Serial.print("0");
        Serial.print(hour);
        Serial.print(":");
        if (minute < 10) Serial.print("0");
        Serial.println(minute);
        Serial.print("[SCHEDULE] tempoAcionamento=");
        Serial.println(duration);
        Serial.print("[SCHEDULE] createdAt=");
        Serial.println(createdAt.length() > 0 ? createdAt : "n/a");
    } else if (action == "scheduleUpdated") {
        Serial.println("[SCHEDULE] updated");
        if (hasChangedFields) {
            Serial.print("[SCHEDULE] changedFields=");
            Serial.println(changedFields);
        }
    }

    Serial.print("[SCHEDULE] action=");
    Serial.print(action);
    Serial.print(" id=");
    Serial.print(scheduleId);
    Serial.print(" deviceId=");
    Serial.print(deviceId);
    Serial.print(" ativo=");
    Serial.print(ativo ? "true" : "false");
    Serial.print(" hora=");
    Serial.print(hour);
    Serial.print(":");
    if (minute < 10) Serial.print("0");
    Serial.print(minute);
    Serial.print(" duration=");
    Serial.print(duration);
    Serial.print(" diasMask=");
    Serial.print(diasMask);
    Serial.print(" saved=");
    Serial.println(ok ? "true" : "false");

    return true;
}

// Publish sensor telemetry (delta-optimized). Keeps payload small and avoid reallocations.
static void publishTelemetry(const SensorData& data) {
    // Buffer aumentado para acomodar o campo deviceId (ex: "esp32_01" + overhead JSON)
    char payload[220];
    const char* waterLevel = data.nivelAgua ? "Cheio" : "Vazio";
    const char* pumpStatus = atuador.isLigado() ? "ON" : "OFF";

    snprintf(
        payload,
        sizeof(payload),
        "{\"deviceId\":\"%s\",\"soilMoisture\":%d,\"temperature\":%d,\"humidity\":%d,\"waterLevel\":\"%s\",\"pump\":\"%s\"}",
        getDeviceIdFromMac().c_str(),
        data.umidadeSolo,
        (int)data.temperatura,
        (int)data.umidadeAr,
        waterLevel,
        pumpStatus);

    publishMessage(cachedTelemetryTopic, payload, true);
}

// ================== CALLBACK ==================
// MQTT message callback: parses incoming commands and updates actuator/schedules accordingly.
void callback(char* topic, byte* payload, unsigned int length) {

    String msg;
    msg.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) {
        msg.concat((char)payload[i]);
    }

    if (strcmp(topic, cachedCommandsTopic) == 0) {
        if (!isLikelyJsonObject(msg) && msg != "ON" && msg != "OFF") {
            Serial.println("[MQTT] payload malformado: comando ignorado");
            return;
        }

        if (isLikelyJsonObject(msg) && !shouldHandlePayloadForThisDevice(msg)) {
            Serial.print("[MQTT] comando ignorado para outro device. esperado=");
            Serial.print(getDeviceIdFromMac());
            Serial.println(" recebido via payload");
            return;
        }

        String action;
        bool hasAction = parseAction(msg, action);

        if (handleScheduleEvent(msg)) {
            return;
        }

        if (hasAction && hasSetModeAction(action)) {
            bool hasMode = false;
            bool modeAuto = parseModeAuto(msg, &hasMode);
            if (hasMode) {
                atuador.setModoAuto(modeAuto);
                Serial.print("[MODE] atualizado via setMode: ");
                Serial.println(modeAuto ? "AUTO" : "MANUAL");
            }
            return;
        }

        if (hasAction && hasSetConfigAction(action)) {
            bool hasMode = false;
            bool modeAuto = parseModeAuto(msg, &hasMode);
            int duration = parseDuration(msg);
            int threshold = parseThreshold(msg);
            int cooldown = parseCooldown(msg);

            if (hasMode) {
                atuador.setModoAuto(modeAuto);
            }

            if (duration > 0) {
                atuador.setDuracaoSegundos(duration);
            }

            if (threshold > 0) {
                atuador.setThresholdUmidade(threshold);
                Serial.print("[CONFIG] threshold=");
                Serial.println(threshold);
            }

            if (cooldown > 0) {
                atuador.setCooldownSegundos(cooldown);
                Serial.print("[CONFIG] cooldown=");
                Serial.println(cooldown);
            }

            // Persiste duracao/threshold/cooldown em uma unica escrita NVS.
            // setModoAuto() ja persiste de forma independente e imediata.
            atuador.flushConfig();

            Serial.println("[CONFIG] setConfig aplicado");
            return;
        }

        // Handler para calibracao de vazao via MQTT
        if (hasAction && action == "setFlowCalibration") {
            float scale = 0.0f;
            float offset = 0.0f;
            bool hasScale = parseFloatKey(msg, "scale", &scale);
            bool hasOffset = parseFloatKey(msg, "offsetMl", &offset);

            if (!hasScale) {
                Serial.println("[MQTT] setFlowCalibration sem 'scale' - ignorando");
                return;
            }

            extern FlowMeterManager flowMeter;
            flowMeter.setVolumeCalibration(scale, hasOffset ? offset : 0.0f);

            // Acknowledge via telemetry topic
            String ack = String("{\"action\":\"setFlowCalibration\",\"scale\":") + String(scale, 6) + ",\"offsetMl\":" + String(hasOffset ? offset : 0.0f, 3) + "}";
            publishMessage(cachedTelemetryTopic, ack.c_str(), false);
            Serial.println("[MQTT] setFlowCalibration aplicado");
            return;
        }

        if (msg == "OFF") {
            if (mqttManagerInstance) {
                mqttManagerInstance->requestStopIrrigation(STOP_REASON_MANUAL);
            }
            return;
        }

        SensorData data = sensores.read();
        if (!data.nivelAgua) {
            Serial.println("[SAFETY] sem agua, comando de ligamento bloqueado");
            atuador.desligar();
            atuador.setActiveUntil(0);
            return;
        }

        if (msg == "ON") {
            unsigned long activeUntil = millis() + ((unsigned long)atuador.getDuracaoSegundos() * 1000UL);
            atuador.setCurrentTrigger(TRIGGER_MANUAL);
            atuador.ligar();
            if (g_eventMgr) g_eventMgr->onIrrigationStart(TRIGGER_MANUAL);
            atuador.setActiveUntil(activeUntil);
            return;
        }

        if (hasAction && hasIrrigateAction(action)) {
            int duration = parseDuration(msg);
            atuador.setDuracaoSegundos(duration);
            // Persiste a nova duracao imediatamente — este setter e chamado
            // isoladamente (sem batch de outros campos de config).
            atuador.flushConfig();

            if (duration > 0) {
                atuador.setCurrentTrigger(TRIGGER_MANUAL);
                atuador.ligar();
                if (g_eventMgr) g_eventMgr->onIrrigationStart(TRIGGER_MANUAL);
                atuador.setActiveUntil(millis() + ((unsigned long)duration * 1000UL));
            } else {
                if (mqttManagerInstance) {
                    mqttManagerInstance->requestStopIrrigation(STOP_REASON_MANUAL);
                }
            }
            return;
        }
    }
}

// ================== CONEXAO ==================
// Establish connection to MQTT broker with exponential backoff. Ensures client state is reset before connect.
bool connectMQTT() {
    // Se já estamos conectados, nada a fazer
    if (client.connected()) return true;

    bool alreadyRunning = false;
    taskENTER_CRITICAL(&mqttConnectMux);
    alreadyRunning = connectTaskRunning;
    if (!alreadyRunning) {
        connectTaskRunning = true;
    }
    taskEXIT_CRITICAL(&mqttConnectMux);

    // Se já existe uma task de conexão em andamento, retornamos false
    if (alreadyRunning) {
        Serial.println("[MQTT] Conexao async ja em andamento");
        return false;
    }

    Serial.println("[MQTT] Iniciando task assíncrona de conexão MQTT...");

    // Criar task no Core 0 para executar a conexão (TLS/handshake podem bloquear)
    BaseType_t result = xTaskCreatePinnedToCore(
        mqttConnectTask,
        "mqttConnect",
        MQTT_CONNECT_TASK_STACK_BYTES,
        nullptr,
        1,
        nullptr,
        0 // core 0
    );

    if (result != pdPASS) {
        fwLogLine("ERROR", "MQTT", "Falha ao criar mqttConnect task");
        taskENTER_CRITICAL(&mqttConnectMux);
        connectTaskRunning = false;
        taskEXIT_CRITICAL(&mqttConnectMux);
        return false;
    }

    return false;
}

// Task que executa uma unica tentativa de conexao sem bloquear o loop principal.
// O loop() ja controla o backoff exponencial entre novas tentativas.
static void mqttConnectTask(void* pvParameters) {
    fwLogSection("MQTT", "Reconexao ao broker");
    fwLogf("INFO", "MQTT", "freeHeap=%lu", (unsigned long)ESP.getFreeHeap());
    fwLogf("INFO", "MQTT", "minFreeHeap=%lu", (unsigned long)ESP.getMinFreeHeap());

    fwLogLine("INFO", "MQTT", "Tentativa unica de conexao com o broker");

    // Setar flag para evitar que Core 1 chame client.loop() durante connect()
    mqttConnectingInProgress = true;

    // NOTA: NÃO chamar esp_task_wdt_delete() aqui.
    // mqttConnectTask nunca foi registrada no TWDT (só a loopTask foi via
    // esp_task_wdt_add(NULL) no setup). Chamar wdt_delete numa task não registrada
    // gera "delete_entry: task not found" e não tem efeito útil.
    //
    // O TWDT agora monitora APENAS a loopTask (main.ino). O handshake TLS desta
    // task não interfere mais no watchdog. Veja idle_core_mask=0 em main.ino.

    // NÃO usar mutex em client.connect() — é bloqueante (TLS handshake).
    // A flag mqttConnectingInProgress evita concorrência com client.loop() no Core 1.
    bool connected = client.connect(
        clientId.c_str(),
        MQTT_USER,
        MQTT_PASS,
        cachedStatusTopic,
        1,
        true,
        "offline");

    if (connected) {
        fwLogLine("INFO", "MQTT", "MQTT conectado");
        // NÃO executar subscribe/publish/sensores.read()/atuador aqui.
        // Estas operações acessam objetos que vivem no Core 1 e não são
        // thread-safe. Sinalizar via flag; o loop() do Core 1 fará tudo
        // em segurança na próxima iteração.
        mqttJustConnected = true;
    } else {
        fwLogLine("WARN", "MQTT", "Falha ao conectar; nova tentativa apenas apos backoff exponencial");
    }

    // Liberar o loop somente depois que todas as operações do connect terminaram.
    mqttConnectingInProgress = false;

    if (!client.connected()) {
        fwLogLine("WARN", "MQTT", "Broker indisponivel no momento");
    }

    taskENTER_CRITICAL(&mqttConnectMux);
    connectTaskRunning = false;
    taskEXIT_CRITICAL(&mqttConnectMux);

    // Pequeno delay cooperativo antes de destruir a task — dá tempo para o Core 1
    // observar connectTaskRunning = false na próxima iteração do loop().
    vTaskDelay(pdMS_TO_TICKS(20));

    // Finalizar task explicitamente (sem reregistrar no TWDT — task encerra aqui)
    vTaskDelete(nullptr);
}

// ================== INIT ==================
// Initialize MQTT client, FreeRTOS firebase queue/task, set callback and prepare clientId.
// eventMgr: referencia explicita ao IrrigationEventManager — elimina acoplamento via extern.
// Call once from setup().
void MqttManager::begin(IrrigationEventManager& eventMgr) {
    mqttManagerInstance = this;
    _eventMgr = &eventMgr;
    g_eventMgr = &eventMgr;

    espClient.setInsecure(); // TLS sem validacao

    clientId = "ESP32_" + WiFi.macAddress();
    refreshMqttTopicCache();

    client.setServer(MQTT_SERVER, MQTT_PORT);
    // Schedule events include nested JSON and timestamps; default PubSubClient packet size is too small.
    client.setBufferSize(4096);
    client.setCallback(callback);
    client.setKeepAlive(10);
    client.setSocketTimeout(15);  // CORREÇÃO: aumentado de 8s para 15s — handshake TLS
                                  // com HiveMQ Cloud pode ultrapassar 8s em rede instável.

    // Cria fila com capacidade para 4 eventos — suficiente para rajadas normais.
    // Cada item e' copiado por valor, sem ponteiros pendentes.
    firebaseQueue = xQueueCreate(4, sizeof(FirebaseEventPayload));
    if (firebaseQueue == nullptr) {
        fwLogLine("ERROR", "FIREBASE", "Falha ao criar fila FreeRTOS");
        return;
    }

    // Cria a task no Core 0 (protocolo/rede). Mantida separada do loopTask (Core 1)
    // para que operações bloqueantes de TLS/HTTPClient não atrasem o loop principal.
    // Stack de 20 KB acomoda HTTPClient + WiFiClientSecure sem risco de overflow.
    BaseType_t result = xTaskCreatePinnedToCore(
        firebaseTask,       // funcao da task
        "firebaseHTTP",    // nome para debug
        20480,              // stack em bytes (20 KB)
        nullptr,            // parametro
        1,                  // prioridade
        nullptr,            // handle (nao necessario)
        0                   // core 0 — separado do loopTask
    );

    if (result != pdPASS) {
        fwLogLine("ERROR", "FIREBASE", "Falha ao criar task FreeRTOS");
    } else {
        fwLogLine("INFO", "FIREBASE", "Task HTTP iniciada no Core 0 (stack=20KB)");
    }
}

// ================== LOOP ==================
void MqttManager::loop() {

    if (WiFi.status() != WL_CONNECTED) {
        mqttWasConnected = false;
        return;
    }

    if (!client.connected()) {
        if (mqttWasConnected) {
            fwLogLine("WARN", "MQTT", "MQTT desconectado");
            mqttWasConnected = false;
        }

        unsigned long now = millis();

        if (now - lastReconnectAttempt >= reconnectBackoffMs) {
            lastReconnectAttempt = now;

            if (connectMQTT()) {
                lastReconnectAttempt = 0;
                reconnectBackoffMs = 5000UL;
                mqttWasConnected = true;
            } else if (reconnectBackoffMs < 60000UL) {
                reconnectBackoffMs *= 2UL;
                if (reconnectBackoffMs > 60000UL) {
                    reconnectBackoffMs = 60000UL;
                }
            }
        }

        return;
    }

    mqttWasConnected = true;

    // Pós-conexão: executado no Core 1 (loop principal) onde client e sensores são seguros.
    // A mqttConnectTask (Core 0) apenas seta mqttJustConnected; toda lógica de
    // subscribe/publish/reset-de-estado acontece aqui.
    if (mqttJustConnected) {
        mqttJustConnected = false;

        client.subscribe(cachedCommandsTopic);
        client.publish(cachedStatusTopic, "online", true);
        fwLogf("INFO", "MQTT", "Subscribed em %s", cachedCommandsTopic);

        // Reset de estado delta — forçar publicação de telemetria completa na reconexão.
        lastSolo   = -1;
        lastTemp   = -1000;
        lastUmidAr = -1000;
        SensorData postConnectData = sensores.read();
        lastNivelAgua = !postConnectData.nivelAgua;
        lastBomba  = !atuador.isLigado();
        pendingSensorStability = false;
        sensorStabilitySince   = 0;
        candidateSolo   = -1;
        candidateTemp   = -1000;
        candidateUmidAr = -1000;
        mqttWasConnected      = true;
        reconnectBackoffMs    = 5000UL;
    }

    // Pular client.loop() se uma conexão está em andamento (Core 0 em client.connect()).
    if (!mqttConnectingInProgress) {
        client.loop();
    }
    
    flushPendingStatusPublish();

    SensorData data = sensores.read();
    unsigned long now = millis();

    // ===== DETECÇÃO DE CONDIÇÕES DE PARADA =====
    // O mqtt_manager NÃO age diretamente sobre atuador nem irrigationEventManager.
    // Apenas detecta a condição e sinaliza para o controlador central (main.ino)
    // via requestStopIrrigation(). main.ino chama stopIrrigation() na próxima
    // iteração do loop de controle (~2 s), garantindo ponto único de desligamento.
    // Guarda _stopRequested: evita dezenas de logs repetidos enquanto main.ino
    // aguarda CONTROL_INTERVAL_MS (2s) para consumir o sinal via consumeStopRequest().
    if (atuador.isLigado() && !_stopRequested) {
        if (atuador.getActiveUntil() > 0 && now >= atuador.getActiveUntil()) {
            fwLogLine("INFO", "MQTT", "Timeout detectado; sinalizando parada (COMPLETED)");
            requestStopIrrigation(STOP_REASON_COMPLETED);
        } else if (!data.nivelAgua) {
            fwLogLine("WARN", "MQTT", "Falta de agua detectada; sinalizando parada (NO_WATER)");
            requestStopIrrigation(STOP_REASON_NO_WATER);
        }
    }

    bool nivelAguaMudou = data.nivelAgua != lastNivelAgua;
    bool bombaMudou = atuador.isLigado() != lastBomba;
    bool soloMudou = abs(data.umidadeSolo - lastSolo) >= 2;
    bool tempMudou = abs((int)data.temperatura - lastTemp) >= 1;
    bool umidadeArMudou = abs((int)data.umidadeAr - lastUmidAr) >= 2;
    bool sensorMudou = soloMudou || tempMudou || umidadeArMudou;
    bool publicarTelemetria = false;

    // Mantem comportamento imediato para reservatorio/bomba.
    if (nivelAguaMudou || bombaMudou) {
        publicarTelemetria = true;
    }

    // Exige estabilidade de 3s para solo/temperatura/umidade do ar.
    if (sensorMudou) {
        int atualTemp = (int)data.temperatura;
        int atualUmidAr = (int)data.umidadeAr;

        if (!pendingSensorStability) {
            pendingSensorStability = true;
            sensorStabilitySince = now;
            candidateSolo = data.umidadeSolo;
            candidateTemp = atualTemp;
            candidateUmidAr = atualUmidAr;
        } else {
            bool alterouDuranteJanela =
                abs(data.umidadeSolo - candidateSolo) >= 2 ||
                abs(atualTemp - candidateTemp) >= 1 ||
                abs(atualUmidAr - candidateUmidAr) >= 2;

            if (alterouDuranteJanela) {
                sensorStabilitySince = now;
                candidateSolo = data.umidadeSolo;
                candidateTemp = atualTemp;
                candidateUmidAr = atualUmidAr;
            }
        }
    }

    if (pendingSensorStability && (now - sensorStabilitySince) >= TELEMETRY_STABLE_WINDOW_MS) {
        publicarTelemetria = true;
        pendingSensorStability = false;
    }

    if (publicarTelemetria) {
        publishTelemetry(data);

        lastNivelAgua = data.nivelAgua;
        lastBomba = atuador.isLigado();
        lastSolo = data.umidadeSolo;
        lastTemp = (int)data.temperatura;
        lastUmidAr = (int)data.umidadeAr;
        pendingSensorStability = false;
    }

    // ===== EVENTOS DE IRRIGACAO =====
    // publishIrrigationEvent (MQTT) e chamado normalmente — e rapido.
    // sendIrrigationEventToFirebase apenas enfileira o payload; o HTTP
    // ocorre na firebaseTask (Core 0) sem bloquear este loop.
    if (g_eventMgr && (mqttWasConnected || client.connected())) {
        IrrigationEvent event;
        while (g_eventMgr->getNextPendingEvent(&event)) {
            char startIso[32];
            char endIso[32];

            g_eventMgr->formatIso8601(event.startAt, startIso, sizeof(startIso));
            g_eventMgr->formatIso8601(event.endAt, endIso, sizeof(endIso));

            // Publicar via MQTT (nao bloqueia). So remove da fila se sucesso.
            bool mqttPublished = publishIrrigationEvent(
                event.eventId,
                startIso,
                endIso,
                event.durationSec,
                g_eventMgr->getTriggerTypeString(event.trigger),
                g_eventMgr->getStopReasonString(event.stopReason),
                event.totalPulses,
                event.totalVolumeLiters,
                event.avgFlowRateLpm,
                event.flowDetected,
                event.flowStatus,
                event.totalVolumeMl,
                event.accountingVolumeMl,
                event.nominalFlowRateMlPerMin
            );

            if (!mqttPublished) {
                fwLogf("WARN", "MQTT", "Evento pendente mantido para retry: %s", event.eventId);
                break;
            }

            // Enfileira para HTTP Firebase (nao bloqueia — a task cuida do envio)
            sendIrrigationEventToFirebase(
                event.eventId,
                startIso,
                endIso,
                event.durationSec,
                g_eventMgr->getTriggerTypeString(event.trigger),
                event.stopReason,
                data.umidadeSolo,
                (int)data.umidadeAr,
                data.temperatura,
                data.nivelAgua,
                event.totalPulses,
                event.totalVolumeLiters,
                event.avgFlowRateLpm,
                event.flowDetected,
                event.flowStatus,
                event.totalVolumeMl,
                event.accountingVolumeMl,
                event.nominalFlowRateMlPerMin
            );

            g_eventMgr->markEventSent(event.eventId);
        }
    }

    // ===== HEARTBEAT =====
    if (now - lastHeartbeat >= 10000) {
        publishMessage(cachedStatusTopic, "online", true);
        lastHeartbeat = now;
    }
}

bool MqttManager::isConnected() {
    return WiFi.status() == WL_CONNECTED && client.connected();
}

// ==================== PUBLICACAO DE EVENTOS ====================

bool MqttManager::publishIrrigationEvent(const char* eventId, const char* startAtIso,
                                         const char* endAtIso, int durationSec,
                                         const char* triggerType, const char* stopReason,
                                         uint32_t totalPulses, float totalVolumeLiters,
                                         float avgFlowRateLpm, bool flowDetected,
                                         const char* flowStatus,
                                         float totalVolumeMl,
                                         float accountingVolumeMl,
                                         float nominalFlowRateMlPerMin) {
    if (!eventId || !startAtIso || !endAtIso || !triggerType || !stopReason) {
        fwLogLine("ERROR", "MQTT", "publishIrrigationEvent: parametros invalidos");
        return false;
    }

    // Construir tópico: irrigahome/{deviceId}/events/irrigation
    char topic[128];
    String deviceId = getDeviceIdFromMac();
    snprintf(topic, sizeof(topic), "irrigahome/%s/events/irrigation", deviceId.c_str());

    // Construir payload JSON
    char payload[800];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"irrigation_completed\",\"deviceId\":\"%s\",\"historyCollection\":\"%s\",\"eventId\":\"%s\"," 
             "\"startAt\":\"%s\",\"endAt\":\"%s\",\"durationSec\":%d,"
             "\"trigger\":\"%s\",\"stopReason\":\"%s\","
             "\"flowSensor\":{\"totalPulses\":%u,\"totalVolumeLiters\":%.3f,\"totalVolumeMl\":%.1f,\"avgFlowRateLpm\":%.3f,\"flowDetected\":%s,\"flowStatus\":\"%s\"},"
             "\"accounting\":{\"accountingVolumeMl\":%.1f,\"nominalFlowRateMlPerMin\":%.1f}}",
             deviceId.c_str(), HISTORY_COLLECTION_NAME, eventId, startAtIso, endAtIso, durationSec, triggerType, stopReason,
             (unsigned)totalPulses, totalVolumeLiters, totalVolumeMl, avgFlowRateLpm, flowDetected ? "true" : "false", flowStatus ? flowStatus : "",
             accountingVolumeMl, nominalFlowRateMlPerMin);

    if (publishMessage(topic, payload, false)) {
        fwLogf("INFO", "MQTT", "Evento MQTT publicado: %s", eventId);
        return true;
    } else {
        fwLogf("ERROR", "MQTT", "Falha ao publicar evento: %s", eventId);
        return false;
    }
}

// ==================== SINALIZAÇÃO DE PARADA ====================
// Registra uma requisição de parada sem agir diretamente sobre o atuador.
// Idempotente: se já há um pedido pendente com motivo mais grave (NO_WATER
// sobre COMPLETED), o motivo existente é preservado.
void MqttManager::requestStopIrrigation(IrrigationStopReason reason) {
    if (!_stopRequested || reason > _stopReason) {
        _stopReason    = reason;
        _stopRequested = true;
    }
}

bool MqttManager::isStopRequested() const {
    return _stopRequested;
}

// Retorna o motivo e limpa o flag — deve ser chamado exatamente uma vez
// pelo consumidor (main.ino) por iteração de controle.
IrrigationStopReason MqttManager::consumeStopRequest() {
    IrrigationStopReason r = _stopReason;
    _stopRequested = false;
    _stopReason    = STOP_REASON_COMPLETED;
    return r;
}
