package com.desky.client.ui.scan

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.desky.shared.domain.QrCodeParser
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import java.util.concurrent.atomic.AtomicBoolean

/**
 * QR pairing shortcut: CameraX preview + bundled ML Kit barcode analyzer
 * (QR-only, model in-APK so first scan works offline on the robot AP). A decoded payload that [QrCodeParser] accepts is a single-shot
 * result — the camera unbinds immediately and [onQrResult] feeds the shared
 * `PairingViewModel.onQrScanned` (auto-pair path, same guards as manual
 * entry). Anything else (non-QR, non-Desky payload) is ignored in place so
 * the user can re-aim; the camera never stops on invalid frames.
 *
 * The ViewModel is intentionally NOT touched here: the result returns via
 * [onQrResult] (the nav host round-trips it through `SavedStateHandle` into
 * the pairing screen's own ViewModel), so there is exactly one VM owner.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ScanQrScreen(
    onQrResult: (String) -> Unit,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    var granted by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA) ==
                PackageManager.PERMISSION_GRANTED,
        )
    }
    var askedOnce by remember { mutableStateOf(false) }
    val launcher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { isGranted ->
        askedOnce = true
        granted = isGranted
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Scan QR") },
                navigationIcon = {
                    OutlinedButton(onClick = onClose) { Text("Close") }
                },
            )
        },
    ) { padding ->
        if (!granted) {
            CameraRationale(
                modifier = Modifier.padding(padding).padding(16.dp),
                askedOnce = askedOnce,
                onGrant = { launcher.launch(Manifest.permission.CAMERA) },
                onClose = onClose,
            )
        } else {
            CameraPreview(
                modifier = Modifier.padding(padding),
                onQrResult = onQrResult,
            )
        }
    }
}

@Composable
private fun CameraRationale(
    modifier: Modifier = Modifier,
    askedOnce: Boolean,
    onGrant: () -> Unit,
    onClose: () -> Unit,
) {
    Column(
        modifier = modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text(
            "Camera access is only used to read the Desky QR on the robot " +
                "display. You can always type the 6-digit code instead.",
            style = MaterialTheme.typography.bodyMedium,
        )
        if (askedOnce) {
            // No rationale from the OS anymore == permanently denied:
            // point at Settings instead of re-firing a dead prompt.
            Text(
                "Camera permission was denied. Enable it in Settings to scan, " +
                    "or type the code manually.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
            )
            Button(onClick = onClose, modifier = Modifier.fillMaxWidth()) {
                Text("Back to manual entry")
            }
        } else {
            Button(onClick = onGrant, modifier = Modifier.fillMaxWidth()) {
                Text("Grant camera access")
            }
        }
    }
}

@Composable
private fun CameraPreview(
    modifier: Modifier = Modifier,
    onQrResult: (String) -> Unit,
) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val latestOnResult by rememberUpdatedState(onQrResult)
    // Single-shot stop: first VALID decode claims the result; in-flight
    // frames after the claim are dropped (debounce while finishing).
    val claimed = remember { AtomicBoolean(false) }
    // Main executor: analyzer + ML Kit listeners stay off the camera thread.
    val executor = remember(context) { ContextCompat.getMainExecutor(context) }
    var previewView by remember { mutableStateOf<PreviewView?>(null) }

    Box(modifier = modifier.fillMaxSize()) {
        AndroidView(
            factory = { ctx -> PreviewView(ctx).also { previewView = it } },
            modifier = Modifier.fillMaxSize(),
        )
        Text(
            "Point at the QR on the robot display",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onPrimary,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .padding(24.dp),
        )
    }

    DisposableEffect(previewView) {
        val view = previewView ?: return@DisposableEffect onDispose {}
        var provider: ProcessCameraProvider? = null
        val options = BarcodeScannerOptions.Builder()
            .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
            .build()
        val scanner = BarcodeScanning.getClient(options)
        val future = ProcessCameraProvider.getInstance(context)
        val listener = Runnable {
            val bound = try {
                future.get()
            } catch (e: Exception) {
                return@Runnable
            }
            provider = bound
            val preview = Preview.Builder().build().also {
                it.setSurfaceProvider(view.surfaceProvider)
            }
            val analysis = ImageAnalysis.Builder()
                .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                .build()
                .also { a ->
                    a.setAnalyzer(executor) { proxy ->
                        if (claimed.get()) {
                            proxy.close()
                            return@setAnalyzer
                        }
                        val mediaImage = proxy.image
                        if (mediaImage == null) {
                            proxy.close()
                            return@setAnalyzer
                        }
                        val image = InputImage.fromMediaImage(
                            mediaImage,
                            proxy.imageInfo.rotationDegrees,
                        )
                        scanner.process(image)
                            .addOnSuccessListener { barcodes ->
                                val raw = barcodes.firstOrNull()?.rawValue
                                val code = raw?.let(QrCodeParser::parse)
                                if (code != null && claimed.compareAndSet(false, true)) {
                                    bound.unbindAll()
                                    latestOnResult(code)
                                }
                            }
                            // Keep scanning through transient ML Kit errors.
                            .addOnCompleteListener { proxy.close() }
                    }
                }
            bound.unbindAll()
            bound.bindToLifecycle(
                lifecycleOwner,
                CameraSelector.DEFAULT_BACK_CAMERA,
                preview,
                analysis,
            )
        }
        future.addListener(listener, executor)
        onDispose {
            provider?.unbindAll()
            scanner.close()
        }
    }
}
