// Responsabilidade: orquestrar o ciclo principal do firmware.
// O que faz: inicializa os managers, processa leituras/safety, executa modo automatico
// (por regra e por agendamento) e atualiza display/MQTT no loop.

#include "wifi_manager.h"
#include <Preferences.h>  // necessário para limpeza emergencial da NVS
#include "mqtt_manager.h"
#include "sensor_manager.h"
#include "actuator_manager.h"
#include "display_manager.h"
#include "irrigation_event_manager.h"
#include "flow_meter_manager.h"
#include "web_server_manager.h"
#include "firmware_logger.h"
#include "config.h"
#include <time.h>
#include <esp_task_wdt.h>


WiFiManagerCustom wifi;
MqttManager mqtt;
SensorManager sensores;
ActuatorManager atuador;
DisplayManager displayManager;
IrrigationEventManager irrigationEventManager;
FlowMeterManager flowMeter;
WebServerManager webServer;

// ===========================================================================
// stopIrrigation — ponto único de desligamento da bomba.
//
// Centraliza as três operações que SEMPRE devem ocorrer juntas:
//   1. desligar pinos físicos
//   2. limpar o timer de duração
//   3. registrar o evento de fim com o motivo correto
//
// Chamada por: timeout de duração, segurança por falta d'água e comandos MQTT.
// O mqtt_manager.loop() NÃO executa mais essas operações diretamente;
// apenas sinaliza a condição via requestStopIrrigation() para que este
// controlador central aja na próxima iteração do loop de controle (2 s).
// ===========================================================================
void stopIrrigation(IrrigationStopReason reason) {
    if (!atuador.isLigado()) {
        return; // Bomba já desligada — garante idempotência.
    }
    atuador.desligar();
    atuador.setActiveUntil(0);
    irrigationEventManager.onIrrigationEnd(reason);
}

static unsigned long soloSecoDesde = 0;
static const unsigned long SOLO_SECO_PERSISTENCIA_MS = 60000UL;
static unsigned long soloUmidoAltoDesde = 0;
static unsigned long lastControlTick = 0;
static const unsigned long CONTROL_INTERVAL_MS = 2000UL;
static const unsigned long FLOW_FLOW_WARNING_TIMEOUT_MS = 5000UL;
static const unsigned long FLOW_NO_DETECTION_TIMEOUT_MS = 6000UL;
static const uint32_t WATCHDOG_TIMEOUT_SECONDS = 8;

void setup() {
    Serial.begin(115200);
    delay(500); // Give serial time to init
    fwLogSection("SETUP", "Inicializacao do firmware Irriga Home");

    // Inicializar watchdog apenas uma vez (evita TWDT already initialized)
    static bool wdtInitialized = false;
    if (!wdtInitialized) {
        wdtInitialized = true;

        esp_task_wdt_config_t wdtConfig = {
            .timeout_ms   = 30000UL,  // 30 s — cobre handshake TLS do HiveMQ Cloud
            .idle_core_mask = 0,      // NÃO monitora IDLE automaticamente
            .trigger_panic  = true,
        };
        esp_task_wdt_init(&wdtConfig);
        esp_task_wdt_add(NULL);  // registra apenas a loopTask (Core 1)
        fwLogf("INFO", "SETUP", "Watchdog configurado (30s, idle_core_mask=0)");
    }

    fwLogSection("SETUP", "Wi-Fi");
    wifi.connect();
    fwLogLine("INFO", "SETUP", "Gerenciador Wi-Fi inicializado");
    
    fwLogSection("SETUP", "Sensores");
    sensores.begin();
    fwLogLine("INFO", "SETUP", "Sensores inicializados");
    
    // ===== CARREGA O OWNER UID DO ARMAZENAMENTO =====
    // Se não existir, usa o valor padrão de secrets.h
    String ownerUid = getOwnerUid();
    fwLogf("INFO", "SETUP", "Owner UID: %s", ownerUid.c_str());
    
    //Isso aqui apaga registros indesejaveis na NVS e zera tudo!
    // ⚠️ LIMPEZA EMERGENCIAL DA NVS — remover após confirmar display "Agendado: 0"
    //{
        //Preferences _limpa;
        //_limpa.begin("irrigahome", false);
        //_limpa.clear();
        //_limpa.end();
        //Serial.println("[NVS] ⚠️ namespace irrigahome limpo — remova este bloco apos confirmar!");
    //}
    // ⚠️ FIM DA LIMPEZA EMERGENCIAL

    fwLogSection("SETUP", "Atuadores");
    atuador.begin();
    fwLogLine("INFO", "SETUP", "Atuadores inicializados");
    
    fwLogSection("SETUP", "Eventos de irrigacao");
    irrigationEventManager.begin();
    fwLogLine("INFO", "SETUP", "Gerenciador de eventos inicializado");
    
    fwLogSection("SETUP", "Flow meter");
    flowMeter.begin();
    fwLogLine("INFO", "SETUP", "Flow meter inicializado");
    
    fwLogSection("SETUP", "MQTT");
    mqtt.begin(irrigationEventManager);  // injeta referencia — elimina extern implicito
    fwLogLine("INFO", "SETUP", "Cliente MQTT inicializado");

    fwLogSection("SETUP", "Servidor web de manutencao");
    webServer.begin(sensores, atuador, flowMeter);
    fwLogLine("INFO", "SETUP", "Servidor web inicializado");
    
    fwLogSection("SETUP", "Display");
    displayManager.begin();
    fwLogLine("INFO", "SETUP", "Display inicializado");
    
    fwLogSection("SETUP", "Firmware pronto");
}

