package com.nr.irrigahome.util

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import com.nr.irrigahome.domain.usecase.LogoutUseCase
import javax.inject.Inject

class AppLifecycleObserver @Inject constructor(
    private val logoutUseCase: LogoutUseCase
) : DefaultLifecycleObserver {

    override fun onStop(owner: LifecycleOwner) {
        SessionManager.onAppBackgrounded()
    }

    override fun onStart(owner: LifecycleOwner) {
        if (SessionManager.shouldLogout()) {
            logoutUseCase()
            SessionManager.resetSession()
        } else {
            SessionManager.onAppForegrounded()
        }
    }
}
