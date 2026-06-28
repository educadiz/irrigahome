// Responsabilidade: monitorar e registrar eventos de irrigação com timestamps.
// O que faz: detectar início/fim de irrigação, gerar eventos com ISO 8601,
// persistir estado, manter fila MQTT offline e integrar com MQTT manager.

#pragma once
#include "actuator_manager.h"
#include <time.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Máximo de eventos na fila MQTT offline
#define MAX_IRRIGATION_QUEUE 8

// Motivo de encerramento da irrigação
typedef enum {
    STOP_REASON_COMPLETED = 0,
    STOP_REASON_MANUAL    = 1,
    STOP_REASON_NO_WATER  = 2,
    STOP_REASON_NO_FLOW   = 3,
} IrrigationStopReason;

// Estrutura de um evento de irrigação completo
struct IrrigationEvent {
    bool used;
    char eventId[24];
    time_t startAt;
    time_t endAt;
    int durationSec;
    IrrigationTriggerType trigger;
    IrrigationStopReason stopReason;
    bool sentToMqtt;
    uint32_t totalPulses;
    float totalVolumeLiters;
    float avgFlowRateLpm;
    float totalVolumeMl;
    float accountingVolumeMl;
    float nominalFlowRateMlPerMin;
    bool flowDetected;
    char flowStatus[16];
};

// Estrutura resumida de histórico persistido
struct IrrigationHistoryEntry {
    char eventId[24];
    time_t startAt;
    time_t endAt;
    int durationSec;
    IrrigationTriggerType trigger;
    IrrigationStopReason stopReason;
    bool sentToFirebase;
    uint32_t totalPulses;
    float totalVolumeLiters;
    float avgFlowRateLpm;
    float totalVolumeMl;
    float accountingVolumeMl;
    float nominalFlowRateMlPerMin;

    bool isSuccess() const { return stopReason == STOP_REASON_COMPLETED; }
};

// Estado atual de irrigação
struct ActiveIrrigationState {
    bool isActive;
    time_t startAt;
    IrrigationTriggerType trigger;
    char eventId[24];
};

class IrrigationEventManager {
public:
    void begin();
    void onIrrigationStart(IrrigationTriggerType trigger);
    void onIrrigationEnd(IrrigationStopReason reason = STOP_REASON_COMPLETED);
    void update();

    bool isIrrigationActive();
    int getActiveIrrigationSeconds();
    const char* getTriggerTypeString(IrrigationTriggerType trigger);
    const char* getStopReasonString(IrrigationStopReason reason);

    int getPendingEventCount();
    bool getNextPendingEvent(IrrigationEvent* outEvent);
    void markEventSent(const char* eventId);
    void removeFromHistory(const char* eventId);

    int getHistoryCount();
    bool getHistoryEvent(int index, IrrigationHistoryEntry* outEvent);

    static bool formatIso8601(time_t timestamp, char* outBuffer, int bufferSize);
    static char* generateEventId(char* outBuffer, int bufferSize);

private:
    ActiveIrrigationState activeState;
    IrrigationEvent eventQueue[MAX_IRRIGATION_QUEUE];
    static const int MAX_IRRIGATION_HISTORY = 10;
    IrrigationHistoryEntry historyBuffer[MAX_IRRIGATION_HISTORY];
    int historyCount;

    bool loadState();
    void saveState();
    void clearState();
    bool loadHistory();
    void saveHistory();
    void addHistoryEvent(const IrrigationEvent& event);

    SemaphoreHandle_t _historyMutex = nullptr;

    static const int MAX_SENT_IDS = 40;
    char sentIds[MAX_SENT_IDS][24];
    int sentIdsCount = 0;
    bool isAlreadySent(const char* eventId) const;
    void markIdAsSent(const char* eventId);

    void generateNewEventId();
};

extern IrrigationEventManager irrigationEventManager;