void loop() {

    wifi.loop();
    mqtt.loop();
    displayManager.update();
    irrigationEventManager.update();

    unsigned long now = millis();
    bool safetyStopIssued = false;
    static bool flowWarningIssued = false;

    if (!atuador.isLigado() || atuador.getPumpStartedAtMs() == 0 || flowMeter.hasFlowDetected()) {
        flowWarningIssued = false;
    } else {
        unsigned long elapsedSincePumpStart = (now >= atuador.getPumpStartedAtMs())
            ? (now - atuador.getPumpStartedAtMs())
            : 0;

        if (!flowWarningIssued && elapsedSincePumpStart >= FLOW_FLOW_WARNING_TIMEOUT_MS) {
            fwLogLine("WARN", "SAFETY", "Bomba ligada ha 5s sem fluxo; verificando pressurizacao");
            flowWarningIssued = true;
        }

        if (elapsedSincePumpStart >= FLOW_NO_DETECTION_TIMEOUT_MS) {
            fwLogLine("ERROR", "SAFETY", "Sem fluxo apos 6s; desligando bomba para protecao");
            stopIrrigation(STOP_REASON_NO_FLOW);
            safetyStopIssued = true;
        }
    }

    if (safetyStopIssued) {
        esp_task_wdt_reset();
        vTaskDelay(1);
        return;
    }

    if ((now - lastControlTick) < CONTROL_INTERVAL_MS) {
        vTaskDelay(1);
        return;
    }
    lastControlTick = now;

    // Alimentar watchdog ao iniciar o bloco de controle
    esp_task_wdt_reset();

    // Aplica calibrações recebidas via página web (executa no Core 1)
    webServer.applyPendingConfig();

    SensorData data = sensores.read();

    // ===== FIM DE IRRIGACAO (TIMEOUT) =====
    if (atuador.isLigado() && atuador.getActiveUntil() > 0 && now >= atuador.getActiveUntil()) {
        stopIrrigation(STOP_REASON_COMPLETED);
        fwLogLine("INFO", "IRR", "Tempo de irrigacao finalizado");
    }

    // ===== SEGURANCA (FIM POR FALTA DE AGUA) =====
    if (!data.nivelAgua || mqtt.isStopRequested()) {
        if (atuador.isLigado()) {
            IrrigationStopReason reason = !data.nivelAgua
                ? STOP_REASON_NO_WATER
                : mqtt.consumeStopRequest();
            stopIrrigation(reason);
            if (reason == STOP_REASON_NO_WATER) {
                fwLogLine("WARN", "SAFETY", "Nivel de agua baixo; bomba desligada");
            }
        } else {
            mqtt.consumeStopRequest(); // descarta sinal se bomba ja estava OFF
        }
    }

    // ===== MODO AUTOMATICO =====
    bool condicaoAutoValida = atuador.isModoAuto() && data.nivelAgua;
    if (!condicaoAutoValida) {
        soloSecoDesde = 0;
        soloUmidoAltoDesde = 0;
    }

    // ===== PROTECAO POR EXCESSO DE UMIDADE =====
    bool bloquearPorSoloUmido = false;
    if (atuador.isModoAuto() && data.nivelAgua) {
        if (data.umidadeSolo >= SOLO_UMIDO_BLOQUEIO_PCT) {
            if (soloUmidoAltoDesde == 0) soloUmidoAltoDesde = now;
            soloSecoDesde = 0;
        } else {
            soloUmidoAltoDesde = 0;
        }
        bloquearPorSoloUmido =
            (soloUmidoAltoDesde != 0) &&
            ((now - soloUmidoAltoDesde) >= SOLO_UMIDO_BLOQUEIO_PERSISTENCIA_MS);
    }

    // ===== AGENDAMENTOS =====
    if (wifi.isClockReady() && data.nivelAgua && !atuador.isLigado() && !bloquearPorSoloUmido) {
        int scheduleDuration = 0;
        const char* scheduleId = nullptr;

        if (atuador.checkDueSchedule(time(nullptr), &scheduleDuration, &scheduleId)) {
            int durationToUse = scheduleDuration > 0 ? scheduleDuration : atuador.getDuracaoSegundos();
            atuador.setCurrentTrigger(TRIGGER_SCHEDULE);
            atuador.ligar();
            irrigationEventManager.onIrrigationStart(TRIGGER_SCHEDULE);
            atuador.setActiveUntil(now + ((unsigned long)durationToUse * 1000UL));
            atuador.setLastScheduleTrigger(now);

            fwLogf("INFO", "SCHEDULE", "Disparado id=%s duracao=%ds", scheduleId != nullptr ? scheduleId : "unknown", durationToUse);
        }
    }

    // ===== AUTOMATICO POR REGRA DE SOLO =====
    if (condicaoAutoValida) {
        int threshold = atuador.getThresholdUmidade();
        int cooldown = atuador.getCooldownSegundos();
        unsigned long lastTrigger = atuador.getLastAutoTrigger();
        unsigned long timeSinceLastTrigger = (now >= lastTrigger) ? (now - lastTrigger) : 0;

        bool canTrigger = (timeSinceLastTrigger >= ((unsigned long)cooldown * 1000UL));

        if (data.umidadeSolo < threshold) {
            if (soloSecoDesde == 0) {
                soloSecoDesde = now;
            }

            bool secoPersistente = (now - soloSecoDesde) >= SOLO_SECO_PERSISTENCIA_MS;
            if (secoPersistente && !bloquearPorSoloUmido && !atuador.isLigado() && canTrigger) {
                atuador.setCurrentTrigger(TRIGGER_AUTOMATIC);
                atuador.ligar();
                irrigationEventManager.onIrrigationStart(TRIGGER_AUTOMATIC);
                atuador.setActiveUntil(now + ((unsigned long)atuador.getDuracaoSegundos() * 1000UL));
                atuador.setLastAutoTrigger(now);
                soloSecoDesde = 0;
                fwLogf("INFO", "AUTO", "Solo=%d%% abaixo do threshold=%d%%; iniciando irrigacao", data.umidadeSolo, threshold);
            }
        } else {
            soloSecoDesde = 0;
        }
    }

    // ===== ENVIO DE EVENTOS PARA O FIREBASE =====
    // O envio ao Firebase e' feito exclusivamente por MqttManager::loop()
    // (mqtt.loop(), chamado no topo deste loop()), via fila FreeRTOS
    // nao-bloqueante (firebaseQueue/firebaseTask). Esse e' o UNICO
    // consumidor de getNextPendingEvent() no firmware.
    //
    // ANTERIORMENTE havia um segundo consumidor aqui (FirebaseSender::
    // sendPendingEvents(), a cada 30s) que chamava a MESMA fila
    // (irrigationEventManager.getNextPendingEvent). Como getNextPendingEvent()
    // consome/avanca o item da fila, os dois consumidores competiam pelo
    // mesmo evento: o mqtt.loop() (que roda a cada poucos ms) quase sempre
    // "ganhava a corrida" e consumia o evento antes do ciclo de 30s deste
    // bloco rodar — fazendo o envio real depender de timing, em vez de ser
    // deterministico. Alem disso, o FirebaseSender usava HTTPClient sem
    // WiFiClientSecure configurado para a URL HTTPS, e fazia delay()
    // bloqueante dentro do loop principal (risco para o watchdog e para a
    // logica de seguranca da bomba). Por isso foi removido daqui; ver
    // firebase_sender.h — mantido no projeto apenas como utilitario
    // standalone, nao deve ser chamado a partir do loop() principal.

    // Reset do watchdog
    esp_task_wdt_reset();
}