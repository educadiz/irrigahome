package com.nr.irrigahome.domain.repository

import com.nr.irrigahome.domain.model.DeviceProfile

interface DeviceRepository {

    fun fetchDeviceProfile(
        deviceId: String,
        onSuccess: (DeviceProfile?) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun fetchCurrentUserDeviceIds(
        onSuccess: (Set<String>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun fetchCurrentUserHistoryCollections(
        onSuccess: (Set<String>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun initializeNewUserStructure(
        uid: String,
        name: String,
        email: String,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    )

    fun clearCache()
}
