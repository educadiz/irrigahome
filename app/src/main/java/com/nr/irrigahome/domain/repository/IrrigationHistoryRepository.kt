package com.nr.irrigahome.domain.repository

import com.google.firebase.firestore.ListenerRegistration
import com.nr.irrigahome.domain.model.IrrigationHistoryEvent
import com.nr.irrigahome.domain.model.TriggerType
import java.util.Date

interface IrrigationHistoryRepository {

    fun loadIrrigationHistory(
        limit: Int = 50,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun observeIrrigationHistory(
        limit: Int = 100,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    ): ListenerRegistration

    fun loadHistoryByDateRange(
        startDate: Date,
        endDate: Date,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun loadHistoryByTriggerType(
        triggerType: TriggerType,
        limit: Int = 50,
        onSuccess: (List<IrrigationHistoryEvent>) -> Unit,
        onError: (Throwable) -> Unit
    )

    fun removeHistoryEvent(
        event: IrrigationHistoryEvent,
        onSuccess: () -> Unit,
        onError: (Throwable) -> Unit
    )

    fun shutdown()
}
