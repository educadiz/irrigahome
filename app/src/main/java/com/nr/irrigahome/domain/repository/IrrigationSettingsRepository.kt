package com.nr.irrigahome.domain.repository

import com.nr.irrigahome.domain.model.IrrigationMode
import com.nr.irrigahome.domain.model.IrrigationSettingsSnapshot

interface IrrigationSettingsRepository {

    fun loadSettings(): IrrigationSettingsSnapshot

    fun saveIrrigationMode(mode: IrrigationMode)

    fun saveManualWaterDurationSeconds(seconds: Int)

    fun saveAutomaticWaterDurationSeconds(seconds: Int)

    fun saveAutomaticSoilThreshold(threshold: Int)

    fun saveAutomaticCooldownSeconds(seconds: Int)

    fun saveMaxManualWaterTempC(tempC: Int)

    fun saveCooldownExpiryTimestamp(expiryTimestamp: Long)

    fun clearCooldownExpiryTimestamp()
}
