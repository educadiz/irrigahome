package com.nr.irrigahome.domain.model

import com.google.firebase.Timestamp
import java.util.Date

/**
 * Modelo de dados para eventos de irrigação do Firestore
 */
data class IrrigationHistoryFirestore(
    val id: String = "",
    val deviceId: String = "",
    val historyCollection: String? = null,
    val ownerUid: String? = null,
    val deviceName: String? = null,
    val firmwareVersion: String? = null,
    val macAddress: String? = null,
    val eventId: String = "",
    val startAt: Timestamp? = null,
    val endAt: Timestamp? = null,
    val durationSec: Long = 0L,
    val trigger: String = "manual",
    val temperature: Int? = null,
    val soilHumidity: Int? = null,
    val airHumidity: Int? = null,
    val waterLevel: String? = null,
    val success: Boolean? = null,
    val stopReason: String? = null,
    val totalVolumeMl: Long? = null
)

/**
 * Modelo de dados para eventos de irrigação do endpoint HTTP
 */
data class IrrigationHistoryHttp(
    val id: String = "",
    val device: String = "",
    val historyCollection: String? = null,
    val ownerUid: String? = null,
    val eventId: String = "",
    val inicio: String = "",
    val fim: String = "",
    val duracao: Long = 0L,
    val trigger: String = "manual"
)

/**
 * Modelo unificado para apresentação na UI.
 *
 * @param success  null = informação ausente (evento antigo); false = falha confirmada.
 * @param stopReason Motivo de parada registrado pelo firmware (ex: "no_water").
 */
data class IrrigationHistoryEvent(
    val id: String,
    val eventId: String,
    val deviceId: String,
    val historyCollection: String? = null,
    val ownerUid: String? = null,
    val deviceName: String? = null,
    val firmwareVersion: String? = null,
    val macAddress: String? = null,
    val startTime: Date,
    val endTime: Date,
    val durationSeconds: Long,
    val triggerType: TriggerType,
    val temperature: Int? = null,
    val soilHumidity: Int? = null,
    val airHumidity: Int? = null,
    val waterLevel: String? = null,
    val success: Boolean? = null,
    val stopReason: String? = null,
    val totalVolumeMl: Long? = null
) {
    val durationMinutes: Long get() = durationSeconds / 60
    val durationSecondsRemainder: Long get() = durationSeconds % 60

    /** Verdadeiro quando o Firestore registrou success = false explicitamente. */
    val hasFailed: Boolean get() = success == false
}

/**
 * Enum para tipos de acionamento
 */
enum class TriggerType(val displayName: String) {
    MANUAL("Manual"),
    AUTOMATIC("Automático"),
    SCHEDULE("Agendado"),
    UNKNOWN("Desconhecido");

    companion object {
        fun fromString(value: String): TriggerType {
            return when (value.lowercase()) {
                "manual" -> MANUAL
                "automatic", "automatico" -> AUTOMATIC
                "schedule", "agendado" -> SCHEDULE
                else -> UNKNOWN
            }
        }
    }
}

/**
 * Estado de carregamento do histórico para a camada de apresentação
 */
sealed class HistoryLoadingState {
    object Loading : HistoryLoadingState()
    data class Success(val events: List<IrrigationHistoryEvent>) : HistoryLoadingState()
    data class Error(val exception: Throwable, val message: String) : HistoryLoadingState()
    object Empty : HistoryLoadingState()
}
