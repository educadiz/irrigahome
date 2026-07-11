package com.nr.irrigahome.data.local

import android.content.Context
import android.content.SharedPreferences
import com.google.firebase.auth.FirebaseAuth
import com.nr.irrigahome.domain.model.IrrigationMode
import com.nr.irrigahome.domain.model.IrrigationSettingsSnapshot

import javax.inject.Inject
import javax.inject.Singleton
import dagger.hilt.android.qualifiers.ApplicationContext

@Singleton
class IrrigationSettingsRepository @Inject constructor(@ApplicationContext context: Context) :
    com.nr.irrigahome.domain.repository.IrrigationSettingsRepository {

    companion object {
        private const val PREFS_NAME = "irrigahome_settings"
        private const val KEY_IRRIGATION_MODE = "irrigation_mode"
        private const val KEY_MANUAL_WATER_DURATION_SECONDS = "manual_water_duration_seconds"
        private const val KEY_AUTOMATIC_WATER_DURATION_SECONDS = "automatic_water_duration_seconds"
        private const val KEY_AUTOMATIC_SOIL_THRESHOLD = "automatic_soil_threshold"
        private const val KEY_AUTOMATIC_COOLDOWN_SECONDS = "automatic_cooldown_seconds"
        private const val KEY_MAX_MANUAL_WATER_TEMP_C = "max_manual_water_temp_c"
        private const val KEY_COOLDOWN_EXPIRY_TIMESTAMP = "cooldown_expiry_timestamp"

        private const val DEFAULT_MANUAL_WATER_DURATION_SECONDS = 5
        private const val DEFAULT_AUTOMATIC_WATER_DURATION_SECONDS = 10
        private const val DEFAULT_AUTOMATIC_SOIL_THRESHOLD = 30
        private const val DEFAULT_AUTOMATIC_COOLDOWN_SECONDS = 300
        private const val DEFAULT_MAX_MANUAL_WATER_TEMP_C = 35
    }

    private val prefs: SharedPreferences = context.applicationContext
        .getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val auth: FirebaseAuth = FirebaseAuth.getInstance()

    private fun currentUserScope(): String {
        val uid = auth.currentUser?.uid?.trim().orEmpty()
        return if (uid.isNotBlank()) uid else "guest"
    }

    private fun scopedKey(baseKey: String): String = "${currentUserScope()}:$baseKey"

    private fun readInt(baseKey: String, defaultValue: Int): Int {
        val scoped = scopedKey(baseKey)
        return when {
            prefs.contains(scoped) -> prefs.getInt(scoped, defaultValue)
            prefs.contains(baseKey) -> prefs.getInt(baseKey, defaultValue) // compatibilidade com versões antigas
            else -> defaultValue
        }
    }

    private fun readLong(baseKey: String, defaultValue: Long): Long {
        val scoped = scopedKey(baseKey)
        return when {
            prefs.contains(scoped) -> prefs.getLong(scoped, defaultValue)
            prefs.contains(baseKey) -> prefs.getLong(baseKey, defaultValue) // compatibilidade com versões antigas
            else -> defaultValue
        }
    }

    private fun readString(baseKey: String, defaultValue: String): String {
        val scoped = scopedKey(baseKey)
        return when {
            prefs.contains(scoped) -> prefs.getString(scoped, defaultValue).orEmpty()
            prefs.contains(baseKey) -> prefs.getString(baseKey, defaultValue).orEmpty() // compatibilidade com versões antigas
            else -> defaultValue
        }
    }

    override fun loadSettings(): IrrigationSettingsSnapshot {
        val savedDuration = readInt(KEY_MANUAL_WATER_DURATION_SECONDS, DEFAULT_MANUAL_WATER_DURATION_SECONDS)
        val savedAutomaticDuration = readInt(KEY_AUTOMATIC_WATER_DURATION_SECONDS, DEFAULT_AUTOMATIC_WATER_DURATION_SECONDS)
        val savedAutomaticThreshold = readInt(KEY_AUTOMATIC_SOIL_THRESHOLD, DEFAULT_AUTOMATIC_SOIL_THRESHOLD)
        val savedAutomaticCooldown = readInt(KEY_AUTOMATIC_COOLDOWN_SECONDS, DEFAULT_AUTOMATIC_COOLDOWN_SECONDS)
        val savedMaxTemp = readInt(KEY_MAX_MANUAL_WATER_TEMP_C, DEFAULT_MAX_MANUAL_WATER_TEMP_C)
        val savedMode = readString(KEY_IRRIGATION_MODE, IrrigationMode.MANUAL.name)
        val cooldownExpiryTimestamp = readLong(KEY_COOLDOWN_EXPIRY_TIMESTAMP, 0L)

        return IrrigationSettingsSnapshot(
            irrigationMode = if (savedMode == IrrigationMode.AUTOMATIC.name) IrrigationMode.AUTOMATIC else IrrigationMode.MANUAL,
            manualWaterDurationSeconds = if (savedDuration in listOf(5, 10, 15, 20)) savedDuration else DEFAULT_MANUAL_WATER_DURATION_SECONDS,
            automaticWaterDurationSeconds = if (savedAutomaticDuration in listOf(5, 10, 15, 20)) savedAutomaticDuration else DEFAULT_AUTOMATIC_WATER_DURATION_SECONDS,
            automaticSoilThreshold = if (savedAutomaticThreshold in listOf(20, 25, 30, 35, 40)) savedAutomaticThreshold else DEFAULT_AUTOMATIC_SOIL_THRESHOLD,
            automaticCooldownSeconds = if (savedAutomaticCooldown in listOf(180, 300, 600, 900)) savedAutomaticCooldown else DEFAULT_AUTOMATIC_COOLDOWN_SECONDS,
            maxManualWaterTempC = if (savedMaxTemp in listOf(25, 30, 35, 40)) savedMaxTemp else DEFAULT_MAX_MANUAL_WATER_TEMP_C,
            cooldownExpiryTimestamp = cooldownExpiryTimestamp
        )
    }

    override fun saveIrrigationMode(mode: IrrigationMode) {
        prefs.edit().putString(scopedKey(KEY_IRRIGATION_MODE), mode.name).apply()
    }

    override fun saveManualWaterDurationSeconds(seconds: Int) {
        prefs.edit().putInt(scopedKey(KEY_MANUAL_WATER_DURATION_SECONDS), seconds).apply()
    }

    override fun saveAutomaticWaterDurationSeconds(seconds: Int) {
        prefs.edit().putInt(scopedKey(KEY_AUTOMATIC_WATER_DURATION_SECONDS), seconds).apply()
    }

    override fun saveAutomaticSoilThreshold(threshold: Int) {
        prefs.edit().putInt(scopedKey(KEY_AUTOMATIC_SOIL_THRESHOLD), threshold).apply()
    }

    override fun saveAutomaticCooldownSeconds(seconds: Int) {
        prefs.edit().putInt(scopedKey(KEY_AUTOMATIC_COOLDOWN_SECONDS), seconds).apply()
    }

    override fun saveMaxManualWaterTempC(tempC: Int) {
        prefs.edit().putInt(scopedKey(KEY_MAX_MANUAL_WATER_TEMP_C), tempC).apply()
    }

    override fun saveCooldownExpiryTimestamp(expiryTimestamp: Long) {
        prefs.edit().putLong(scopedKey(KEY_COOLDOWN_EXPIRY_TIMESTAMP), expiryTimestamp).apply()
    }

    override fun clearCooldownExpiryTimestamp() {
        prefs.edit().remove(scopedKey(KEY_COOLDOWN_EXPIRY_TIMESTAMP)).apply()
    }
}
