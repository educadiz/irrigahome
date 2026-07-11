package com.nr.irrigahome.util

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import javax.inject.Inject

class AppLifecycleObserver @Inject constructor() : DefaultLifecycleObserver {

    override fun onStop(owner: LifecycleOwner) {
        SessionManager.onAppBackgrounded()
    }

    override fun onStart(owner: LifecycleOwner) {
        if (SessionManager.shouldLogout()) {
            SessionManager.markAutoLogoutPending()
        } else {
            SessionManager.onAppForegrounded()
        }
    }
}
