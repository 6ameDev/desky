package com.desky.client.ui.scan

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.Camera
import androidx.camera.core.CameraSelector
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.lifecycle.compose.LocalLifecycleOwner
import com.desky.shared.core.QrCodeParser
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import java.util.concurrent.atomic.AtomicBoolean

/**
 * QR pairing shortcut: CameraX preview + bundled ML Kit barcode analyzer
 * (QR-only, model in-APK so the first scan works fully offline). A decoded payload that [QrCodeParser] accepts is a single-shot
 * result — the camera unbinds immediately and [onQrResult] feeds the shared
 * `PairingViewModel.onQrScanned` (auto-pair path, same guards as manual
 * entry). Anything else (non-QR, non-Desky payload) is ignored in place so
 * the user can re-aim; the camera never stops on invalid frames.
 *
 * The ViewModel is intentionally NOT touched here: the result returns via
 * [onQrResult] (the nav host round-trips it through `SavedStateHandle` into
 * the pairing screen's own ViewModel), so there is exactly one VM owner.
 */
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

    if (!granted) {
        CameraRationale(
            modifier = Modifier.fillMaxSize().padding(16.dp),
            askedOnce = askedOnce,
            onGrant = { launcher.launch(Manifest.permission.CAMERA) },
            onClose = onClose,
        )
    } else {
        CameraPreview(
            modifier = Modifier.fillMaxSize(),
            onQrResult = onQrResult,
            onClose = onClose,
        )
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
    onClose: () -> Unit,
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
    var camera by remember { mutableStateOf<Camera?>(null) }
    var torchOn by remember { mutableStateOf(false) }
    LaunchedEffect(torchOn, camera) {
        runCatching { camera?.cameraControl?.enableTorch(torchOn) }
    }
    val scrim = Color.Black.copy(alpha = 0.6f)

    BoxWithConstraints(modifier = modifier.fillMaxSize()) {
        val viewfinder = (maxWidth - 70.dp).coerceAtMost(320.dp).coerceAtLeast(160.dp)
        AndroidView(
            factory = { ctx -> PreviewView(ctx).also { previewView = it } },
            modifier = Modifier.fillMaxSize(),
        )
        Column(modifier = Modifier.fillMaxSize()) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(scrim),
            ) {
                Row(
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier
                        .fillMaxWidth()
                        .windowInsetsPadding(WindowInsets.statusBars)
                        .padding(horizontal = 8.dp, vertical = 12.dp),
                ) {
                    CloseButton(onClose = onClose)
                    TorchButton(
                        on = torchOn,
                        onToggle = { torchOn = !torchOn },
                    )
                }
            }
            Row(modifier = Modifier.fillMaxWidth()) {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(viewfinder)
                        .background(scrim),
                )
                Box(modifier = Modifier.size(viewfinder)) {
                    CornerBrackets(modifier = Modifier.fillMaxSize())
                }
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .height(viewfinder)
                        .background(scrim),
                )
            }
            Box(
                contentAlignment = Alignment.TopCenter,
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(scrim),
            ) {
                Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    modifier = Modifier.padding(top = 24.dp),
                ) {
                    Text(
                        "Point at Desky's QR code",
                        color = Color.White,
                        fontSize = 15.sp,
                    )
                    TextButton(onClick = onClose) {
                        Text(
                            "enter code instead",
                            color = Color.White,
                            fontSize = 15.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                }
            }
        }
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
                        val image = imageProxyToInputImage(proxy)
                        if (image == null) {
                            proxy.close()
                            return@setAnalyzer
                        }
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
            camera = bound.bindToLifecycle(
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

@Composable
private fun CloseButton(onClose: () -> Unit) {
    Box(
        contentAlignment = Alignment.Center,
        modifier = Modifier
            .size(48.dp)
            .clickable(
                indication = null,
                interactionSource = remember { MutableInteractionSource() },
                onClick = onClose,
            ),
    ) {
        Text(
            "✕",
            color = Color.White,
            fontSize = 22.sp,
        )
    }
}

@Composable
private fun TorchButton(
    on: Boolean,
    onToggle: () -> Unit,
) {
    Box(
        contentAlignment = Alignment.Center,
        modifier = Modifier
            .size(48.dp)
            .clickable(
                indication = null,
                interactionSource = remember { MutableInteractionSource() },
                onClick = onToggle,
            ),
    ) {
        Canvas(modifier = Modifier.size(24.dp)) {
            val w = size.width
            val h = size.height
            val bolt = Path().apply {
                moveTo(w * 0.55f, 0f)
                lineTo(w * 0.25f, h * 0.55f)
                lineTo(w * 0.47f, h * 0.55f)
                lineTo(w * 0.4f, h)
                lineTo(w * 0.75f, h * 0.42f)
                lineTo(w * 0.52f, h * 0.42f)
                close()
            }
            drawPath(
                bolt,
                if (on) Color.White else Color.White.copy(alpha = 0.5f),
            )
        }
    }
}

@Composable
private fun CornerBrackets(modifier: Modifier = Modifier) {
    Canvas(modifier = modifier) {
        val length = 26.dp.toPx()
        val thickness = 5.dp.toPx()
        val o = thickness / 2
        val w = size.width
        val h = size.height
        val c = Color.White
        drawLine(c, Offset(o, length), Offset(o, o), thickness)
        drawLine(c, Offset(o, o), Offset(length, o), thickness)
        drawLine(c, Offset(w - o, length), Offset(w - o, o), thickness)
        drawLine(c, Offset(w - o, o), Offset(w - length, o), thickness)
        drawLine(c, Offset(o, h - length), Offset(o, h - o), thickness)
        drawLine(c, Offset(o, h - o), Offset(length, h - o), thickness)
        drawLine(c, Offset(w - o, h - length), Offset(w - o, h - o), thickness)
        drawLine(c, Offset(w - o, h - o), Offset(w - length, h - o), thickness)
    }
}

@androidx.annotation.OptIn(ExperimentalGetImage::class)
private fun imageProxyToInputImage(proxy: ImageProxy): InputImage? {
    val mediaImage = proxy.image ?: return null
    return InputImage.fromMediaImage(mediaImage, proxy.imageInfo.rotationDegrees)
}
