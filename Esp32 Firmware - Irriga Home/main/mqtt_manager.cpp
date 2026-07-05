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
#include "firebase_payload_builder.h"
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
static IrrigationEventManager* g_eventMgr = nullptr;

// Controle de eventos ja enfileirados para o Firebase nesta sessao (RAM).
static const int MAX_FIREBASE_ENQUEUED_IDS = 16;
static char firebaseEnqueuedIds[MAX_FIREBASE_ENQUEUED_IDS][24];
static int firebaseEnqueuedCount = 0;

static bool isAlreadyEnqueuedForFirebase(const char* eventId) {
    for (int i = 0; i < firebaseEnqueuedCount; i++) {
        if (strcmp(firebaseEnqueuedIds[i], eventId) == 0) return true;
    }
    return false;
}

static void markEnqueuedForFirebase(const char* eventId) {
    if (isAlreadyEnqueuedForFirebase(eventId)) return;
    if (firebaseEnqueuedCount < MAX_FIREBASE_ENQUEUED_IDS) {
        strncpy(firebaseEnqueuedIds[firebaseEnqueuedCount], eventId, sizeof(firebaseEnqueuedIds[0]) - 1);
        firebaseEnqueuedIds[firebaseEnqueuedCount][sizeof(firebaseEnqueuedIds[0]) - 1] = '\0';
        firebaseEnqueuedCount++;
    } else {
        for (int i = 1; i < MAX_FIREBASE_ENQUEUED_IDS; i++) {
            memcpy(firebaseEnqueuedIds[i - 1], firebaseEnqueuedIds[i], sizeof(firebaseEnqueuedIds[0]));
        }
        strncpy(firebaseEnqueuedIds[MAX_FIREBASE_ENQUEUED_IDS - 1], eventId, sizeof(firebaseEnqueuedIds[0]) - 1);
        firebaseEnqueuedIds[MAX_FIREBASE_ENQUEUED_IDS - 1][sizeof(firebaseEnqueuedIds[0]) - 1] = '\0';
    }
}

// Conexão assíncrona
static volatile bool connectTaskRunning = false;
static volatile bool mqttConnectingInProgress = false;
static volatile bool mqttJustConnected = false;
static void mqttConnectTask(void* pvParameters);
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

extern SensorManager sensores;
extern ActuatorManager atuador;

// ==================== FREERTOS: FILA E TASK FIREBASE ====================

struct FirebaseEventPayload {
    char eventId[24];
    char startAtIso[32];
    char endAtIso[32];
    int  durationSec;
    char triggerType[16];
    char stopReason[16];
    bool success;
    int  soilHumidity;
    int  airHumidity;
    float temperature;
    char waterLevel[10];
    uint32_t totalPulses;
    float totalVolumeLiters;
    float avgFlowRateLpm;
    float totalVolumeMl;
    float accountingVolumeMl;
    float nominalFlowRateMlPerMin;
    bool flowDetected;
    char flowStatus[16];
    char jsonPayload[1024];
};

static QueueHandle_t firebaseQueue = nullptr;

// ==================== SINCRONIZACAO DE AGENDAMENTOS ====================
static QueueHandle_t scheduleSyncQueue = nullptr;

