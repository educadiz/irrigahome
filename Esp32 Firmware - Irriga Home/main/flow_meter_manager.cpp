#include "flow_meter_manager.h"
#include "config.h"
#include "firmware_logger.h"
#include <Preferences.h>
#include <Arduino.h>
#include <math.h>

FlowMeterManager* FlowMeterManager::_instance = nullptr;

static const float LEGACY_FLOW_VOLUME_SCALE = 1.1489f;

// Mutex para proteger _pulseCount durante acesso simultâneo
static portMUX_TYPE flowMeterMux = portMUX_INITIALIZER_UNLOCKED;

FlowMeterManager::FlowMeterManager()
    : _pulseCount(0), _startMs(0), _stopMs(0), _active(false), _pulsesPerLiter(FLOW_PULSES_PER_LITER), _lastPulseUs(0), _minPulseIntervalUs(0), _volumeScale(FLOW_VOLUME_SCALE), _volumeOffsetMl(FLOW_VOLUME_OFFSET_ML), _calib_a(0.0f), _calib_b(0.0f), _useLinearCalibration(false), _calib_count(0), _calibrationVolumeCorrectionFactor(0.7f) {}

void FlowMeterManager::setPulsesPerLiter(float p) {
    portENTER_CRITICAL(&flowMeterMux);
    _pulsesPerLiter = p;
    portEXIT_CRITICAL(&flowMeterMux);
    fwLogf("INFO", "FLOW", "Pulsos por litro atualizado: %.0f", p);
}

float FlowMeterManager::getPulsesPerLiter() {
    portENTER_CRITICAL(&flowMeterMux);
    float v = _pulsesPerLiter;
    portEXIT_CRITICAL(&flowMeterMux);
    return v;
}

void FlowMeterManager::begin() {
    _instance = this;
    pinMode(FLOW_SENSOR_PIN, INPUT);  // INPUT simples, sem pull-up interno - deixar sensor fazer o pull-up
    Serial.print("[FLOW] Sensor inicializado no GPIO ");
    Serial.println(FLOW_SENSOR_PIN);
    // Carregar calibração persistida (se houver)
    loadCalibration();
}

void FlowMeterManager::startMeasurement() {
    portENTER_CRITICAL(&flowMeterMux);
    _pulseCount = 0;
    _startMs = millis();
    _stopMs = 0;
    _active = true;
    _instance = this;
    portEXIT_CRITICAL(&flowMeterMux);
    
    int rawLevel = digitalRead(FLOW_SENSOR_PIN);
    fwLogf("INFO", "FLOW", "Nivel bruto no GPIO %d: %s", FLOW_SENSOR_PIN, rawLevel == HIGH ? "HIGH" : "LOW");

    // Attach ISR fora do crítico
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), FlowMeterManager::_isr, FALLING);
    fwLogf("INFO", "FLOW", "Medicao iniciada em GPIO %d; aguardando pulsos na borda de descida", FLOW_SENSOR_PIN);
}

void FlowMeterManager::stopMeasurement() {
    detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
    
    portENTER_CRITICAL(&flowMeterMux);
    _stopMs = millis();
    _active = false;
    portEXIT_CRITICAL(&flowMeterMux);
    
    // Debug detalhado de vazão
    uint32_t pulses = getTotalPulses();
    float rawLiters = (_pulsesPerLiter > 0.0f) ? (((float)pulses) / _pulsesPerLiter) : 0.0f;
    float rawMl = rawLiters * 1000.0f;
    float volume = getTotalVolumeLiters();
    float correctedMl = volume * 1000.0f;
    float flowRate = getAvgFlowRateLpm();
    uint32_t durationMs = (_stopMs > _startMs) ? (_stopMs - _startMs) : 0;
    
    fwLogSection("FLOW", "Resultado final da medicao");
    fwLogf("INFO", "FLOW", "Pulsos=%u | volume=%.3fL | bruto=%.1fmL | taxa=%.3fL/min | duracao=%lums",
           _pulseCount, volume, rawMl, flowRate, (unsigned long)durationMs);
}

uint32_t FlowMeterManager::getTotalPulses() {
    portENTER_CRITICAL(&flowMeterMux);
    uint32_t p = _pulseCount;
    portEXIT_CRITICAL(&flowMeterMux);
    return p;
}

