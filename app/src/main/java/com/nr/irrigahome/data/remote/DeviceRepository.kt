package com.nr.irrigahome.data.remote

import android.util.Log
import com.google.firebase.Timestamp
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.DocumentSnapshot
import com.google.firebase.firestore.FirebaseFirestore
import com.nr.irrigahome.domain.model.DeviceProfile
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class DeviceRepository @Inject constructor(
    private val firestore: FirebaseFirestore,
    private val auth: FirebaseAuth
) : com.nr.irrigahome.domain.repository.DeviceRepository {

    companion object {
        private const val TAG = "DeviceRepository"
    }

    private val cache = mutableMapOf<String, DeviceProfile?>()

    private fun normalizeDeviceId(deviceId: String?): String {
        return deviceId
            ?.trim()
            ?.lowercase()
            ?.replace(":", "")
            ?.replace("-", "")
            .orEmpty()
    }

    override fun clearCache() {
        cache.clear()
    }

    override fun fetchDeviceProfile(
        deviceId: String,
        onSuccess: (DeviceProfile?) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val normalizedDeviceId = normalizeDeviceId(deviceId)

        if (normalizedDeviceId.isBlank()) {
            onSuccess(null)
            return
        }

        cache[normalizedDeviceId]?.let { cached ->
            onSuccess(cached)
            return
        }

        val currentUid = auth.currentUser?.uid

        if (currentUid != null) {
            firestore.collection("users")
                .document(currentUid)
                .collection("devices")
                .document(normalizedDeviceId)
                .get()
                .addOnSuccessListener { snapshot ->
                    val macAddress = snapshot.getString("macAddress")?.takeIf { it.isNotBlank() }
                    if (snapshot.exists() && macAddress != null) {
                        val profile = snapshot.toDeviceProfile(
                            deviceId = normalizedDeviceId,
                            ownerUidFallback = currentUid
                        )
                        cache[normalizedDeviceId] = profile
                        onSuccess(profile)
                    } else {
                        cache[normalizedDeviceId] = null
                        onSuccess(null)
                    }
                }
                .addOnFailureListener { error ->
                    Log.w(TAG, "Falha ao ler users/{uid}/devices/$normalizedDeviceId: ${error.message}")
                    cache[normalizedDeviceId] = null
                    onSuccess(null)
                }
            return
        }

        fetchGlobalDeviceProfile(normalizedDeviceId, onSuccess, onError)
    }

    private fun fetchGlobalDeviceProfile(
        deviceId: String,
        onSuccess: (DeviceProfile?) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        firestore.collection("devices")
            .document(deviceId)
            .get()
            .addOnSuccessListener { snapshot ->
                val profile = if (snapshot.exists()) {
                    snapshot.toDeviceProfile(deviceId = deviceId, ownerUidFallback = null)
                } else {
                    null
                }
                cache[deviceId] = profile
                onSuccess(profile)
            }
            .addOnFailureListener(onError)
    }

    override fun fetchCurrentUserDeviceIds(
        onSuccess: (Set<String>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            onSuccess(emptySet())
            return
        }

        firestore.collection("users")
            .document(currentUid)
            .collection("devices")
            .get()
            .addOnSuccessListener { snapshot ->
                val deviceIds = snapshot.documents
                    .mapNotNull { doc ->
                        val macAddress = doc.getString("macAddress")?.takeIf { it.isNotBlank() }
                        if (macAddress != null) normalizeDeviceId(macAddress) else null
                    }
                    .toSet()

                if (deviceIds.isNotEmpty()) {
                    onSuccess(deviceIds)
                    return@addOnSuccessListener
                }

                onSuccess(emptySet())
            }
            .addOnFailureListener(onError)
    }

    override fun fetchCurrentUserHistoryCollections(
        onSuccess: (Set<String>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        onSuccess(setOf("events"))
    }

    override fun initializeNewUserStructure(
        uid: String,
        name: String,
        email: String,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val now = Timestamp.now()

        val batch = firestore.batch()
        val userDoc = firestore.collection("users").document(uid)
        val placeholderDoc = userDoc.collection("devices").document("_placeholder")

        batch.set(
            userDoc,
            hashMapOf(
                "name" to name,
                "email" to email,
                "createdAt" to now,
                "deviceBindingMode" to "macAddress",
                "devicesInitializedAt" to now
            )
        )

        batch.set(
            placeholderDoc,
            hashMapOf(
                "createdAt" to now,
                "placeholder" to true,
                "bindingState" to "pending_mac"
            )
        )

        batch.commit()
            .addOnSuccessListener { onSuccess() }
            .addOnFailureListener { e -> onError(e) }
    }

    private fun DocumentSnapshot.toDeviceProfile(
        deviceId: String,
        ownerUidFallback: String?
    ): DeviceProfile {
        return DeviceProfile(
            deviceId = deviceId,
            ownerUid = getString("ownerUid")?.takeIf { it.isNotBlank() } ?: ownerUidFallback,
            deviceName = getString("deviceName")?.takeIf { it.isNotBlank() },
            firmwareVersion = getString("firmwareVersion")?.takeIf { it.isNotBlank() },
            macAddress = getString("macAddress")?.takeIf { it.isNotBlank() },
            status = getString("status")?.takeIf { it.isNotBlank() },
            pairedAt = getTimestamp("pairedAt"),
            historyCollections = listOf("events")
        )
    }
}
