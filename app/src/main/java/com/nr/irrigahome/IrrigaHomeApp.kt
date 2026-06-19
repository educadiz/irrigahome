// Ciclo de vida da aplicação, para monitorar o estado da aplicação e realizar
// ações apropriadas, como liberar recursos ou pausar tarefas em segundo plano
// quando a aplicação estiver em segundo plano.

package com.nr.irrigahome

import android.app.Application
import androidx.lifecycle.ProcessLifecycleOwner
import com.nr.irrigahome.util.AppLifecycleObserver
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class IrrigaHomeApp : Application() {

    @Inject
    lateinit var appLifecycleObserver: AppLifecycleObserver

    override fun onCreate() {
        super.onCreate()

        ProcessLifecycleOwner.get().lifecycle.addObserver(appLifecycleObserver)
    }
}