float FlowMeterManager::getTotalVolumeLiters() {
    uint32_t pulses = getTotalPulses();
    if (_useLinearCalibration) {
        // volume em mL = a * pulses + b
        float correctedMl = (_calib_a * (float)pulses) + _calib_b;
        if (correctedMl < 0.0f) correctedMl = 0.0f;
        return correctedMl / 1000.0f;
    }

    if (_pulsesPerLiter <= 0.0f) return 0.0f;

    float rawLiters = ((float)pulses) / _pulsesPerLiter;
    float rawMl = rawLiters * 1000.0f;
    float correctedMl = (rawMl * _volumeScale) + _volumeOffsetMl;
    if (correctedMl < 0.0f) correctedMl = 0.0f;
    return correctedMl / 1000.0f;
}

void FlowMeterManager::setVolumeCalibration(float scale, float offsetMl) {
    portENTER_CRITICAL(&flowMeterMux);
    _volumeScale = scale;
    _volumeOffsetMl = offsetMl;
    portEXIT_CRITICAL(&flowMeterMux);
    fwLogf("INFO", "FLOW", "Calibracao atualizada | scale=%.6f | offset_ml=%.3f", scale, offsetMl);
    saveCalibration();
}

float FlowMeterManager::getVolumeCalibrationScale() {
    portENTER_CRITICAL(&flowMeterMux);
    float v = _volumeScale;
    portEXIT_CRITICAL(&flowMeterMux);
    return v;
}

float FlowMeterManager::getVolumeCalibrationOffsetMl() {
    portENTER_CRITICAL(&flowMeterMux);
    float v = _volumeOffsetMl;
    portEXIT_CRITICAL(&flowMeterMux);
    return v;
}

void FlowMeterManager::loadCalibration() {
    Preferences prefs;
    // namespace 'flowcal'
    if (prefs.begin("flowcal", true)) {
        float s = prefs.getFloat("scale", _volumeScale);
        float o = prefs.getFloat("offset", _volumeOffsetMl);
        // tentar carregar calibração linear (a,b)
        float a = prefs.getFloat("a", NAN);
        float b = prefs.getFloat("b", NAN);
        prefs.end();

        bool legacyScaleLoaded = (s > LEGACY_FLOW_VOLUME_SCALE)
            ? ((s - LEGACY_FLOW_VOLUME_SCALE) < 0.0001f)
            : ((LEGACY_FLOW_VOLUME_SCALE - s) < 0.0001f);
        if (legacyScaleLoaded) {
            s = FLOW_VOLUME_SCALE;
            o = FLOW_VOLUME_OFFSET_ML;
            Serial.println("[FLOW] Calibracao legada detectada, migrando para a nova referencia 13000 pulsos = 50 mL");
        }

        portENTER_CRITICAL(&flowMeterMux);
        _volumeScale = s;
        _volumeOffsetMl = o;
        // se a é válida, usar calibração linear
        if (!isnan(a)) {
            _calib_a = a;
            _calib_b = isnan(b) ? 0.0f : b;
            _useLinearCalibration = true;
        } else {
            _useLinearCalibration = false;
        }
        portEXIT_CRITICAL(&flowMeterMux);

        Serial.print("[FLOW] Calibracao carregada - scale: ");
        Serial.print(_volumeScale, 6);
        Serial.print(" offset_ml: ");
        Serial.println(_volumeOffsetMl, 3);
        if (_useLinearCalibration) {
            Serial.print("[FLOW] Calibracao linear carregada - a: ");
            Serial.print(_calib_a, 9);
            Serial.print(" b: ");
            Serial.println(_calib_b, 3);
        }

        if (legacyScaleLoaded) {
            saveCalibration();
        }
    } else {
        Serial.println("[FLOW] Nenhuma calibracao persistida encontrada (usando padrao em config.h)");
    }
}

void FlowMeterManager::saveCalibration() {
    Preferences prefs;
    if (prefs.begin("flowcal", false)) {
        prefs.putFloat("scale", _volumeScale);
        prefs.putFloat("offset", _volumeOffsetMl);
        if (_useLinearCalibration) {
            prefs.putFloat("a", _calib_a);
            prefs.putFloat("b", _calib_b);
        }
        prefs.end();
        Serial.println("[FLOW] Calibracao salva na NVS");
    } else {
        Serial.println("[FLOW] Falha ao abrir NVS para salvar calibracao");
    }
}

