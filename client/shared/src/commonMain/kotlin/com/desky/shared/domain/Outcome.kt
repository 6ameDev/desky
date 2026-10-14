package com.desky.shared.domain

/**
 * Domain-level result type. Plain sealed interface (instead of Kotlin
 * [Result]) so the type stays visible to SKIE-generated Swift and to any
 * future Koin/ViewModel consumers (T6).
 */
sealed interface Outcome<out T> {
    data class Success<T>(val data: T) : Outcome<T>
    data class Failure(val reason: String, val cause: Throwable? = null) : Outcome<Nothing>
}
