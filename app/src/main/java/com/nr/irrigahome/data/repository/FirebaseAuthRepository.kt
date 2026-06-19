package com.nr.irrigahome.data.repository

import com.google.firebase.Timestamp
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.firestore.FirebaseFirestore
import com.nr.irrigahome.data.remote.DeviceRepository
import com.nr.irrigahome.domain.repository.AuthRepository
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class FirebaseAuthRepository @Inject constructor(
    private val auth: FirebaseAuth,
    private val firestore: FirebaseFirestore,
    private val deviceRepository: DeviceRepository
) : AuthRepository {

    override fun hasValidSession(): Boolean = auth.currentUser != null

    override fun login(
        email: String,
        password: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    ) {
        auth.signInWithEmailAndPassword(email, password)
            .addOnSuccessListener { onSuccess() }
            .addOnFailureListener(onFailure)
    }

    override fun register(
        name: String,
        email: String,
        password: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    ) {
        auth.createUserWithEmailAndPassword(email, password)
            .addOnSuccessListener {
                val uid = auth.currentUser?.uid
                if (uid == null) {
                    onFailure(IllegalStateException("Nao foi possível validar o usuário criado."))
                    return@addOnSuccessListener
                }

                val userMap = hashMapOf(
                    "name" to name,
                    "email" to email,
                    "createdAt" to Timestamp.now()
                )

                firestore.collection("users")
                    .document(uid)
                    .set(userMap)
                    .addOnSuccessListener {
                        deviceRepository.initializeNewUserStructure(
                            uid = uid,
                            name = name,
                            email = email,
                            onSuccess = onSuccess,
                            onError = { throwable ->
                                onFailure(throwable as? Exception ?: Exception(throwable))
                            }
                        )
                    }
                    .addOnFailureListener(onFailure)
            }
            .addOnFailureListener(onFailure)
    }

    override fun sendPasswordReset(
        email: String,
        onSuccess: () -> Unit,
        onFailure: (Exception) -> Unit
    ) {
        auth.sendPasswordResetEmail(email)
            .addOnSuccessListener { onSuccess() }
            .addOnFailureListener(onFailure)
    }

    override fun logout() {
        auth.signOut()
    }
}
