package com.nr.irrigahome.domain.repository

interface AuthRepository {
    fun hasValidSession(): Boolean

    fun login(
        email: String,
        password: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    )

    fun register(
        name: String,
        email: String,
        password: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    )

    fun sendPasswordReset(
        email: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    )

    fun logout()
}

