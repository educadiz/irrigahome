package com.nr.irrigahome.di

import com.nr.irrigahome.data.remote.IrrigationHistoryRepository
import com.nr.irrigahome.data.remote.IrrigationScheduleRepository
import com.nr.irrigahome.data.repository.FirebaseAuthRepository
import com.nr.irrigahome.domain.repository.AuthRepository
import com.nr.irrigahome.domain.repository.IrrigationHistoryRepository as IIrrigationHistoryRepository
import com.nr.irrigahome.domain.repository.IrrigationScheduleRepository as IIrrigationScheduleRepository
import dagger.Binds
import dagger.Module
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
abstract class RepositoryModule {

    @Binds
    @Singleton
    abstract fun bindAuthRepository(impl: FirebaseAuthRepository): AuthRepository

    @Binds
    @Singleton
    abstract fun bindIrrigationHistoryRepository(impl: IrrigationHistoryRepository): IIrrigationHistoryRepository

    @Binds
    @Singleton
    abstract fun bindIrrigationScheduleRepository(impl: IrrigationScheduleRepository): IIrrigationScheduleRepository
}
