// Responsabilidade: declarar a API de controle do atuador e dos agendamentos.
// O que faz: disponibiliza operacoes da bomba/modo automatico, parametros de irrigacao,
// controle de tempo ativo e gerenciamento de agendas persistidas.

#pragma once
#include "config.h"
#include <stdint.h>
#include <time.h>

// Tipos de acionamento (deve sincronizar com irrigation_event_manager.h)
typedef enum {
    TRIGGER_UNKNOWN = 0,
    TRIGGER_MANUAL = 1,      // App ou comando MQTT direto
    TRIGGER_AUTOMATIC = 2,   // Regra local (sensor)
    TRIGGER_SCHEDULE = 3     // Agendamento
} IrrigationTriggerType;

struct IrrigationSchedule {
    bool used = false;
    // Firestore IDs or UUIDs can exceed 31 chars.
    char id[64] = {0};
    char createdAt[32] = {0};
    bool ativo = false;
    uint8_t diasMask = 0;
    int hour = 0;
    int minute = 0;
    int durationSeconds = 0;
    time_t lastTriggerAt = 0;
};

class ActuatorManager {
public:
    void begin();
    void ligar();
    void desligar();
    bool status();
    bool isLigado();
    void setModoAuto(bool enabled);
    bool isModoAuto();
    void setDuracaoSegundos(int seconds);
    int getDuracaoSegundos();
    void setThresholdUmidade(int threshold);
    int getThresholdUmidade();
    void setCooldownSegundos(int cooldown);
    int getCooldownSegundos();
    void setLastAutoTrigger(unsigned long time);
    unsigned long getLastAutoTrigger();

    // Rastreamento separado para cooldown de agendamentos (não afeta o automático por sensor)
    void setLastScheduleTrigger(unsigned long time);
    unsigned long getLastScheduleTrigger();
    void setActiveUntil(unsigned long time);
    unsigned long getActiveUntil();
    unsigned long getPumpStartedAtMs();
    int getRemainingSeconds(unsigned long now);
    int getScheduleCount();
    void clearSchedules();
    // Retorna o ID do agendamento no slot index (0-based). Retorna nullptr se slot vazio.
    const char* getScheduleId(int index) const;
    bool upsertSchedule(const char* id, bool ativo, uint8_t diasMask, int hour, int minute, int durationSeconds, const char* createdAtIso = nullptr);
    bool removeSchedule(const char* id);
    /** Se já existe agendamento com esse id, devolve `ativo` na NVS (payload MQTT sem campo `ativo`). */
    bool getStoredScheduleAtivo(const char* id, bool* outAtivo) const;
    bool checkDueSchedule(time_t nowTs, int* outDurationSeconds, const char** outScheduleId);
    
    // Retorna o limite maximo de agendamentos persistidos.
    // Usado pelo mqtt_manager para gerar o payload de erro sem literal hardcoded.
    int getMaxSchedules() const { return MAX_SCHEDULES; }

    // Consolida escritas NVS pendentes de duracao, threshold e cooldown em uma
    // unica operacao de flush. Deve ser chamado pelo caller apos aplicar um ou
    // mais setters de configuracao — evita N escritas separadas por comando setConfig.
    // setModoAuto() nao participa do batch: persiste imediatamente por ser mudanca
    // critica de modo que deve sobreviver a reboot de forma independente.
    void flushConfig();

    // Rastreamento de tipo de acionamento (para eventos)
    void setCurrentTrigger(IrrigationTriggerType trigger);
    IrrigationTriggerType getCurrentTrigger();

 private:
     bool bombaLigada = false;
     bool modoAuto = false;
     int duracaoSegundos = DURACAO_IRRIGACAO_PADRAO;
     int thresholdUmidade = THRESHOLD_UMIDADE_PADRAO;
     int cooldownSegundos = COOLDOWN_IRRIGACAO_PADRAO;
     unsigned long lastAutoTrigger = 0;
     unsigned long lastScheduleTrigger = 0;
     unsigned long activeUntil = 0;
    unsigned long pumpStartedAtMs = 0;
     IrrigationTriggerType currentTrigger = TRIGGER_UNKNOWN;
     // Flag de escrita NVS pendente para duracao/threshold/cooldown.
     // Setado pelos setters individuais; limpo por flushConfig().
     bool _nvsDirty = false;
    static const int MAX_SCHEDULES = 4;
    IrrigationSchedule schedules[MAX_SCHEDULES];
    void loadSchedulesFromStorage();
    void saveSchedulesToStorage();
};