void FlowMeterManager::addCalibrationSample(uint32_t pulses, float realVolumeMl) {
    float correctedVolume = realVolumeMl * _calibrationVolumeCorrectionFactor;
    portENTER_CRITICAL(&flowMeterMux);
    if (_calib_count < MAX_CAL_SAMPLES) {
        _calib_pulses[_calib_count] = pulses;
        _calib_realml[_calib_count] = correctedVolume;
        _calib_count++;
        Serial.print("[FLOW-CAL] Amostra adicionada: pulsos=");
        Serial.print(pulses);
        Serial.print(" volume_original=");
        Serial.print(realVolumeMl, 2);
        Serial.print(" mL -> volume_corrigido=");
        Serial.print(correctedVolume, 2);
        Serial.print(" mL (fator=");
        Serial.print(_calibrationVolumeCorrectionFactor, 2);
        Serial.println(")");
    }
    portEXIT_CRITICAL(&flowMeterMux);
}

bool FlowMeterManager::computeLinearCalibration() {
    portENTER_CRITICAL(&flowMeterMux);
    int n = _calib_count;
    if (n < 2) {
        portEXIT_CRITICAL(&flowMeterMux);
        return false;
    }

    double sumX = 0.0;
    double sumY = 0.0;
    for (int i = 0; i < n; ++i) {
        sumX += (double)_calib_pulses[i];
        sumY += (double)_calib_realml[i];
    }
    double meanX = sumX / (double)n;
    double meanY = sumY / (double)n;

    double num = 0.0;
    double den = 0.0;
    for (int i = 0; i < n; ++i) {
        double dx = (double)_calib_pulses[i] - meanX;
        double dy = (double)_calib_realml[i] - meanY;
        num += dx * dy;
        den += dx * dx;
    }

    if (den == 0.0) {
        portEXIT_CRITICAL(&flowMeterMux);
        return false;
    }

    double a = num / den;
    double b = meanY - a * meanX;

    _calib_a = (float)a;
    _calib_b = (float)b;
    _useLinearCalibration = true;
    portEXIT_CRITICAL(&flowMeterMux);

    saveCalibration();
    return true;
}

void FlowMeterManager::clearCalibrationSamples() {
    portENTER_CRITICAL(&flowMeterMux);
    _calib_count = 0;
    portEXIT_CRITICAL(&flowMeterMux);
}

void FlowMeterManager::getLinearCalibrationCoefficients(float &a, float &b) {
    portENTER_CRITICAL(&flowMeterMux);
    a = _calib_a;
    b = _calib_b;
    portEXIT_CRITICAL(&flowMeterMux);
}

void FlowMeterManager::addCalibrationSampleFromCurrentMeasurement(float realVolumeMl) {
    uint32_t pulses = getTotalPulses();
    addCalibrationSample(pulses, realVolumeMl);
}

void FlowMeterManager::setCalibrationVolumeCorrectionFactor(float factor) {
    portENTER_CRITICAL(&flowMeterMux);
    _calibrationVolumeCorrectionFactor = factor;
    portEXIT_CRITICAL(&flowMeterMux);
    Serial.print("[FLOW-CAL] Fator de correcao definido: ");
    Serial.println(factor, 4);
}

float FlowMeterManager::getCalibrationVolumeCorrectionFactor() {
    portENTER_CRITICAL(&flowMeterMux);
    float f = _calibrationVolumeCorrectionFactor;
    portEXIT_CRITICAL(&flowMeterMux);
    return f;
}

float FlowMeterManager::getAvgFlowRateLpm() {
    uint32_t endMs = _stopMs ? _stopMs : millis();
    uint32_t durationMs = (endMs > _startMs) ? (endMs - _startMs) : 0;
    if (durationMs == 0) return 0.0f;
    float liters = getTotalVolumeLiters();
    float minutes = ((float)durationMs) / 60000.0f;
    if (minutes <= 0.0f) return 0.0f;
    return liters / minutes;
}

bool FlowMeterManager::hasFlowDetected() {
    return getTotalPulses() > 0;
}

void IRAM_ATTR FlowMeterManager::_isr() {
    if (_instance) _instance->handlePulse();
}

void IRAM_ATTR FlowMeterManager::handlePulse() {
    // ISR crítica: apenas incrementar contador
    // A mutex global flowMeterMux protege acesso simultâneo
    // NUNCA fazer Serial.print() dentro de ISR - causa watchdog timeout!
    extern portMUX_TYPE flowMeterMux;
    portENTER_CRITICAL_ISR(&flowMeterMux);
    _pulseCount = _pulseCount + 1;
    portEXIT_CRITICAL_ISR(&flowMeterMux);
}

