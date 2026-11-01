package com.desky.client.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.desky.client.ui.driving.DrivingScreen
import com.desky.client.ui.expressions.ExpressionsScreen
import com.desky.client.ui.home.HomeScreen
import com.desky.client.ui.pairing.PairingScreen
import com.desky.client.ui.scan.ScanQrScreen
import com.desky.client.ui.settings.SettingsScreen
import com.desky.shared.core.DeskyTab
import com.desky.shared.features.pairing.PairingViewModel
import com.desky.shared.features.session.SessionGateViewModel
import org.koin.androidx.compose.koinViewModel

object DeskyRoutes {
    const val PAIRING = "pairing"
    const val HOME = "home"
    const val DRIVING = "driving"
    const val EXPRESSIONS = "expressions"
    const val SETTINGS = "settings"
    const val SCAN = "scan"
}

/** SavedStateHandle key carrying the decoded QR string SCAN -> PAIRING. */
private const val SCAN_RESULT_KEY = "qr_raw"

private fun DeskyTab.route(): String = when (this) {
    DeskyTab.HOME -> DeskyRoutes.HOME
    DeskyTab.SETTINGS -> DeskyRoutes.SETTINGS
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
    val backStack by navController.currentBackStackEntryAsState()
    val currentRoute = backStack?.destination?.route
    // Tabs show only on the two tab roots. Drive and Expressions are pushed
    // from Home tiles, so the bar hides there and exit returns to Home. The
    // bar floats over content; tab screens reserve bottom clearance for it.
    val currentTab = when (currentRoute) {
        DeskyRoutes.HOME -> DeskyTab.HOME
        DeskyRoutes.SETTINGS -> DeskyTab.SETTINGS
        else -> null
    }
    fun selectTab(tab: DeskyTab) {
        navController.navigate(tab.route()) {
            popUpTo(DeskyRoutes.HOME) { saveState = true }
            launchSingleTop = true
            restoreState = true
        }
    }
    Box(modifier = Modifier.fillMaxSize()) {
        NavHost(
            navController = navController,
            startDestination = DeskyRoutes.PAIRING,
            modifier = Modifier.fillMaxSize(),
        ) {
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
            composable(DeskyRoutes.SETTINGS) {
                SettingsScreen()
            }
        }
        if (currentTab != null) {
            DeskyTabBar(
                current = currentTab,
                onTab = ::selectTab,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(horizontal = 20.dp)
                    .navigationBarsPadding()
                    .padding(bottom = 16.dp),
            )
        }
    }
}
