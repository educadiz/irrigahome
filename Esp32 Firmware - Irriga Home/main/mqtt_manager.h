// Responsabilidade: declarar a interface de comunicacao MQTT.
// O que faz: expor inicializacao, manutencao do loop MQTT e consulta de conexao.

#pragma once
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "irrigation_event_manager.h"  // Para IrrigationStopReason e IrrigationEventManager

class MqttManager {
public:
    // Recebe referencia explicita ao IrrigationEventManager para eliminar o
    // acoplamento implicito via `extern`. O compilador garante que a instancia
    // correta e' usada mesmo que main.ino seja refatorado.
    void begin(IrrigationEventManager& eventMgr);
    void loop();
    bool isConnected();

    // Publicar evento de irrigação completada (via MQTT)
    bool publishIrrigationEvent(const char* eventId, const char* startAtIso,
                                const char* endAtIso, int durationSec,
                                const char* triggerType, const char* stopReason,
                                uint32_t totalPulses = 0, float totalVolumeLiters = 0.0,
                                float avgFlowRateLpm = 0.0, bool flowDetected = false,
                                const char* flowStatus = "",
                                float totalVolumeMl = 0.0,
                                float accountingVolumeMl = 0.0,
                                float nominalFlowRateMlPerMin = 0.0);

    // Enviar evento para Firebase via HTTP (executado na firebaseTask — não bloqueia).
    // getStopReasonString() e' chamado aqui (antes do enfileiramento) e nao dentro
    // da task, garantindo que nenhum acesso a IrrigationEventManager ocorra em outra
    // thread apos o dado ter sido copiado por valor para FirebaseEventPayload.
    void sendIrrigationEventToFirebase(const char* eventId, const char* startAtIso,
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

    // Sinalização de parada para o controlador central (main.ino).
    // O mqtt_manager DETECTA as condições mas nunca age diretamente sobre
    // atuador ou irrigationEventManager — apenas sinaliza via estes métodos.
    // main.ino consome o sinal na próxima iteração do loop de controle (2 s).
    void requestStopIrrigation(IrrigationStopReason reason);
    bool isStopRequested() const;
    IrrigationStopReason consumeStopRequest();

private:
    bool              _stopRequested = false;
    IrrigationStopReason _stopReason = STOP_REASON_COMPLETED;
    // Referencia ao gerenciador de eventos injetada em begin().
    // Ponteiro (e nao referencia de membro) para permitir inicializacao tardia.
    IrrigationEventManager* _eventMgr = nullptr;
};
