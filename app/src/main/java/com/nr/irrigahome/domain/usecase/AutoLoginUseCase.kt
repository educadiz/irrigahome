package com.nr.irrigahome.domain.usecase

import com.nr.irrigahome.domain.repository.AuthRepository
import javax.inject.Inject

class AutoLoginUseCase @Inject constructor(
    private val repository: AuthRepository
) {
    operator fun invoke(): Boolean = repository.hasValidSession()
}

