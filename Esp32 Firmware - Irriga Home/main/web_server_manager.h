// Responsabilidade: declarar a interface do servidor HTTP local de manutencao.
// O que faz: expoe begin() para inicializacao e applyPendingConfig() para aplicacao
// thread-safe de configuracoes recebidas via POST no loop principal (Core 1).

#pragma once
#include "sensor_manager.h"
#include "actuator_manager.h"
#include "flow_meter_manager.h"
#include "irrigation_event_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WebServerManager {
public:
    // Injeta referencias aos managers e inicia a task FreeRTOS no Core 0.
    // Deve ser chamado em setup() apos wifi.connect().
    void begin(SensorManager& sensors, ActuatorManager& actuator,
               FlowMeterManager& flow,
               IrrigationEventManager& eventManager);

    // Aplica configuracoes pendentes recebidas via POST /api/config.
    // Deve ser chamado no loop() do main.ino — executa no Core 1, onde vivem
    // os managers. Nao-op quando nenhuma config esta pendente.
    void applyPendingConfig();

private:
    static void _task(void* pv);
    void        _run();

    void handleRoot();
    void handleData();
    void handleAuth();
    void handleLogout();
    void handleMeasure();
    void handleConfig();
    void handleResetSchedules();

    SensorManager* _sensors  = nullptr;
    ActuatorManager* _actuator = nullptr;
    FlowMeterManager* _flow     = nullptr;
    IrrigationEventManager* _eventManager = nullptr;

    // Mutex de hardware (Spinlock) para garantir barreira de memoria entre Core 0 e Core 1
    portMUX_TYPE _pendingMux = portMUX_INITIALIZER_UNLOCKED;

    // Estrutura sem volatile, pois o Spinlock ja garante a visibilidade correta
    struct PendingConfig {
        bool  pending    = false;
        float offTemp    = 0.0f;
        float offSolo    = 0.0f;
        float offUmidAr  = 0.0f;
        float flowScale  = 1.0f;
        float flowOffset = 0.0f;
    };
    PendingConfig _pending;
};