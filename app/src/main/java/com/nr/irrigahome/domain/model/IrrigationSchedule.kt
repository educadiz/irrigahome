package com.nr.irrigahome.domain.model

/**
 * Modelo de dados para agendamentos de irrigação.
 *
 * @param id               Identificador único do agendamento
 * @param diasSemana       Lista de dias da semana (1=Dom, 7=Sáb) para ativação
 * @param hora             Horário de ativação em formato "HH:mm"
 * @param duracaoSegundos  Tempo de acionamento da bomba em segundos
 * @param ativo            Flag para ativar/desativar agendamento sem deletar
 * @param deviceId         Identificador do dispositivo IoT vinculado
 * @param ownerUid         UID do usuário dono do agendamento
 * @param createdAtMillis  Timestamp de criação do agendamento em milissegundos
 */
data class IrrigationSchedule(
    val id: String = "",
    val diasSemana: List<Int> = emptyList(),
    val hora: String = "",
    val duracaoSegundos: Int = 0,
    val ativo: Boolean = true,
    val deviceId: String = "",
    val ownerUid: String? = null,
    val createdAtMillis: Long = 0L
)
