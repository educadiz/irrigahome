package com.nr.irrigahome.data.remote

import android.util.Log
import com.google.android.gms.tasks.Tasks
import com.google.firebase.Timestamp
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.DocumentSnapshot
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.ListenerRegistration
import com.google.firebase.firestore.Query
import com.google.firebase.firestore.QuerySnapshot
import com.nr.irrigahome.domain.model.IrrigationHistoryEvent
import com.nr.irrigahome.domain.model.TriggerType
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

class IrrigationHistoryRepository @javax.inject.Inject constructor(
    private val firestore: FirebaseFirestore,
    private val auth: FirebaseAuth
) : com.nr.irrigahome.domain.repository.IrrigationHistoryRepository {

    companion object {
        private const val TAG = "IrrigationHistoryRepo"
        private const val HISTORY_COLLECTION_NAME = "events"
        private val dateFormats = listOf(
            "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'",
            "yyyy-MM-dd'T'HH:mm:ss'Z'",
            "yyyy-MM-dd HH:mm:ss",
            "dd/MM/yyyy HH:mm:ss"
        )
    }

    private val deviceRepository = DeviceRepository(firestore, auth)

    override fun shutdown() {
        // Mantido para compatibilidade com chamadas existentes no ViewModel (onCleared).
    }

    override fun loadIrrigationHistory(
        limit: Int,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            loadHistoryFromFirestore(limit, null, emptySet(), onSuccess, onError)
            return
        }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                loadHistoryFromFirestore(limit, currentUid, deviceIds, onSuccess, onError)
            },
            onError = onError
        )
    }

    override fun observeIrrigationHistory(
        limit: Int,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ): ListenerRegistration {
        val currentUid = auth.currentUser?.uid

        if (currentUid == null) {
            return attachHistoryListener(null, emptySet(), limit, onSuccess, onError)
        }

        var registration: ListenerRegistration? = null
        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                registration = attachHistoryListener(currentUid, deviceIds, limit, onSuccess, onError)
            },
            onError = onError
        )

        return object : ListenerRegistration {
            override fun remove() {
                registration?.remove()
            }
        }
    }

    private fun attachHistoryListener(
        currentUid: String?,
        allowedDeviceIds: Set<String>,
        limit: Int,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ): ListenerRegistration {
        val normalizedDeviceIds = allowedDeviceIds
            .map { it.trim() }
            .filter { it.isNotBlank() }
            .distinct()

        if (!currentUid.isNullOrBlank() && normalizedDeviceIds.isEmpty()) {
            onSuccess(emptyList())
            return object : ListenerRegistration {
                override fun remove() {}
            }
        }

        val registrations = mutableListOf<ListenerRegistration>()
        val latestSnapshots = mutableMapOf<String, List<DocumentSnapshot>>()

        fun emitMergedEvents() {
            try {
                val events = latestSnapshots.values
                    .flatten()
                    .distinctBy { it.reference.path }
                    .mapNotNull { doc ->
                        val data = (doc.data as? Map<String, Any> ?: emptyMap()).toMutableMap()
                        if (!data.containsKey("id") || (data["id"] as? String).isNullOrBlank()) {
                            data["id"] = doc.id
                        }
                        data.putIfAbsent("historyCollection", doc.reference.parent.id)
                        parseIrrigationHistoryFirestore(data)?.takeIf { event ->
                            belongsToCurrentUser(event, currentUid, allowedDeviceIds)
                        }
                    }
                    .sortedByDescending { it.startTime }
                    .take(limit)

                Log.d(TAG, "🔄 Histórico atualizado em tempo real: ${events.size} registros")
                onSuccess(events)
            } catch (e: Exception) {
                Log.e(TAG, "❌ Erro ao processar listener do histórico: ${e.message}")
                onError(e)
            }
        }

        if (normalizedDeviceIds.isNotEmpty()) {
            normalizedDeviceIds.forEach { deviceId ->
                val query = firestore.collection("irrigationHistory")
                    .document(deviceId)
                    .collection(HISTORY_COLLECTION_NAME)
                    .orderBy("startAt", Query.Direction.DESCENDING)
                    .limit(limit.toLong())

                val registration = query.addSnapshotListener { snapshot, error ->
                    if (error != null) {
                        Log.e(TAG, "❌ Erro no listener do histórico: ${error.message}", error)
                        onError(error)
                        return@addSnapshotListener
                    }

                    latestSnapshots[deviceId] = snapshot?.documents ?: emptyList()
                    emitMergedEvents()
                }

                registrations.add(registration)
            }
        }

        return object : ListenerRegistration {
            override fun remove() {
                registrations.forEach { it.remove() }
            }
        }
    }

    override fun loadHistoryByDateRange(
        startDate: Date,
        endDate: Date,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            loadHistoryFromFirestoreByDateRange(startDate, endDate, null, emptySet(), onSuccess, onError)
            return
        }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                loadHistoryFromFirestoreByDateRange(startDate, endDate, currentUid, deviceIds, onSuccess, onError)
            },
            onError = onError
        )
    }

    override fun loadHistoryByTriggerType(
        triggerType: TriggerType,
        limit: Int,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            loadHistoryFromFirestore(
                limit = limit,
                currentUid = null,
                allowedDeviceIds = emptySet(),
                onSuccess = { allEvents ->
                    onSuccess(allEvents.filter { it.triggerType == triggerType })
                },
                onError = onError
            )
            return
        }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                loadHistoryFromFirestore(
                    limit = limit,
                    currentUid = currentUid,
                    allowedDeviceIds = deviceIds,
                    onSuccess = { allEvents ->
                        onSuccess(allEvents.filter { it.triggerType == triggerType })
                    },
                    onError = onError
                )
            },
            onError = onError
        )
    }

    override fun removeHistoryEvent(
        event: IrrigationHistoryEvent,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val id = event.id.trim()

        if (id.isNotBlank()) {
            val deviceId = event.deviceId.trim()
            val historyCollection = event.historyCollection?.trim().takeIf { !it.isNullOrBlank() }
                ?: HISTORY_COLLECTION_NAME

            if (deviceId.isNotBlank()) {
                firestore.collection("irrigationHistory")
                    .document(deviceId)
                    .collection(historyCollection)
                    .document(id)
                    .delete()
                    .addOnSuccessListener { onSuccess() }
                    .addOnFailureListener(onError)
                return
            }

            onError(NoSuchElementException("Evento não encontrado no Firestore"))
        } else {
            onError(IllegalStateException("ID do evento inválido para remoção"))
        }
    }

    // ── Privado: Firestore ─────────────────────────────────────────────────────

    private fun loadHistoryFromFirestore(
        limit: Int,
        currentUid: String?,
        allowedDeviceIds: Set<String>,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val normalizedDeviceIds = allowedDeviceIds.map { it.trim() }.filter { it.isNotBlank() }.distinct()

        if (!currentUid.isNullOrBlank() && normalizedDeviceIds.isEmpty()) {
            onSuccess(emptyList()); return
        }

        val tasks = if (normalizedDeviceIds.isNotEmpty()) {
            normalizedDeviceIds.map { deviceId ->
                firestore.collection("irrigationHistory")
                    .document(deviceId)
                    .collection(HISTORY_COLLECTION_NAME)
                    .orderBy("startAt", Query.Direction.DESCENDING)
                    .limit(limit.toLong())
                    .get()
            }
        } else emptyList()

        if (tasks.isEmpty()) { onSuccess(emptyList()); return }

        Tasks.whenAllSuccess<QuerySnapshot>(tasks)
            .addOnSuccessListener { snapshots ->
                try {
                    val events = snapshots
                        .flatMap { it.documents }
                        .distinctBy { it.reference.path }
                        .mapNotNull { doc ->
                            val data = (doc.data as? Map<String, Any> ?: emptyMap()).toMutableMap()
                            if (!data.containsKey("id") || (data["id"] as? String).isNullOrBlank()) data["id"] = doc.id
                            data.putIfAbsent("historyCollection", doc.reference.parent.id)
                            parseIrrigationHistoryFirestore(data)?.takeIf { event ->
                                belongsToCurrentUser(event, currentUid, allowedDeviceIds)
                            }
                        }
                        .sortedByDescending { it.startTime }
                        .take(limit)

                    onSuccess(events)
                } catch (e: Exception) {
                    onError(e)
                }
            }
            .addOnFailureListener(onError)
    }

    private fun loadHistoryFromFirestoreByDateRange(
        startDate: Date,
        endDate: Date,
        currentUid: String?,
        allowedDeviceIds: Set<String>,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val startTimestamp = Timestamp(startDate)
        val endTimestamp = Timestamp(endDate)

        val normalizedDeviceIds = allowedDeviceIds.map { it.trim() }.filter { it.isNotBlank() }.distinct()

        if (!currentUid.isNullOrBlank() && normalizedDeviceIds.isEmpty()) {
            onSuccess(emptyList()); return
        }

        val tasks = if (normalizedDeviceIds.isNotEmpty()) {
            normalizedDeviceIds.map { deviceId ->
                firestore.collection("irrigationHistory")
                    .document(deviceId)
                    .collection(HISTORY_COLLECTION_NAME)
                    .whereGreaterThanOrEqualTo("startAt", startTimestamp)
                    .whereLessThanOrEqualTo("startAt", endTimestamp)
                    .orderBy("startAt", Query.Direction.DESCENDING)
                    .get()
            }
        } else emptyList()

        if (tasks.isEmpty()) { onSuccess(emptyList()); return }

        Tasks.whenAllSuccess<QuerySnapshot>(tasks)
            .addOnSuccessListener { snapshots ->
                try {
                    val events = snapshots
                        .flatMap { it.documents }
                        .distinctBy { it.reference.path }
                        .mapNotNull { doc ->
                            val data = (doc.data as? Map<String, Any> ?: emptyMap()).toMutableMap()
                            if (!data.containsKey("id") || (data["id"] as? String).isNullOrBlank()) data["id"] = doc.id
                            data.putIfAbsent("historyCollection", doc.reference.parent.id)
                            parseIrrigationHistoryFirestore(data)?.takeIf { event ->
                                belongsToCurrentUser(event, currentUid, allowedDeviceIds)
                            }
                        }
                        .sortedByDescending { it.startTime }

                    onSuccess(events)
                } catch (e: Exception) {
                    onError(e)
                }
            }
            .addOnFailureListener(onError)
    }

    private fun belongsToCurrentUser(
        event: IrrigationHistoryEvent,
        currentUid: String?,
        allowedDeviceIds: Set<String>
    ): Boolean {
        if (currentUid.isNullOrBlank()) return true
        if (!event.ownerUid.isNullOrBlank()) return event.ownerUid == currentUid
        return event.deviceId in allowedDeviceIds
    }

    private fun parseIrrigationHistoryFirestore(data: Map<String, Any>): IrrigationHistoryEvent? {
        return try {
            val id = data["id"] as? String ?: ""
            val deviceId = data["deviceId"] as? String ?: "esp32_01"
            val historyCollection = (data["historyCollection"] as? String)?.takeIf { it.isNotBlank() }
                ?: HISTORY_COLLECTION_NAME
            val ownerUid = data["ownerUid"] as? String
            val deviceName = data["deviceName"] as? String
            val firmwareVersion = data["firmwareVersion"] as? String
            val macAddress = data["macAddress"] as? String
            val eventId = data["eventId"] as? String ?: ""
            val startAt = parseFirestoreDate(data["startAt"])
            val endAt = parseFirestoreDate(data["endAt"])
            val durationSec = (data["durationSec"] as? Number)?.toLong() ?: 0L
            val trigger = data["trigger"] as? String ?: "manual"
            val temperature = (data["temperature"] as? Number)?.toInt()
            val soilHumidity = (data["soilHumidity"] as? Number)?.toInt()
            val airHumidity = (data["airHumidity"] as? Number)?.toInt()
            val waterLevel = data["waterLevel"] as? String
            val success = data["success"] as? Boolean
            val stopReason = data["stopReason"] as? String
            val totalVolumeMl = (data["totalVolumeMl"] as? Number)?.toLong()

            if (startAt == null || endAt == null) {
                Log.w(TAG, "⚠️ Timestamps inválidos no Firestore")
                return null
            }

            IrrigationHistoryEvent(
                id = id,
                eventId = eventId,
                deviceId = deviceId,
                historyCollection = historyCollection,
                ownerUid = ownerUid,
                deviceName = deviceName,
                firmwareVersion = firmwareVersion,
                macAddress = macAddress ?: deviceId,
                startTime = startAt,
                endTime = endAt,
                durationSeconds = durationSec,
                triggerType = TriggerType.fromString(trigger),
                temperature = temperature,
                soilHumidity = soilHumidity,
                airHumidity = airHumidity,
                waterLevel = waterLevel,
                success = success,
                stopReason = stopReason,
                totalVolumeMl = totalVolumeMl
            )
        } catch (e: Exception) {
            Log.e(TAG, "❌ Erro ao parsear Firestore: ${e.message}")
            null
        }
    }

    private fun parseFirestoreDate(value: Any?): Date? {
        return when (value) {
            is Timestamp -> value.toDate()
            is Date -> value
            is String -> parseDateTime(value)
            is Number -> {
                val timestamp = value.toLong()
                if (timestamp < 10_000_000_000L) Date(timestamp * 1000) else Date(timestamp)
            }
            else -> null
        }
    }

    private fun parseDateTime(dateStr: String): Date? {
        if (dateStr.isBlank()) return null

        for (format in dateFormats) {
            try {
                return SimpleDateFormat(format, Locale.US).apply {
                    isLenient = false
                    timeZone = TimeZone.getTimeZone("UTC")
                }.parse(dateStr)
            } catch (_: Exception) { }
        }

        return try {
            val timestamp = dateStr.toLong()
            if (timestamp < 10_000_000_000L) Date(timestamp * 1000) else Date(timestamp)
        } catch (_: Exception) {
            null
        }
    }
}
