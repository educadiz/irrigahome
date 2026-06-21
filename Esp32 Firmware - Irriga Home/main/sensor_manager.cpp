// Responsabilidade: implementar a aquisicao de dados dos sensores fisicos.
// O que faz: inicializa DHT e sensor de nivel, le temperatura/umidade do ar,
// converte a leitura analogica do solo para porcentagem, aplica offsets de
// calibracao e retorna SensorData. Persiste offsets na NVS.

#include "sensor_manager.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

static const unsigned long SENSOR_READ_CACHE_MS       = 500UL;
static const unsigned long DHT_STALE_TIMEOUT_MS       = 300000UL;
static const unsigned long DHT_FAILURE_LOG_INTERVAL_MS = 30000UL;

// Namespace NVS exclusivo para offsets de sensor (nao colide com "irrigahome")
static const char* NVS_NS_SENSOR   = "sensoroffset";
static const char* KEY_OFF_TEMP    = "off_temp";
static const char* KEY_OFF_SOLO    = "off_solo";
static const char* KEY_OFF_UMID_AR = "off_umid_ar";

SensorManager::SensorManager() : dht(DHTPIN, DHTTYPE) {
    lastValidData.temperatura = 0;
    lastValidData.umidadeAr   = 0;
    lastValidData.umidadeSolo = 100;
    lastValidData.nivelAgua   = false;
}

// Initialize sensor peripherals (DHT and level pin). Call once from setup().
void SensorManager::begin() {
    dht.begin();
    pinMode(WATER_LEVEL_PIN, INPUT);
    loadOffsets();
}

// Carrega offsets persistidos da NVS. Chamado uma vez em begin().
void SensorManager::loadOffsets() {
    Preferences prefs;
    if (prefs.begin(NVS_NS_SENSOR, true)) {   // read-only
        _offsetTemperatura = prefs.getFloat(KEY_OFF_TEMP,    0.0f);
        _offsetUmidadeSolo = prefs.getFloat(KEY_OFF_SOLO,    0.0f);
        _offsetUmidadeAr   = prefs.getFloat(KEY_OFF_UMID_AR, 0.0f);
        prefs.end();
        Serial.print("[SENSOR] offsets carregados — temp:");
        Serial.print(_offsetTemperatura, 1);
        Serial.print(" solo:");
        Serial.print(_offsetUmidadeSolo, 1);
        Serial.print(" ar:");
        Serial.println(_offsetUmidadeAr, 1);
    } else {
        Serial.println("[SENSOR] nenhum offset persistido (usando 0)");
    }
}

// Persiste todos os offsets em uma unica passagem pela NVS.
// Nao-op se nenhum setter foi chamado desde o ultimo flush (_nvsDirty == false).
void SensorManager::flushOffsets() {
    if (!_nvsDirty) return;
    Preferences prefs;
    if (prefs.begin(NVS_NS_SENSOR, false)) {
        prefs.putFloat(KEY_OFF_TEMP,    _offsetTemperatura);
        prefs.putFloat(KEY_OFF_SOLO,    _offsetUmidadeSolo);
        prefs.putFloat(KEY_OFF_UMID_AR, _offsetUmidadeAr);
        prefs.end();
        _nvsDirty = false;
        Serial.println("[SENSOR] offsets persistidos na NVS");
    } else {
        Serial.println("[SENSOR] falha ao abrir NVS para salvar offsets");
    }
}

// ── Setters ──────────────────────────────────────────────────────────────────

void SensorManager::setOffsetTemperatura(float offset) {
    // Limite razoavel: ±10 °C
    if (offset < -10.0f) offset = -10.0f;
    if (offset >  10.0f) offset =  10.0f;
    _offsetTemperatura = offset;
    _nvsDirty = true;
}

float SensorManager::getOffsetTemperatura() const {
    return _offsetTemperatura;
}

void SensorManager::setOffsetUmidadeSolo(float offset) {
    // Limite: ±30 pontos percentuais, precisao de 0.1
    if (offset < -30.0f) offset = -30.0f;
    if (offset >  30.0f) offset =  30.0f;
    // Arredonda para 1 casa decimal
    _offsetUmidadeSolo = roundf(offset * 10.0f) / 10.0f;
    _nvsDirty = true;
}

float SensorManager::getOffsetUmidadeSolo() const {
    return _offsetUmidadeSolo;
}

void SensorManager::setOffsetUmidadeAr(float offset) {
    // Limite: ±20 pontos percentuais
    if (offset < -20.0f) offset = -20.0f;
    if (offset >  20.0f) offset =  20.0f;
    _offsetUmidadeAr = offset;
    _nvsDirty = true;
}

float SensorManager::getOffsetUmidadeAr() const {
    return _offsetUmidadeAr;
}

// Read sensor values with short caching to reduce I2C/ADC load.
// Offsets de calibracao sao aplicados ANTES de retornar — afetam tanto
// a exibicao quanto a logica de controle (threshold, bloqueio por solo umido).
SensorData SensorManager::read() {
    unsigned long now = millis();
    if (hasLastValidData && (now - lastReadAt) < SENSOR_READ_CACHE_MS) {
        return lastValidData;
    }

    SensorData data = hasLastValidData ? lastValidData : SensorData{0, 0, 100, false};

    float temperatura = dht.readTemperature();
    float umidadeAr   = dht.readHumidity();
    bool  dhtValido   = !isnan(temperatura) && !isnan(umidadeAr);

    if (dhtValido) {
        // Aplica offset e satura dentro de limites fisicos plausíveis
        data.temperatura = temperatura + _offsetTemperatura;
        data.umidadeAr   = constrain(umidadeAr + _offsetUmidadeAr, 0.0f, 100.0f);
        lastDhtValidAt   = now;
        dhtFaultActive   = false;
    } else if (lastDhtValidAt > 0 && (now - lastDhtValidAt) >= DHT_STALE_TIMEOUT_MS) {
        data.temperatura = NAN;
        data.umidadeAr   = NAN;
        dhtFaultActive   = true;
        if ((now - lastDhtFailureLogAt) >= DHT_FAILURE_LOG_INTERVAL_MS) {
            Serial.println("[SENSOR] falha persistente do DHT: temperatura/umidade indisponiveis");
            lastDhtFailureLogAt = now;
        }
    }

    // Media de SOIL_ADC_SAMPLES amostras + offset de calibracao
    long soilAccum = 0;
    for (int _s = 0; _s < SOIL_ADC_SAMPLES; _s++) {
        soilAccum += analogRead(SOIL_PIN);
    }
    int leitura = (int)(soilAccum / SOIL_ADC_SAMPLES);
    int soloRaw = map(leitura, SOLO_SECO, SOLO_UMIDO, 0, 100);
    data.umidadeSolo = constrain((int)roundf((float)soloRaw + _offsetUmidadeSolo), 0, 100);

    data.nivelAgua = digitalRead(WATER_LEVEL_PIN);

    if (dhtValido) {
        lastValidData    = data;
        hasLastValidData = true;
    } else if (dhtFaultActive) {
        lastValidData.umidadeSolo = data.umidadeSolo;
        lastValidData.nivelAgua   = data.nivelAgua;
    }

    lastReadAt = now;
    return data;
}
