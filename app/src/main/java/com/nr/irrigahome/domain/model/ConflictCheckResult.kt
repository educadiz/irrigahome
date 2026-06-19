package com.nr.irrigahome.domain.model

/**
 * Resultado da verificação de conflito entre irrigação manual e agendamentos ativos.
 */
sealed class ConflictCheckResult {
    object NoConflict : ConflictCheckResult()

    data class ConflictDetected(
        val schedule: IrrigationSchedule,
        val scheduledTimeLabel: String,
        val minutesUntilStart: Long
    ) : ConflictCheckResult()
}
