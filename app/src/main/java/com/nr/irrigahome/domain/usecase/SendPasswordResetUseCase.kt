package com.nr.irrigahome.domain.usecase

import com.nr.irrigahome.domain.repository.AuthRepository
import javax.inject.Inject

class SendPasswordResetUseCase @Inject constructor(
    private val repository: AuthRepository
) {
    operator fun invoke(
        email: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    ) {
        repository.sendPasswordReset(
            email = email,
            onSuccess = onSuccess,
            onFailure = onFailure
        )
    }
}

