// Responsabilidade: monitorar e registrar eventos de irrigação com timestamps.
// O que faz: detectar início/fim de irrigação, gerar eventos com ISO 8601,
// persistir estado, manter fila MQTT offline e integrar com MQTT manager.

#pragma once
#include "actuator_manager.h"  // Para obter IrrigationTriggerType
#include <time.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Máximo de eventos na fila MQTT offline
#define MAX_IRRIGATION_QUEUE 8

// Motivo de encerramento da irrigação
typedef enum {
    STOP_REASON_COMPLETED = 0,  // Encerrou normalmente pelo tempo programado
    STOP_REASON_MANUAL    = 1,  // Desligado manualmente pelo usuário
    STOP_REASON_NO_WATER  = 2,  // Reservatório vazio durante irrigação
    STOP_REASON_NO_FLOW   = 3,  // Bomba ligada sem fluxo detectado após timeout
} IrrigationStopReason;

// Estrutura de um evento de irrigação completo
struct IrrigationEvent {
    bool used;
    char eventId[24];               // timestamp-based ID (e.g., "1708612345-a1b2")
    time_t startAt;                 // Unix timestamp do início
    time_t endAt;                   // Unix timestamp do fim (0 se ainda ativo)
    int durationSec;                // Duração em segundos
    IrrigationTriggerType trigger;  // Tipo de acionamento
    IrrigationStopReason stopReason; // Motivo de encerramento
    bool sentToMqtt;                // Flag: já foi enviado ao MQTT
    // Dados do sensor de vazão
    uint32_t totalPulses;          // Contagem bruta de pulsos durante o evento
    float totalVolumeLiters;       // Litros totais durante o evento
    float avgFlowRateLpm;          // Vazão média em L/min
    float totalVolumeMl;           // Volume medido pelo sensor em mL
    float accountingVolumeMl;      // Volume contábil baseado na vazão nominal da bomba
    float nominalFlowRateMlPerMin; // Vazão nominal configurada para contabilidade (mL/min)
    bool flowDetected;             // True se houve fluxo
    char flowStatus[16];           // "OK", "ZERO_FLOW", "ANOMALY"
};

// Estrutura resumida de histórico persistido
struct IrrigationHistoryEntry {
    char eventId[24];
    time_t startAt;
    time_t endAt;
    int durationSec;
    IrrigationTriggerType trigger;
    IrrigationStopReason stopReason; // Motivo de encerramento
    bool sentToFirebase;             // true somente após HTTP 200 confirmado
    // resumo de vazão para histórico
    uint32_t totalPulses;
    float totalVolumeLiters;
    float avgFlowRateLpm;
    float totalVolumeMl;
    float accountingVolumeMl;
    float nominalFlowRateMlPerMin;

    // Derivado: sucesso significa encerramento normal pelo tempo programado.
    // Qualquer outro motivo (reservatório vazio, interrupção manual) indica
    // que a irrigação não concluiu o ciclo planejado.
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
    // Inicialização
    void begin();

    // Notificações de evento (chamadas pela actuator_manager)
    void onIrrigationStart(IrrigationTriggerType trigger);
    // reason com valor padrão: chamadas existentes sem argumento continuam funcionando
    void onIrrigationEnd(IrrigationStopReason reason = STOP_REASON_COMPLETED);

    // Loop de processamento (deve ser chamado periodicamente no main loop)
    void update();

    // Consultas de estado
    bool isIrrigationActive();
    int getActiveIrrigationSeconds();
    const char* getTriggerTypeString(IrrigationTriggerType trigger);
    const char* getStopReasonString(IrrigationStopReason reason);

    // Fila de eventos para MQTT
    int getPendingEventCount();
    bool getNextPendingEvent(IrrigationEvent* outEvent);
    void markEventSent(const char* eventId);

    // Remove evento do historyBuffer e do JSONL.
    // Chamado SOMENTE após HTTP 200 confirmado pelo Firebase. Thread-safe.
    void removeFromHistory(const char* eventId);

    // Histórico persistido de eventos concluídos
    int getHistoryCount();
    bool getHistoryEvent(int index, IrrigationHistoryEntry* outEvent);

    // Utilitários
    static bool formatIso8601(time_t timestamp, char* outBuffer, int bufferSize);
    static char* generateEventId(char* outBuffer, int bufferSize);

private:
    ActiveIrrigationState activeState;
    IrrigationEvent eventQueue[MAX_IRRIGATION_QUEUE];
    static const int MAX_IRRIGATION_HISTORY = 10;
    IrrigationHistoryEntry historyBuffer[MAX_IRRIGATION_HISTORY];
    int historyCount;

    // Persistência em LittleFS
    bool loadState();
    void saveState();
    void clearState();
    bool loadHistory();
    void saveHistory();
    void addHistoryEvent(const IrrigationEvent& event);

    // Mutex FreeRTOS: protege historyBuffer e LittleFS entre Core 0 e Core 1.
    SemaphoreHandle_t _historyMutex = nullptr;

    // Controle de IDs já enviados ao MQTT (evita reenvio duplicado após reload)
    static const int MAX_SENT_IDS = 40;
    char sentIds[MAX_SENT_IDS][24];
    int sentIdsCount = 0;
    bool isAlreadySent(const char* eventId) const;
    void markIdAsSent(const char* eventId);

    // Geração de event ID
    void generateNewEventId();
};

// Instância global (será inicializada em main.ino)
extern IrrigationEventManager irrigationEventManager;
