package com.nr.irrigahome.domain.model

import com.google.firebase.Timestamp

data class DeviceProfile(
    val deviceId: String,
    val ownerUid: String? = null,
    val deviceName: String? = null,
    val firmwareVersion: String? = null,
    val macAddress: String? = null,
    val status: String? = null,
    val pairedAt: Timestamp? = null,
    val historyCollections: List<String> = emptyList()
) {
    val displayName: String
        get() = deviceName?.takeIf { it.isNotBlank() }
            ?: macAddress?.takeIf { it.isNotBlank() }
            ?: deviceId
}
