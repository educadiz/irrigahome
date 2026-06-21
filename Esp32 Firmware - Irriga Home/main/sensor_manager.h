// Responsabilidade: declarar tipos e interface de leitura de sensores.
// O que faz: define SensorData e os metodos para inicializar, coletar leituras
// e aplicar offsets de calibracao persistidos na NVS.

#pragma once
#include <DHT.h>

struct SensorData {
    float temperatura;
    float umidadeAr;
    int   umidadeSolo;
    bool  nivelAgua;
};

class SensorManager {
public:
    SensorManager();
    void begin();

    // Retorna leitura com offsets de calibracao ja aplicados.
    // Toda a logica de controle (threshold, bloqueio por solo umido) usa
    // este metodo e portanto opera sobre o valor calibrado.
    SensorData read();

    // ── Offsets de calibracao ──────────────────────────────────────────────
    // Aplicados ANTES de devolver o dado ao chamador — afetam exibicao E controle.
    // Persistidos em NVS namespace "sensoroffset".

    void  setOffsetTemperatura(float offset);
    float getOffsetTemperatura() const;

    // Solo: offset em pontos percentuais com precisao de 0.1 (ex.: +2.5 eleva leitura de 40.0% para 42.5%)
    void  setOffsetUmidadeSolo(float offset);
    float getOffsetUmidadeSolo() const;

    void  setOffsetUmidadeAr(float offset);
    float getOffsetUmidadeAr() const;

    // Persiste todos os offsets de uma vez na NVS (batch).
    // Deve ser chamado pelo caller (web_server_manager) apos aplicar
    // um ou mais setters — mesmo padrao do ActuatorManager::flushConfig().
    void flushOffsets();

private:
    DHT        dht;
    SensorData lastValidData;
    bool       hasLastValidData   = false;
    unsigned long lastReadAt         = 0;
    unsigned long lastDhtValidAt     = 0;
    unsigned long lastDhtFailureLogAt = 0;
    bool       dhtFaultActive     = false;

    // Offsets em RAM (carregados da NVS em begin())
    float _offsetTemperatura = 0.0f;
    float _offsetUmidadeSolo = 0.0f;
    float _offsetUmidadeAr   = 0.0f;

    // Flag de escrita NVS pendente — limpo por flushOffsets()
    bool  _nvsDirty = false;

    void loadOffsets();
};
