package com.nr.irrigahome.domain.repository

import com.nr.irrigahome.domain.model.IrrigationSchedule

interface IrrigationScheduleRepository {

    fun saveSchedule(
        diasSemana: List<Int>,
        hora: String,
        duracaoSegundos: Int,
        deviceId: String,
        onSuccess: (String) -> Unit,
        onError: (String) -> Unit
    )

    fun loadLatestSchedules(
        onSuccess: (List<IrrigationSchedule>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun removeSchedule(
        scheduleId: String,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    )
}
