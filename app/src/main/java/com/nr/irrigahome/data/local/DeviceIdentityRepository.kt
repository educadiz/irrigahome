package com.nr.irrigahome.data.local

import android.content.Context

class DeviceIdentityRepository(context: Context) {
    private val prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun saveLastKnownMacAddress(macAddress: String?) {
        val normalized = normalizeMacAddress(macAddress)
        prefs.edit()
            .apply {
                if (normalized == null) {
                    remove(KEY_LAST_MAC_ADDRESS)
                } else {
                    putString(KEY_LAST_MAC_ADDRESS, normalized)
                }
            }
            .apply()
    }

    fun loadLastKnownMacAddressFormatted(): String? {
        val normalized = prefs.getString(KEY_LAST_MAC_ADDRESS, null)?.takeIf { it.isNotBlank() }
            ?: return null
        return formatMacAddress(normalized)
    }

    private fun normalizeMacAddress(macAddress: String?): String? {
        return macAddress
            ?.trim()
            ?.replace(":", "")
            ?.replace("-", "")
            ?.uppercase()
            ?.takeIf { it.length == 12 && it.all { char -> char.isDigit() || char in 'A'..'F' } }
    }

    private fun formatMacAddress(normalizedMac: String): String? {
        if (normalizedMac.length != 12) return null
        return normalizedMac.chunked(2).joinToString(":")
    }

    private companion object {
        private const val PREFS_NAME = "device_identity"
        private const val KEY_LAST_MAC_ADDRESS = "last_mac_address"
    }
}