static void scheduleSyncTask(void* pvParameters) {
    for (;;) {
        uint8_t trigger = 0;
        if (xQueueReceive(scheduleSyncQueue, &trigger, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!mqttManagerInstance) {
            continue;
        }

        mqttManagerInstance->syncSchedulesFromFirestore();
    }
}

struct FirestoreScheduleEntry {
    char id[64];
    bool ativo;
    uint8_t diasMask;
    int hour;
    int minute;
    int durationSeconds;
    char createdAt[32];
};

static const int MAX_FIRESTORE_SCHEDULES = 16;

// ==================== TASK FIREBASE ====================

static void firebaseTask(void* pvParameters) {
    FirebaseEventPayload payload;
    char url[1024];

    for (;;) {
        if (xQueueReceive(firebaseQueue, &payload, portMAX_DELAY) == pdTRUE) {

            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[FIREBASE] sem WiFi, envio adiado (sera retentado apos reconexao)");
                continue;
            }

            if (payload.jsonPayload[0] != '\0') {
                WiFiClientSecure httpsClient;
                httpsClient.setInsecure();
                httpsClient.setTimeout(10);

                HTTPClient http;
                http.begin(httpsClient, FIREBASE_SAVE_EVENT_URL);
                http.addHeader("Content-Type", "application/json");
                http.setTimeout(5000);

                Serial.print("[FIREBASE] Enviando JSON para: ");
                Serial.println(FIREBASE_SAVE_EVENT_URL);
                Serial.print("[FIREBASE] Payload: ");
                Serial.println(payload.jsonPayload);

                int httpCode = http.POST(payload.jsonPayload);

                if (httpCode > 0) {
                    if (httpCode == 200 || httpCode == 201) {
                        fwLogf("INFO", "FIREBASE", "Evento gravado com sucesso: %s", payload.eventId);
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
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            snprintf(url, sizeof(url),
                "%s?macAddress=%s&historyCollection=events&ownerUid=%s&eventId=%s&startAt=%s&endAt=%s&durationSec=%d&trigger=%s&stopReason=%s&success=%s&soilHumidity=%d&airHumidity=%d&temperature=%.2f&waterLevel=%s&totalPulses=%u&totalVolumeLiters=%.3f&avgFlowRateLpm=%.3f&totalVolumeMl=%.1f&accountingVolumeMl=%.1f&nominalFlowRateMlPerMin=%.1f&flowDetected=%s&flowStatus=%s&createdAt=%s&updatedAt=%s",
                FIREBASE_SAVE_EVENT_URL,
                getDeviceIdFromMac().c_str(),
                getOwnerUid().c_str(),
                payload.eventId,
                payload.startAtIso,
                payload.endAtIso,
                payload.durationSec,
                payload.triggerType,
                payload.stopReason,
                payload.success ? "true" : "false",
                payload.soilHumidity,
                payload.airHumidity,
                payload.temperature,
                payload.waterLevel,
                (unsigned)payload.totalPulses,
                payload.totalVolumeLiters,
                payload.avgFlowRateLpm,
                payload.totalVolumeMl,
                payload.accountingVolumeMl,
                payload.nominalFlowRateMlPerMin,
                payload.flowDetected ? "true" : "false",
                payload.flowStatus,
                payload.startAtIso,
                payload.endAtIso
            );

            Serial.print("[FIREBASE] Enviando evento (GET): ");
            Serial.println(url);

            WiFiClientSecure httpsClient;
            httpsClient.setInsecure();
            httpsClient.setTimeout(10);

            HTTPClient http;
            http.begin(httpsClient, url);
            http.setTimeout(5000);

            int httpCode = http.GET();

            if (httpCode > 0) {
                if (httpCode == 200) {
                    fwLogf("INFO", "FIREBASE", "Evento gravado: %s", payload.eventId);
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
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ==================== ENVIO PARA FIREBASE ====================

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
    auto parseTriggerType = [](const char* value) -> IrrigationTriggerType {
        if (!value) {
            return TRIGGER_UNKNOWN;
        }
        String trigger = String(value);
        trigger.trim();
        trigger.toLowerCase();

        if (trigger == "manual") return TRIGGER_MANUAL;
        if (trigger == "automatic" || trigger == "automatico" || trigger == "auto") return TRIGGER_AUTOMATIC;
        if (trigger == "schedule" || trigger == "scheduled" || trigger == "agendado") return TRIGGER_SCHEDULE;
        return TRIGGER_UNKNOWN;
    };

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

    IrrigationEvent event;
    event.used = true;
    strncpy(event.eventId, eventId, sizeof(event.eventId) - 1);
    event.startAt = (time_t)atol(startAtIso);
    event.endAt = (time_t)atol(endAtIso);
    event.durationSec = durationSec;
    event.trigger = parseTriggerType(triggerType);
    event.stopReason = stopReason;
    event.totalPulses = totalPulses;
    event.totalVolumeLiters = totalVolumeLiters;
    event.avgFlowRateLpm = avgFlowRateLpm;
    event.totalVolumeMl = totalVolumeMl;
    event.accountingVolumeMl = accountingVolumeMl;
    event.nominalFlowRateMlPerMin = nominalFlowRateMlPerMin;
    event.flowDetected = flowDetected;
    strncpy(event.flowStatus, flowStatus ? flowStatus : "", sizeof(event.flowStatus) - 1);

    String payload = FirebasePayloadBuilder::buildFullPayload(
        event,
        soilHumidity,
        airHumidity,
        temperature,
        waterLevel
    );

    FirebaseEventPayload firebasePayload = {};
    strncpy(firebasePayload.eventId, eventId, sizeof(firebasePayload.eventId) - 1);
    strncpy(firebasePayload.startAtIso, startAtIso, sizeof(firebasePayload.startAtIso) - 1);
    strncpy(firebasePayload.endAtIso, endAtIso ? endAtIso : "", sizeof(firebasePayload.endAtIso) - 1);
    strncpy(firebasePayload.triggerType, triggerType, sizeof(firebasePayload.triggerType) - 1);
    strncpy(firebasePayload.stopReason,
            g_eventMgr->getStopReasonString(stopReason),
            sizeof(firebasePayload.stopReason) - 1);
    firebasePayload.success = (stopReason == STOP_REASON_COMPLETED);
    firebasePayload.durationSec = durationSec;
    firebasePayload.soilHumidity = soilHumidity;
    firebasePayload.airHumidity = airHumidity;
    firebasePayload.temperature = temperature;
    firebasePayload.totalPulses = totalPulses;
    firebasePayload.totalVolumeLiters = totalVolumeLiters;
    firebasePayload.avgFlowRateLpm = avgFlowRateLpm;
    firebasePayload.totalVolumeMl = totalVolumeMl;
    firebasePayload.accountingVolumeMl = accountingVolumeMl;
    firebasePayload.nominalFlowRateMlPerMin = nominalFlowRateMlPerMin;
    firebasePayload.flowDetected = flowDetected;
    strncpy(firebasePayload.flowStatus, flowStatus ? flowStatus : "", sizeof(firebasePayload.flowStatus) - 1);
    strncpy(firebasePayload.waterLevel, waterLevel ? "Cheio" : "Vazio", sizeof(firebasePayload.waterLevel) - 1);
    firebasePayload.waterLevel[sizeof(firebasePayload.waterLevel) - 1] = '\0';
    
    strncpy(firebasePayload.jsonPayload, payload.c_str(), sizeof(firebasePayload.jsonPayload) - 1);
    firebasePayload.jsonPayload[sizeof(firebasePayload.jsonPayload) - 1] = '\0';

    Serial.print("[FIREBASE-QUEUE] Evento enfileirado - ID: ");
    Serial.print(eventId);
    Serial.print(" | Payload size: ");
    Serial.println(payload.length());

    if (xQueueSend(firebaseQueue, &firebasePayload, 0) != pdTRUE) {
        fwLogLine("WARN", "FIREBASE", "Fila cheia; evento descartado");
    }
}

// ==================== UNIFICACAO DE PARSERS DE AGENDAMENTO (CORRIGIDO) ====================

static int weekdayFromName(const String& nameRaw) {
    String name = nameRaw;
    name.trim();
    name.toLowerCase();

    if (name == "0" || name == "1" || name == "sun" || name == "sunday" || name == "domingo") return 0;
    if (name == "2" || name == "mon" || name == "monday" || name == "segunda" || name == "segunda-feira") return 1;
    if (name == "3" || name == "tue" || name == "tuesday" || name == "terca" || name == "terça" || name == "terca-feira" || name == "terça-feira") return 2;
    if (name == "4" || name == "wed" || name == "wednesday" || name == "quarta" || name == "quarta-feira") return 3;
    if (name == "5" || name == "thu" || name == "thursday" || name == "quinta" || name == "quinta-feira") return 4;
    if (name == "6" || name == "fri" || name == "friday" || name == "sexta" || name == "sexta-feira") return 5;
    if (name == "7" || name == "sat" || name == "saturday" || name == "sabado" || name == "sábado") return 6;

    return -1;
}

// Único parser de diasSemana responsável agora por strings de texto E inteiros, 
// prevenindo perda de máscara em sincronização HTTP e Push MQTT
static uint8_t extractDiasSemanaMask(const char* json, int maxLen) {
    if (!json || maxLen <= 0) return 0;
    
    const char* keyPos = strstr(json, "\"diasSemana\"");
    if (!keyPos || (keyPos - json) > maxLen) return 0;

    const char* openBracket = strchr(keyPos, '[');
    if (!openBracket || (openBracket - json) > maxLen) return 0;

    const char* closeBracket = strchr(openBracket, ']');
    if (!closeBracket || (closeBracket - json) > maxLen) return 0;

    uint8_t mask = 0;
    const char* p = openBracket + 1;

    while (p < closeBracket) {
        while (p < closeBracket && (*p == ' ' || *p == '\t' || *p == ',')) {
            p++;
        }
        if (p >= closeBracket) break;

        if (*p == '"') {
            const char* quoteEnd = strchr(p + 1, '"');
            if (quoteEnd && quoteEnd <= closeBracket) {
                String token;
                token.reserve(quoteEnd - p);
                for (const char* t = p + 1; t < quoteEnd; t++) {
                    token += *t;
                }
                int w = weekdayFromName(token);
                if (w >= 0) mask |= (1U << w);
                p = quoteEnd + 1;
                continue;
            }
        }

        if (*p >= '0' && *p <= '9') {
            int val = 0;
            while (p < closeBracket && *p >= '0' && *p <= '9') {
                val = (val * 10) + (*p - '0');
                p++;
            }
            if (val >= 1 && val <= 7) {
                mask |= (1U << (val - 1));
            }
            continue;
        }
        p++;
    }
    return mask;
}

static bool syncParseString(const char* json, int jsonLen, const char* key, char* outBuf, int outBufLen) {
    char token[72];
    snprintf(token, sizeof(token), "\"%s\":\"", key);
    const char* pos = strstr(json, token);
    if (!pos) return false;
    pos += strlen(token);
    int i = 0;
    while (pos[i] != '"' && pos[i] != '\0' && i < (outBufLen - 1)) {
        outBuf[i] = pos[i];
        i++;
    }
    outBuf[i] = '\0';
    return i > 0;
}

static bool syncParseBool(const char* json, const char* key, bool* outVal) {
    char token[72];
    snprintf(token, sizeof(token), "\"%s\":", key);
    const char* pos = strstr(json, token);
    if (!pos) return false;
    pos += strlen(token);
    while (*pos == ' ') pos++;
    if (strncmp(pos, "true", 4) == 0)  { *outVal = true;  return true; }
    if (strncmp(pos, "false", 5) == 0) { *outVal = false; return true; }
    return false;
}

static bool syncParseInt(const char* json, const char* key, int* outVal) {
    char token[72];
    snprintf(token, sizeof(token), "\"%s\":", key);
    const char* pos = strstr(json, token);
    if (!pos) return false;
    pos += strlen(token);
    while (*pos == ' ' || *pos == '"') pos++;
    if (*pos == '\0') return false;
    char* end = nullptr;
    long val = strtol(pos, &end, 10);
    if (end == pos) return false;
    *outVal = (int)val;
    return true;
}

static const char* syncNextObject(const char* start, int* outLen) {
    const char* p = start;
    while (*p && *p != '{') p++;
    if (!*p) return nullptr;
    const char* begin = p;
    int depth = 0;
    bool inStr = false;
    char prev = 0;
    while (*p) {
        if (!inStr) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { *outLen = (int)(p - begin + 1); return begin; } }
            else if (*p == '"') inStr = true;
        } else {
            if (*p == '"' && prev != '\\') inStr = false;
        }
        prev = *p;
        p++;
    }
    return nullptr;
}

static int syncParseSchedulesJson(const char* json, int jsonLen, FirestoreScheduleEntry* outEntries, int maxEntries) {
    if (!json || jsonLen <= 0 || !outEntries || maxEntries <= 0) return 0;

    const char* arrayKey = strstr(json, "\"schedules\":");
    if (!arrayKey) return 0;
    const char* bracket = strchr(arrayKey, '[');
    if (!bracket) return 0;

    int count = 0;
    const char* p = bracket + 1;

    while (count < maxEntries) {
        int objLen = 0;
        const char* obj = syncNextObject(p, &objLen);
        if (!obj) break;

        FirestoreScheduleEntry& e = outEntries[count];
        memset(&e, 0, sizeof(e));

        if (!syncParseString(obj, objLen, "id", e.id, sizeof(e.id))) {
            p = obj + objLen;
            continue;
        }

        char hora[16] = {0};
        if (!syncParseString(obj, objLen, "horaAcionamento", hora, sizeof(hora))) {
            syncParseString(obj, objLen, "hora", hora, sizeof(hora));
        }
        int h = 0, m = 0;
        if (sscanf(hora, "%d:%d", &h, &m) == 2 && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
            e.hour = h;
            e.minute = m;
        } else {
            p = obj + objLen;
            continue;
        }

        int dur = 0;
        if (!syncParseInt(obj, "tempoAcionamento", &dur)) {
            syncParseInt(obj, "duracaoSegundos", &dur);
        }
        if (dur <= 0) { p = obj + objLen; continue; }
        if (dur > DURACAO_IRRIGACAO_MAXIMA) dur = DURACAO_IRRIGACAO_MAXIMA;
        e.durationSeconds = dur;

        bool ativo = false;
        syncParseBool(obj, "ativo", &ativo);
        e.ativo = ativo;
        
        // ===== CORREÇÃO: Usando o parser unificado =====
        e.diasMask = extractDiasSemanaMask(obj, objLen);
        
        syncParseString(obj, objLen, "createdAt", e.createdAt, sizeof(e.createdAt));

        count++;
        p = obj + objLen;
    }

    return count;
}

void MqttManager::syncSchedulesFromFirestore() {
    if (_scheduleSyncRunning) {
        fwLogLine("INFO", "SYNC", "Sync ja em andamento; ignorando disparo duplo");
        return;
    }
    _scheduleSyncRunning = true;
    _scheduleSyncPending = false;

    if (WiFi.status() != WL_CONNECTED) {
        fwLogLine("WARN", "SYNC", "Sem Wi-Fi; sync adiado");
        _scheduleSyncRunning = false;
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s?deviceId=%s", FIREBASE_GET_DEVICE_SCHEDULES_URL, cachedDeviceId);

    fwLogf("INFO", "SYNC", "Buscando agendamentos: %s", url);

    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    httpsClient.setTimeout(10);

    HTTPClient http;
    http.begin(httpsClient, url);
    http.setTimeout(8000);

    int httpCode = http.GET();

    if (httpCode != 200) {
        fwLogf("WARN", "SYNC", "HTTP %d ao buscar agendamentos; sync cancelado", httpCode);
        http.end();
        _scheduleSyncRunning = false;
        return;
    }

    String body = http.getString();
    http.end();

    if (body.length() == 0) {
        fwLogLine("WARN", "SYNC", "Resposta vazia; sync cancelado");
        _scheduleSyncRunning = false;
        return;
    }

    FirestoreScheduleEntry firestoreSchedules[MAX_FIRESTORE_SCHEDULES];
    int firestoreCount = syncParseSchedulesJson(body.c_str(), (int)body.length(), firestoreSchedules, MAX_FIRESTORE_SCHEDULES);

    fwLogf("INFO", "SYNC", "Firestore retornou %d agendamento(s)", firestoreCount);

    char idsToRemove[4][64] = {};
    int removeCount = 0;

    taskENTER_CRITICAL(&mqttConnectMux);
    for (int i = 0; i < atuador.getMaxSchedules() && removeCount < 4; i++) {
        const char* nvsId = atuador.getScheduleId(i);
        if (nvsId == nullptr) continue;

        bool foundInFirestore = false;
        for (int j = 0; j < firestoreCount; j++) {
            if (strcmp(firestoreSchedules[j].id, nvsId) == 0) {
                foundInFirestore = true;
                break;
            }
        }
        if (!foundInFirestore) {
            strncpy(idsToRemove[removeCount++], nvsId, 63);
        }
    }
    taskEXIT_CRITICAL(&mqttConnectMux);

    for (int k = 0; k < removeCount; k++) {
        fwLogf("INFO", "SYNC", "Removendo da NVS (ausente no Firestore): %s", idsToRemove[k]);
        atuador.removeSchedule(idsToRemove[k]);
    }

    int upserted = 0;
    int rejected = 0;
    for (int j = 0; j < firestoreCount; j++) {
        const FirestoreScheduleEntry& fe = firestoreSchedules[j];
        bool ok = atuador.upsertSchedule(
            fe.id,
            fe.ativo,
            fe.diasMask,
            fe.hour,
            fe.minute,
            fe.durationSeconds,
            fe.createdAt[0] != '\0' ? fe.createdAt : nullptr
        );
        if (ok) {
            upserted++;
        } else {
            rejected++;
            fwLogf("WARN", "SYNC", "Slot cheio; agendamento rejeitado: %s", fe.id);
        }
    }

    fwLogf("INFO", "SYNC", "Sync concluido: %d upserted, %d rejected. NVS agora tem %d agendamento(s)",
           upserted, rejected, atuador.getScheduleCount());

    _scheduleSyncRunning = false;
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

static bool findFirstKey(const String& msg, const char* key, int* outKeyPos) {
    String token = String("\"") + key + "\"";
    int searchFrom = 0;
    const int len = msg.length();

    while (true) {
        int pos = msg.indexOf(token, searchFrom);
        if (pos < 0) return false;

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

static bool parseFloatKey(const String& msg, const char* key, float* outVal) {
    int keyPos = -1;
    if (!findFirstKey(msg, key, &keyPos)) return false;
    int colonPos = msg.indexOf(':', keyPos);
    if (colonPos < 0) return false;
    int i = colonPos + 1;
    int len = msg.length();
    while (i < len && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '"')) i++;
    int start = i;
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

    // ===== CORREÇÃO: Usando o parser unificado =====
    uint8_t diasMask = extractDiasSemanaMask(scheduleJson.c_str(), scheduleJson.length());
    
    String createdAt;
    parseStringByKey(scheduleJson, "createdAt", createdAt);

    bool ok = atuador.upsertSchedule(scheduleId.c_str(), ativo, diasMask, hour, minute, duration, createdAt.c_str());

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

static void publishTelemetry(const SensorData& data) {
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

            atuador.flushConfig();

            Serial.println("[CONFIG] setConfig aplicado");
            return;
        }

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

bool connectMQTT() {
    if (client.connected()) return true;

    bool alreadyRunning = false;
    taskENTER_CRITICAL(&mqttConnectMux);
    alreadyRunning = connectTaskRunning;
    if (!alreadyRunning) {
        connectTaskRunning = true;
    }
    taskEXIT_CRITICAL(&mqttConnectMux);

    if (alreadyRunning) {
        Serial.println("[MQTT] Conexao async ja em andamento");
        return false;
    }

    Serial.println("[MQTT] Iniciando task assíncrona de conexão MQTT...");

    BaseType_t result = xTaskCreatePinnedToCore(
        mqttConnectTask,
        "mqttConnect",
        MQTT_CONNECT_TASK_STACK_BYTES,
        nullptr,
        1,
        nullptr,
        0
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

static void mqttConnectTask(void* pvParameters) {
    fwLogSection("MQTT", "Reconexao ao broker");
    fwLogf("INFO", "MQTT", "freeHeap=%lu", (unsigned long)ESP.getFreeHeap());
    fwLogf("INFO", "MQTT", "minFreeHeap=%lu", (unsigned long)ESP.getMinFreeHeap());

    fwLogLine("INFO", "MQTT", "Tentativa unica de conexao com o broker");

    mqttConnectingInProgress = true;

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
        mqttJustConnected = true;
    } else {
        fwLogLine("WARN", "MQTT", "Falha ao conectar; nova tentativa apenas apos backoff exponencial");
    }

    mqttConnectingInProgress = false;

    if (!client.connected()) {
        fwLogLine("WARN", "MQTT", "Broker indisponivel no momento");
    }

    taskENTER_CRITICAL(&mqttConnectMux);
    connectTaskRunning = false;
    taskEXIT_CRITICAL(&mqttConnectMux);

    vTaskDelay(pdMS_TO_TICKS(20));

    vTaskDelete(nullptr);
}

// ================== INIT ==================

void MqttManager::begin(IrrigationEventManager& eventMgr) {
    mqttManagerInstance = this;
    _eventMgr = &eventMgr;
    g_eventMgr = &eventMgr;

    espClient.setInsecure();

    clientId = "ESP32_" + WiFi.macAddress();
    refreshMqttTopicCache();

    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setBufferSize(4096);
    client.setCallback(callback);
    client.setKeepAlive(10);
    client.setSocketTimeout(15);

    firebaseQueue = xQueueCreate(4, sizeof(FirebaseEventPayload));
    if (firebaseQueue == nullptr) {
        fwLogLine("ERROR", "FIREBASE", "Falha ao criar fila FreeRTOS");
        return;
    }

    scheduleSyncQueue = xQueueCreate(1, sizeof(uint8_t));
    if (scheduleSyncQueue == nullptr) {
        fwLogLine("ERROR", "SYNC", "Falha ao criar fila de sync FreeRTOS");
        return;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        firebaseTask,
        "firebaseHTTP",
        20480,
        nullptr,
        1,
        nullptr,
        0
    );

    if (result != pdPASS) {
        fwLogLine("ERROR", "FIREBASE", "Falha ao criar task FreeRTOS");
    } else {
        fwLogLine("INFO", "FIREBASE", "Task HTTP iniciada no Core 0 (stack=20KB)");
    }

    BaseType_t syncResult = xTaskCreatePinnedToCore(
        scheduleSyncTask,
        "scheduleSync",
        20480,
        nullptr,
        1,
        nullptr,
        0
    );

    if (syncResult != pdPASS) {
        fwLogLine("ERROR", "SYNC", "Falha ao criar task de sync de agendamentos");
    } else {
        fwLogLine("INFO", "SYNC", "Task scheduleSync iniciada no Core 0 (stack=20KB)");
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

    if (mqttJustConnected) {
        mqttJustConnected = false;

        client.subscribe(cachedCommandsTopic);
        client.publish(cachedStatusTopic, "online", true);
        fwLogf("INFO", "MQTT", "Subscribed em %s", cachedCommandsTopic);

        lastSolo = -1;
        lastTemp = -1000;
        lastUmidAr = -1000;
        SensorData postConnectData = sensores.read();
        lastNivelAgua = !postConnectData.nivelAgua;
        lastBomba = !atuador.isLigado();
        pendingSensorStability = false;
        sensorStabilitySince = 0;
        candidateSolo = -1;
        candidateTemp = -1000;
        candidateUmidAr = -1000;
        mqttWasConnected = true;
        reconnectBackoffMs = 5000UL;

        if (scheduleSyncQueue != nullptr && !_scheduleSyncRunning) {
            uint8_t trigger = 1;
            _scheduleSyncPending = true;
            xQueueOverwrite(scheduleSyncQueue, &trigger);
            fwLogLine("INFO", "SYNC", "Sincronizacao de agendamentos disparada apos reconexao MQTT");
        }
    }

    if (!mqttConnectingInProgress) {
        client.loop();
    }
    
    flushPendingStatusPublish();

    SensorData data = sensores.read();
    unsigned long now = millis();

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

    if (nivelAguaMudou || bombaMudou) {
        publicarTelemetria = true;
    }

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
    if (g_eventMgr && (mqttWasConnected || client.connected())) {
        IrrigationEvent event;
        while (g_eventMgr->getNextPendingEvent(&event)) {
            char startIso[32];
            char endIso[32];

            g_eventMgr->formatIso8601(event.startAt, startIso, sizeof(startIso));
            g_eventMgr->formatIso8601(event.endAt, endIso, sizeof(endIso));

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
            }

            if (!isAlreadyEnqueuedForFirebase(event.eventId)) {
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
                markEnqueuedForFirebase(event.eventId);
            }

            if (!mqttPublished) {
                break;
            }

            g_eventMgr->markEventSent(event.eventId);
        }
    }

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

    char topic[128];
    String deviceId = getDeviceIdFromMac();
    snprintf(topic, sizeof(topic), "irrigahome/%s/events/irrigation", deviceId.c_str());

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

void MqttManager::requestStopIrrigation(IrrigationStopReason reason) {
    if (!_stopRequested || reason > _stopReason) {
        _stopReason = reason;
        _stopRequested = true;
    }
}

bool MqttManager::isStopRequested() const {
    return _stopRequested;
}

IrrigationStopReason MqttManager::consumeStopRequest() {
    IrrigationStopReason r = _stopReason;
    _stopRequested = false;
    _stopReason = STOP_REASON_COMPLETED;
    return r;
}