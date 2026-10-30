package com.desky.client.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.desky.client.ui.control.ControlScreen
import com.desky.client.ui.driving.DrivingScreen
import com.desky.client.ui.expressions.ExpressionsScreen
import com.desky.client.ui.home.HomeScreen
import com.desky.client.ui.pairing.PairingScreen
import com.desky.client.ui.scan.ScanQrScreen
import com.desky.client.ui.settings.SettingsScreen
import com.desky.shared.features.pairing.PairingViewModel
import com.desky.shared.features.session.SessionGateViewModel
import org.koin.androidx.compose.koinViewModel

object DeskyRoutes {
    const val PAIRING = "pairing"
    const val HOME = "home"
    const val DRIVING = "driving"
    const val EXPRESSIONS = "expressions"
    const val CONTROL = "control"
    const val SETTINGS = "settings"
    const val SCAN = "scan"
}

/** SavedStateHandle key carrying the decoded QR string SCAN -> PAIRING. */
private const val SCAN_RESULT_KEY = "qr_raw"

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
        composable(DeskyRoutes.PAIRING) { entry ->
            // Single VM owner for pairing (ScanQrScreen never mints one):
            // the scan result round-trips through SavedStateHandle so the
            // decoded code feeds this VM's onQrScanned (auto-pair path).
            // Consumed via current-entry observation (no getStateFlow: the
            // resolved lifecycle 2.10 split artifact doesn't export it):
            // refires on first composition (no-op, key absent) and on every
            // return from SCAN.
            val vm: PairingViewModel = koinViewModel()
            val currentEntry by navController.currentBackStackEntryAsState()
            LaunchedEffect(currentEntry?.id) {
                if (currentEntry?.destination?.route == DeskyRoutes.PAIRING) {
                    val raw: String? = entry.savedStateHandle.remove(SCAN_RESULT_KEY)
                    if (raw != null) vm.onQrScanned(raw)
                }
            }
            PairingScreen(
                vm = vm,
                onPaired = {
                    navController.navigate(DeskyRoutes.HOME) {
                        popUpTo(DeskyRoutes.PAIRING) { inclusive = true }
                    }
                },
                onOpenControls = {
                    navController.navigate(DeskyRoutes.HOME) {
                        popUpTo(DeskyRoutes.PAIRING) { inclusive = true }
                    }
                },
                onOpenScan = {
                    navController.navigate(DeskyRoutes.SCAN)
                },
            )
        }
        composable(DeskyRoutes.SCAN) {
            ScanQrScreen(
                onQrResult = { raw ->
                    navController.previousBackStackEntry
                        ?.savedStateHandle
                        ?.set(SCAN_RESULT_KEY, raw)
                    navController.popBackStack()
                },
                onClose = { navController.popBackStack() },
            )
        }
        composable(DeskyRoutes.HOME) {
            HomeScreen(
                onDrive = { navController.navigate(DeskyRoutes.DRIVING) },
                onExpressions = { navController.navigate(DeskyRoutes.EXPRESSIONS) },
                onSettings = { navController.navigate(DeskyRoutes.SETTINGS) },
            )
        }
        composable(DeskyRoutes.DRIVING) {
            DrivingScreen(
                onExit = { navController.popBackStack() },
            )
        }
        composable(DeskyRoutes.EXPRESSIONS) {
            ExpressionsScreen(
                onExit = { navController.popBackStack() },
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
