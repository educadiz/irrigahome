// Gerencia a leitura do sensor de vazao YF-S401 via interrupcao
#pragma once
#include <Arduino.h>
#include <stdint.h>

class FlowMeterManager {
public:
    FlowMeterManager();
    void begin();
    void startMeasurement();
    void stopMeasurement();
    uint32_t getTotalPulses();
    float getTotalVolumeLiters();
    float getAvgFlowRateLpm();
    bool hasFlowDetected();
    void setPulsesPerLiter(float p);
    float getPulsesPerLiter();
    // Calibração dinâmica (scale e offset em mL)
    void setVolumeCalibration(float scale, float offsetMl);
    float getVolumeCalibrationScale();
    float getVolumeCalibrationOffsetMl();
    // Calibração linear direta (volume mL = a * pulses + b)
    void addCalibrationSample(uint32_t pulses, float realVolumeMl);
    bool computeLinearCalibration();
    void clearCalibrationSamples();
    void getLinearCalibrationCoefficients(float &a, float &b);
    // Conveniência: usa o total de pulsos da última medição como amostra
    void addCalibrationSampleFromCurrentMeasurement(float realVolumeMl);
    // Fator de correção para amostras de volume (default 0.7 = -30%)
    void setCalibrationVolumeCorrectionFactor(float factor);
    float getCalibrationVolumeCorrectionFactor();
    // Forçar recarregar/salvar calibração da NVS
    void loadCalibration();
    void saveCalibration();

private:
    volatile uint32_t _pulseCount;
    uint32_t _startMs;
    uint32_t _stopMs;
    bool _active;
    float _pulsesPerLiter;

    // opcional: debouncing mínimo entre pulsos (us)
    volatile uint32_t _lastPulseUs;
    uint32_t _minPulseIntervalUs;

    // Calibração dinâmica aplicada ao volume (múltiplo + offset em mL)
    float _volumeScale;
    float _volumeOffsetMl;
    // Calibração linear direta (pulses -> mL)
    float _calib_a;
    float _calib_b;
    bool _useLinearCalibration;
    static const int MAX_CAL_SAMPLES = 64;
    uint32_t _calib_pulses[MAX_CAL_SAMPLES];
    float _calib_realml[MAX_CAL_SAMPLES];
    int _calib_count;
    // Fator de correção aplicado ao volume durante coleta de amostras (-30% = 0.7)
    float _calibrationVolumeCorrectionFactor;

    static FlowMeterManager* _instance;
    static void IRAM_ATTR _isr();
    void handlePulse();  // Chamada apenas de _isr, não requer IRAM_ATTR
};

// Instancia global (definida em main.ino)
extern FlowMeterManager flowMeter;
