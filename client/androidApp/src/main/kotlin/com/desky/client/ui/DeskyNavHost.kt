package com.desky.client.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.desky.client.ui.control.ControlScreen
import com.desky.client.ui.pairing.PairingScreen
import com.desky.client.ui.settings.SettingsScreen
import com.desky.shared.presentation.SessionGateViewModel
import org.koin.androidx.compose.koinViewModel

object DeskyRoutes {
    const val PAIRING = "pairing"
    const val CONTROL = "control"
    const val SETTINGS = "settings"
}

@Composable
fun DeskyNavHost(
    navController: NavHostController = rememberNavController(),
    gateVm: SessionGateViewModel = koinViewModel(),
) {
    val provenDead by gateVm.provenDead.collectAsStateWithLifecycle()
    LaunchedEffect(provenDead) {
        if (provenDead && navController.currentDestination?.route != DeskyRoutes.PAIRING) {
            navController.navigate(DeskyRoutes.PAIRING) {
                popUpTo(0) { inclusive = true }
                launchSingleTop = true
            }
        }
    }
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
            )
        }
    }
}
