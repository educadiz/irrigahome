package com.nr.irrigahome.util

import com.nr.irrigahome.domain.model.ConflictCheckResult
import com.nr.irrigahome.domain.model.IrrigationSchedule
import java.util.Calendar

object ScheduleConflictDetector {

    private const val ALERT_WINDOW_MINUTES = 60L

    /**
     * Verifica se existe algum agendamento ativo que dispara nos próximos
     * [ALERT_WINDOW_MINUTES] minutos. Retorna o conflito mais próximo ou NoConflict.
     *
     * @param activeSchedules Lista de agendamentos do ViewModel (pode estar vazia).
     * @param nowCalendar     Instante de referência — injetável para testes.
     */
    fun check(
        activeSchedules: List<IrrigationSchedule>,
        nowCalendar: Calendar = Calendar.getInstance()
    ): ConflictCheckResult {

        if (activeSchedules.isEmpty()) return ConflictCheckResult.NoConflict

        val nowDayOfWeek = nowCalendar.get(Calendar.DAY_OF_WEEK)
        val nowMinutes = nowCalendar.get(Calendar.HOUR_OF_DAY) * 60 +
                nowCalendar.get(Calendar.MINUTE)

        val candidates = activeSchedules
            .filter { it.ativo && it.diasSemana.isNotEmpty() && it.hora.isNotBlank() }
            .mapNotNull { schedule ->
                val minutesUntil = minutesUntilNextOccurrence(
                    schedule = schedule,
                    nowDayOfWeek = nowDayOfWeek,
                    nowMinutes = nowMinutes
                ) ?: return@mapNotNull null

                if (minutesUntil in 0..ALERT_WINDOW_MINUTES) schedule to minutesUntil
                else null
            }

        if (candidates.isEmpty()) return ConflictCheckResult.NoConflict

        val (nearest, minutesUntil) = candidates.minByOrNull { it.second }!!

        return ConflictCheckResult.ConflictDetected(
            schedule = nearest,
            scheduledTimeLabel = nearest.hora,
            minutesUntilStart = minutesUntil
        )
    }

    private fun minutesUntilNextOccurrence(
        schedule: IrrigationSchedule,
        nowDayOfWeek: Int,
        nowMinutes: Int
    ): Long? {
        val (schedHour, schedMin) = parseHora(schedule.hora) ?: return null
        val schedMinutesInDay = schedHour * 60 + schedMin

        if (schedule.diasSemana.contains(nowDayOfWeek)) {
            val diff = schedMinutesInDay - nowMinutes
            if (diff >= 0) return diff.toLong()
        }

        val minutesToMidnight = (24 * 60) - nowMinutes

        for (daysAhead in 1..6) {
            val candidateDay = ((nowDayOfWeek - 1 + daysAhead) % 7) + 1
            if (schedule.diasSemana.contains(candidateDay)) {
                val fullDaysInBetween = (daysAhead - 1) * 24 * 60
                return (minutesToMidnight + fullDaysInBetween + schedMinutesInDay).toLong()
            }
        }

        return null
    }

    private fun parseHora(hora: String): Pair<Int, Int>? {
        return try {
            val parts = hora.trim().split(":")
            if (parts.size < 2) return null
            val h = parts[0].trim().toInt()
            val m = parts[1].trim().toInt()
            if (h !in 0..23 || m !in 0..59) return null
            h to m
        } catch (_: NumberFormatException) {
            null
        }
    }
}
