package com.nr.irrigahome.domain.model

import com.nr.irrigahome.domain.model.IrrigationMode

data class IrrigationSettingsSnapshot(
    val irrigationMode: IrrigationMode,
    val manualWaterDurationSeconds: Int,
    val automaticWaterDurationSeconds: Int,
    val automaticSoilThreshold: Int,
    val automaticCooldownSeconds: Int,
    val maxManualWaterTempC: Int,
    val cooldownExpiryTimestamp: Long
)
