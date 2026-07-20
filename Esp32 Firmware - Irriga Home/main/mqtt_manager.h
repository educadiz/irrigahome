// Responsabilidade: declarar a interface de comunicacao MQTT.
// O que faz: expor inicializacao, manutencao do loop MQTT e consulta de conexao.

#pragma once
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "irrigation_event_manager.h"
#include "firebase_payload_builder.h"  // ADICIONADO

class MqttManager {
public:
    void begin(IrrigationEventManager& eventMgr);
    void loop();
    bool isConnected();

    bool publishIrrigationEvent(const char* eventId, const char* startAtIso,
                                const char* endAtIso, int durationSec,
                                const char* triggerType, const char* stopReason,
                                uint32_t totalPulses = 0, float totalVolumeLiters = 0.0,
                                float avgFlowRateLpm = 0.0, bool flowDetected = false,
                                const char* flowStatus = "",
                                float totalVolumeMl = 0.0,
                                float accountingVolumeMl = 0.0,
                                float nominalFlowRateMlPerMin = 0.0);

    void syncSchedulesFromFirestore();

    bool sendIrrigationEventToFirebase(const char* eventId, const char* startAtIso,
                                       const char* endAtIso, int durationSec,
                                       const char* triggerType,
                                       IrrigationStopReason stopReason = STOP_REASON_COMPLETED,
                                       int soilHumidity = 0, int airHumidity = 0,
                                       float temperature = 0.0,
                                       bool waterLevel = true,
                                       uint32_t totalPulses = 0, float totalVolumeLiters = 0.0,
                                       float avgFlowRateLpm = 0.0, bool flowDetected = false,
                                       const char* flowStatus = "",
                                       float totalVolumeMl = 0.0,
                                       float accountingVolumeMl = 0.0,
                                       float nominalFlowRateMlPerMin = 0.0);

    void requestStopIrrigation(IrrigationStopReason reason);
    bool isStopRequested() const;
    IrrigationStopReason consumeStopRequest();

private:
    bool              _stopRequested = false;
    IrrigationStopReason _stopReason = STOP_REASON_COMPLETED;
    volatile bool     _scheduleSyncPending = false;
    volatile bool     _scheduleSyncRunning = false;
    IrrigationEventManager* _eventMgr = nullptr;
};