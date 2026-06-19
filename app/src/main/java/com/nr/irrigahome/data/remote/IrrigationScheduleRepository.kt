package com.nr.irrigahome.data.remote

import android.util.Log
import com.google.android.gms.tasks.Tasks
import com.google.firebase.Timestamp
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FieldValue
import com.google.firebase.firestore.FirebaseFirestore
import com.google.firebase.firestore.QuerySnapshot
import com.nr.irrigahome.domain.model.IrrigationSchedule
import org.json.JSONArray
import java.net.HttpURLConnection
import java.net.URL

class IrrigationScheduleRepository @javax.inject.Inject constructor(
    private val firestore: FirebaseFirestore,
    private val auth: FirebaseAuth
) : com.nr.irrigahome.domain.repository.IrrigationScheduleRepository {

    companion object {
        private const val TAG = "IrrigationScheduleRepo"
        private const val GET_AGENDAMENTOS_URL = "https://getagendamentos-ytpw22yygq-uc.a.run.app"
        private const val MAX_SCHEDULES_PER_DEVICE = 4
        private val SLOT_COLLECTIONS = listOf("schedule_01", "schedule_02", "schedule_03", "schedule_04")
    }

    private val deviceRepository = DeviceRepository(firestore, auth)

    private fun normalizeDeviceId(deviceId: String?): String {
        return deviceId
            ?.trim()
            ?.lowercase()
            ?.replace(":", "")
            ?.replace("-", "")
            .orEmpty()
    }

    private fun deviceScheduleCollection(deviceId: String, slotName: String) =
        firestore.collection("schedules").document(deviceId).collection(slotName)

    override fun saveSchedule(
        diasSemana: List<Int>,
        hora: String,
        duracaoSegundos: Int,
        deviceId: String,
        onSuccess: (String) -> Unit,
        onError: (String) -> Unit
    ) {
        if (diasSemana.isEmpty() || hora.isBlank() || duracaoSegundos <= 0) {
            onError("Preencha todos os campos corretamente")
            return
        }

        val safeDeviceId = normalizeDeviceId(deviceId)

        if (safeDeviceId.isBlank()) {
            onError("Dispositivo IoT ainda não identificado. Aguarde o MQTT enviar o MAC.")
            return
        }

        val schedule = hashMapOf(
            "id" to java.util.UUID.randomUUID().toString(),
            "diasSemana" to diasSemana,
            "horaAcionamento" to hora,
            "tempoAcionamento" to duracaoSegundos,
            "ativo" to true,
            "deviceId" to safeDeviceId,
            "ownerUid" to auth.currentUser?.uid,
            "createdAt" to FieldValue.serverTimestamp(),
            "lastExecution" to FieldValue.serverTimestamp(),
            "scheduleCollection" to ""
        )

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                val deviceSet = deviceIds.map { normalizeDeviceId(it) }.filter { it.isNotBlank() }.toSet()
                if (safeDeviceId !in deviceSet) {
                    onError("Dispositivo IoT não pertence ao usuário logado")
                    return@fetchCurrentUserDeviceIds
                }

                val slotReads = SLOT_COLLECTIONS.map { slot ->
                    deviceScheduleCollection(safeDeviceId, slot).limit(1).get()
                }

                Tasks.whenAllSuccess<QuerySnapshot>(slotReads)
                    .addOnSuccessListener { snapshots ->
                        val occupied = snapshots
                            .mapIndexedNotNull { index, snapshot -> if (!snapshot.isEmpty) index else null }

                        val targetSlotIndex = when {
                            occupied.size < MAX_SCHEDULES_PER_DEVICE ->
                                SLOT_COLLECTIONS.indices.firstOrNull { index -> index !in occupied }
                            else -> snapshots
                                .mapIndexedNotNull { index, snapshot ->
                                    snapshot.documents.firstOrNull()?.let { doc ->
                                        index to ((doc.getTimestamp("createdAt") ?: Timestamp.now()).toDate().time)
                                    }
                                }
                                .minByOrNull { it.second }
                                ?.first
                        }

                        if (targetSlotIndex == null || targetSlotIndex !in SLOT_COLLECTIONS.indices) {
                            onError("Não foi possível localizar slot disponível para o agendamento")
                            return@addOnSuccessListener
                        }

                        val slotName = SLOT_COLLECTIONS[targetSlotIndex]
                        val slotRef = deviceScheduleCollection(safeDeviceId, slotName)
                        val scheduleId = schedule["id"] as String

                        if (occupied.size >= MAX_SCHEDULES_PER_DEVICE) {
                            snapshots[targetSlotIndex].documents.firstOrNull()?.reference?.delete()
                        }

                        slotRef.document(scheduleId)
                            .set(schedule + mapOf("scheduleCollection" to slotName))
                            .addOnSuccessListener {
                                Log.d(TAG, "Agendamento salvo no Firestore em $slotName. id=$scheduleId")
                                onSuccess("Agendamento salvo com sucesso (id: $scheduleId) ✅")
                            }
                            .addOnFailureListener { error ->
                                Log.e(TAG, "Falha ao salvar agendamento no Firestore", error)
                                onError("Erro ao salvar: ${error.message ?: "erro desconhecido"} ❌")
                            }
                    }
                    .addOnFailureListener { error ->
                        Log.e(TAG, "Falha ao consultar slots de agendamento", error)
                        onError("Erro ao preparar salvamento: ${error.message ?: "erro desconhecido"} ❌")
                    }
            },
            onError = { error ->
                onError(error.message ?: "Erro ao validar dispositivo")
            }
        )
    }

    override fun loadLatestSchedules(
        onSuccess: (List<IrrigationSchedule>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            Thread {
                val schedulesFromFunction = runCatching { fetchSchedulesFromFunction(null, emptySet()) }.getOrNull()
                if (schedulesFromFunction != null) { onSuccess(schedulesFromFunction); return@Thread }
                loadLatestSchedulesFromFirestore(null, emptySet(), onSuccess, onError)
            }.start()
            return
        }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                Thread {
                    val schedulesFromFunction = runCatching {
                        fetchSchedulesFromFunction(currentUid, deviceIds)
                    }.getOrNull()

                    if (schedulesFromFunction != null) { onSuccess(schedulesFromFunction); return@Thread }
                    loadLatestSchedulesFromFirestore(currentUid, deviceIds, onSuccess, onError)
                }.start()
            },
            onError = onError
        )
    }

    override fun removeSchedule(
        scheduleId: String,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    ) {
        if (scheduleId.isBlank()) return

        val currentUid = auth.currentUser?.uid
        if (currentUid == null) {
            onError(IllegalStateException("Usuário não autenticado"))
            return
        }

        deviceRepository.fetchCurrentUserDeviceIds(
            onSuccess = { deviceIds ->
                val normalizedIds = deviceIds.map { normalizeDeviceId(it) }.toSet()
                val searches = normalizedIds.flatMap { deviceId ->
                    SLOT_COLLECTIONS.map { slot ->
                        deviceScheduleCollection(deviceId, slot)
                            .whereEqualTo("id", scheduleId)
                            .limit(1)
                            .get()
                    }
                }

                Tasks.whenAllSuccess<QuerySnapshot>(searches)
                    .addOnSuccessListener { snapshots ->
                        val document = snapshots.firstOrNull { !it.isEmpty }?.documents?.firstOrNull()
                        if (document == null) {
                            onError(NoSuchElementException("Agendamento não encontrado"))
                            return@addOnSuccessListener
                        }
                        document.reference.delete()
                            .addOnSuccessListener { onSuccess() }
                            .addOnFailureListener(onError)
                    }
                    .addOnFailureListener(onError)
            },
            onError = onError
        )
    }

    private fun fetchSchedulesFromFunction(
        currentUid: String?,
        allowedDeviceIds: Set<String>
    ): List<IrrigationSchedule>? {
        val idToken = fetchIdTokenOrNull()
        val connection = (URL(GET_AGENDAMENTOS_URL).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = 10_000
            readTimeout = 10_000
            doInput = true
            setRequestProperty("Accept", "application/json")
            if (!idToken.isNullOrBlank()) setRequestProperty("Authorization", "Bearer $idToken")
        }

        return try {
            val responseCode = connection.responseCode
            if (responseCode !in 200..299) throw IllegalStateException("HTTP $responseCode ao buscar agendamentos")

            val payload = connection.inputStream.bufferedReader().use { it.readText() }
            val jsonArray = JSONArray(payload)
            val parsedSchedules = mutableListOf<IrrigationSchedule>()

            for (i in 0 until jsonArray.length()) {
                val obj = jsonArray.optJSONObject(i) ?: continue
                val diasJson = obj.optJSONArray("dias")
                val dias = mutableListOf<Int>()
                if (diasJson != null) {
                    for (idx in 0 until diasJson.length()) dias.add(diasJson.optInt(idx))
                }

                parsedSchedules.add(
                    IrrigationSchedule(
                        id = obj.optString("id"),
                        diasSemana = dias,
                        hora = obj.optString("hora"),
                        duracaoSegundos = obj.optInt("duracao"),
                        ativo = obj.optBoolean("ativo", true),
                        deviceId = normalizeDeviceId(obj.optString("device", "")),
                        ownerUid = obj.optString("ownerUid").takeIf { it.isNotBlank() },
                        createdAtMillis = System.currentTimeMillis()
                    )
                )
            }

            filterSchedulesByOwner(parsedSchedules, currentUid, allowedDeviceIds)
        } finally {
            connection.disconnect()
        }
    }

    private fun fetchIdTokenOrNull(): String? {
        val currentUser = auth.currentUser ?: return null
        return try {
            Tasks.await(currentUser.getIdToken(false)).token
        } catch (error: Exception) {
            Log.w(TAG, "Falha ao obter Firebase ID token: ${error.message}")
            null
        }
    }

    private fun loadLatestSchedulesFromFirestore(
        currentUid: String?,
        allowedDeviceIds: Set<String>,
        onSuccess: (List<IrrigationSchedule>) -> Unit,
        onError: (Throwable) -> Unit
    ) {
        val normalizedDeviceIds = allowedDeviceIds.map { normalizeDeviceId(it) }.filter { it.isNotBlank() }.distinct()

        if (normalizedDeviceIds.isEmpty()) { onSuccess(emptyList()); return }

        val tasks = normalizedDeviceIds.flatMap { deviceId ->
            SLOT_COLLECTIONS.map { slot -> deviceScheduleCollection(deviceId, slot).limit(1).get() }
        }

        Tasks.whenAllSuccess<QuerySnapshot>(tasks)
            .addOnSuccessListener { snapshots ->
                val items = snapshots
                    .flatMap { it.documents }
                    .map { doc ->
                        val timestamp = doc.getTimestamp("createdAt")
                        IrrigationSchedule(
                            id = doc.getString("id") ?: doc.id,
                            diasSemana = (doc.get("diasSemana") as? List<*>)
                                ?.mapNotNull { (it as? Number)?.toInt() }
                                ?: emptyList(),
                            hora = doc.getString("horaAcionamento") ?: doc.getString("hora") ?: "",
                            duracaoSegundos = (
                                doc.getLong("tempoAcionamento") ?: doc.getLong("duracaoSegundos") ?: 0L
                            ).toInt(),
                            ativo = doc.getBoolean("ativo") ?: true,
                            deviceId = normalizeDeviceId(doc.getString("deviceId")),
                            ownerUid = doc.getString("ownerUid"),
                            createdAtMillis = (timestamp ?: Timestamp.now()).toDate().time
                        )
                    }

                onSuccess(filterSchedulesByOwner(items, currentUid, allowedDeviceIds))
            }
            .addOnFailureListener(onError)
    }

    private fun filterSchedulesByOwner(
        schedules: List<IrrigationSchedule>,
        currentUid: String?,
        allowedDeviceIds: Set<String>
    ): List<IrrigationSchedule> {
        if (currentUid.isNullOrBlank()) return schedules
        val normalizedAllowed = allowedDeviceIds.map { normalizeDeviceId(it) }.toSet()
        return schedules.filter { schedule ->
            when {
                !schedule.ownerUid.isNullOrBlank() -> schedule.ownerUid == currentUid
                else -> normalizeDeviceId(schedule.deviceId) in normalizedAllowed
            }
        }
    }
}
