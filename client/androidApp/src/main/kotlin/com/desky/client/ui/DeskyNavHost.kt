package com.desky.client.ui

import androidx.compose.runtime.Composable
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.desky.client.ui.control.ControlScreen
import com.desky.client.ui.pairing.PairingScreen
import com.desky.client.ui.settings.SettingsScreen

object DeskyRoutes {
    const val PAIRING = "pairing"
    const val CONTROL = "control"
    const val SETTINGS = "settings"
}

@Composable
fun DeskyNavHost() {
    val navController = rememberNavController()
    NavHost(navController = navController, startDestination = DeskyRoutes.PAIRING) {
        composable(DeskyRoutes.PAIRING) {
            PairingScreen(
                onPaired = {
                    navController.navigate(DeskyRoutes.CONTROL) {
                        popUpTo(DeskyRoutes.PAIRING) { inclusive = true }
                    }
                },
                onOpenControls = {
                    navController.navigate(DeskyRoutes.CONTROL) {
                        popUpTo(DeskyRoutes.PAIRING) { inclusive = true }
                    }
                },
            )
        }
        composable(DeskyRoutes.CONTROL) {
            ControlScreen(
                onOpenSettings = { navController.navigate(DeskyRoutes.SETTINGS) },
            )
        }
        composable(DeskyRoutes.SETTINGS) {
            SettingsScreen(
                onBack = { navController.popBackStack() },
                onUnpaired = {
                    navController.navigate(DeskyRoutes.PAIRING) {
                        popUpTo(DeskyRoutes.CONTROL) { inclusive = true }
                    }
                },
            )
        }
    }
}
