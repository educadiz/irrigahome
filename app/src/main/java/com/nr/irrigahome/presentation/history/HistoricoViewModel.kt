package com.nr.irrigahome.presentation.history

import android.util.Log
import androidx.compose.runtime.State
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.google.firebase.firestore.ListenerRegistration
import com.nr.irrigahome.data.remote.IrrigationHistoryRepository
import com.nr.irrigahome.domain.model.HistoryLoadingState
import com.nr.irrigahome.domain.model.IrrigationHistoryEvent
import com.nr.irrigahome.domain.model.TriggerType
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.util.Calendar
import java.util.Date
import javax.inject.Inject

@HiltViewModel
class HistoricoViewModel @Inject constructor(
    private val repository: IrrigationHistoryRepository
) : ViewModel() {

    companion object {
        private const val TAG = "HistoricoViewModel"
    }

    private var historyListener: ListenerRegistration? = null

    private val _loadingState = mutableStateOf<HistoryLoadingState>(HistoryLoadingState.Loading)
    val loadingState: State<HistoryLoadingState> = _loadingState

    private val _allEvents = mutableStateListOf<IrrigationHistoryEvent>()
    val allEvents: List<IrrigationHistoryEvent> = _allEvents

    private val _filteredEvents = mutableStateListOf<IrrigationHistoryEvent>()
    val filteredEvents: List<IrrigationHistoryEvent> = _filteredEvents

    private val _selectedFilterType = mutableStateOf<TriggerType?>(null)
    val selectedFilterType: State<TriggerType?> = _selectedFilterType

    private val _dateRangeFilter = mutableStateOf<Pair<Date?, Date?>>(Pair(null, null))
    val dateRangeFilter: State<Pair<Date?, Date?>> = _dateRangeFilter

    private val _actionStatus = mutableStateOf("")
    val actionStatus: State<String> = _actionStatus

    init {
        observeHistoryChanges()
    }

    fun loadHistory() {
        _loadingState.value = HistoryLoadingState.Loading
        _allEvents.clear()
        _filteredEvents.clear()

        viewModelScope.launch(Dispatchers.IO) {
            repository.loadIrrigationHistory(
                limit = 100,
                onSuccess = { events ->
                    updateOnMain {
                        _allEvents.clear()
                        _allEvents.addAll(events)
                        applyFilters()
                        _loadingState.value = if (events.isEmpty()) HistoryLoadingState.Empty
                        else HistoryLoadingState.Success(events)
                        Log.d(TAG, "✅ Histórico carregado: ${events.size} registros")
                    }
                },
                onError = { error ->
                    updateOnMain {
                        val message = error.message ?: "Erro desconhecido"
                        _loadingState.value = HistoryLoadingState.Error(error, message)
                        Log.e(TAG, "❌ Erro ao carregar histórico: $message", error)
                    }
                }
            )
        }
    }

    private fun observeHistoryChanges() {
        historyListener?.remove()
        historyListener = repository.observeIrrigationHistory(
            limit = 100,
            onSuccess = { events ->
                updateOnMain {
                    _allEvents.clear()
                    _allEvents.addAll(events)
                    applyFilters()
                    _loadingState.value = if (events.isEmpty()) HistoryLoadingState.Empty
                    else HistoryLoadingState.Success(events)
                    Log.d(TAG, "🔄 Histórico atualizado em tempo real: ${events.size} registros")
                }
            },
            onError = { error ->
                updateOnMain {
                    val message = error.message ?: "Erro desconhecido"
                    _loadingState.value = HistoryLoadingState.Error(error, message)
                    Log.e(TAG, "❌ Erro no listener do histórico: $message", error)
                }
            }
        )
    }

    fun loadHistoryByDateRange(startDate: Date, endDate: Date) {
        _loadingState.value = HistoryLoadingState.Loading
        _allEvents.clear()
        _filteredEvents.clear()

        viewModelScope.launch(Dispatchers.IO) {
            repository.loadHistoryByDateRange(
                startDate = startDate,
                endDate = endDate,
                onSuccess = { events ->
                    updateOnMain {
                        _allEvents.clear()
                        _allEvents.addAll(events)
                        _loadingState.value = if (events.isEmpty()) HistoryLoadingState.Empty
                        else HistoryLoadingState.Success(events)
                        Log.d(TAG, "✅ Histórico (período) carregado: ${events.size} registros")
                    }
                },
                onError = { error ->
                    updateOnMain {
                        val message = error.message ?: "Erro desconhecido"
                        _loadingState.value = HistoryLoadingState.Error(error, message)
                        Log.e(TAG, "❌ Erro ao carregar histórico (período): $message", error)
                    }
                }
            )
        }
    }

    fun filterByTriggerType(triggerType: TriggerType?) {
        _selectedFilterType.value = triggerType
        applyFilters()
    }

    fun setDateRangeFilter(startDate: Date?, endDate: Date?) {
        _dateRangeFilter.value = Pair(startDate, endDate)
        applyFilters()
    }

    fun loadTodayHistory() {
        val today = Calendar.getInstance().apply {
            set(Calendar.HOUR_OF_DAY, 0); set(Calendar.MINUTE, 0)
            set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
        }.time

        val tomorrow = Calendar.getInstance().apply {
            add(Calendar.DAY_OF_MONTH, 1)
            set(Calendar.HOUR_OF_DAY, 0); set(Calendar.MINUTE, 0)
            set(Calendar.SECOND, 0); set(Calendar.MILLISECOND, 0)
        }.time

        loadHistoryByDateRange(today, tomorrow)
    }

    fun loadLastWeekHistory() {
        val today = Date()
        loadHistoryByDateRange(Date(today.time - (7 * 24 * 60 * 60 * 1000L)), today)
    }

    fun loadLastMonthHistory() {
        val today = Date()
        loadHistoryByDateRange(Date(today.time - (30 * 24 * 60 * 60 * 1000L)), today)
    }

    fun clearFilters() {
        _selectedFilterType.value = null
        _dateRangeFilter.value = Pair(null, null)
        applyFilters()
    }

    fun removeHistoryEvent(event: IrrigationHistoryEvent) {
        repository.removeHistoryEvent(
            event = event,
            onSuccess = {
                updateOnMain { _actionStatus.value = "Registro removido com sucesso" }
            },
            onError = { error ->
                updateOnMain {
                    _actionStatus.value = error.message ?: "Erro ao remover registro"
                    Log.e(TAG, "Erro ao remover histórico", error)
                }
            }
        )
    }

    fun clearActionStatus() {
        _actionStatus.value = ""
    }

    private fun applyFilters() {
        var filtered = _allEvents.toList()

        _selectedFilterType.value?.let { filterType ->
            filtered = filtered.filter { it.triggerType == filterType }
        }

        val (startDate, endDate) = _dateRangeFilter.value
        if (startDate != null && endDate != null) {
            filtered = filtered.filter { event ->
                event.startTime >= startDate && event.startTime <= endDate
            }
        }

        _filteredEvents.clear()
        _filteredEvents.addAll(filtered)
    }

    private fun updateOnMain(update: () -> Unit) {
        if (Dispatchers.Main.immediate.isDispatchNeeded(viewModelScope.coroutineContext)) {
            viewModelScope.launch(Dispatchers.Main.immediate) { update() }
        } else {
            update()
        }
    }

    override fun onCleared() {
        historyListener?.remove()
        historyListener = null
        repository.shutdown()
        super.onCleared()
        Log.d(TAG, "HistoricoViewModel cleared")
    }
}
