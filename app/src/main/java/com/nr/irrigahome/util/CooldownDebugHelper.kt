package com.nr.irrigahome.util

import android.content.Context
import android.util.Log

object CooldownDebugHelper {
    private const val TAG = "CooldownDebug"
    private const val PREFS_NAME = "irrigahome_settings"
    private const val KEY_COOLDOWN_EXPIRY_TIMESTAMP = "cooldown_expiry_timestamp"

    fun debugCooldownStatus(context: Context) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val expiryTimestamp = prefs.getLong(KEY_COOLDOWN_EXPIRY_TIMESTAMP, 0L)
        val currentTimeMillis = System.currentTimeMillis()

        Log.d(TAG, "=== COOLDOWN DEBUG STATUS ===")
        Log.d(TAG, "Timestamp armazenado: $expiryTimestamp")
        Log.d(TAG, "Hora atual (ms): $currentTimeMillis")

        if (expiryTimestamp == 0L) {
            Log.d(TAG, "Status: Nenhum cooldown salvo")
        } else {
            val difference = expiryTimestamp - currentTimeMillis
            if (difference <= 0) {
                Log.d(TAG, "Status: Cooldown já expirou há ${-difference}ms")
            } else {
                val remainingSeconds = (difference + 999) / 1000
                Log.d(TAG, "Status: Cooldown ativo - $remainingSeconds segundos restantes")
            }
        }
        Log.d(TAG, "=============================")
    }

    fun clearCooldown(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().remove(KEY_COOLDOWN_EXPIRY_TIMESTAMP).apply()
        Log.d(TAG, "Cooldown limpo manualmente")
    }

    fun simulateCooldown(context: Context, durationSeconds: Int) {
        val expiryTimestamp = System.currentTimeMillis() + (durationSeconds * 1000L)
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putLong(KEY_COOLDOWN_EXPIRY_TIMESTAMP, expiryTimestamp).apply()
        Log.d(TAG, "Cooldown simulado: $durationSeconds segundos")
    }

    fun debugAllPreferences(context: Context) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        Log.d(TAG, "=== TODAS AS PREFERÊNCIAS ===")
        for ((key, value) in prefs.all) {
            Log.d(TAG, "$key = $value")
        }
        Log.d(TAG, "=============================")
    }
}
