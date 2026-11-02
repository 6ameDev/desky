import AVFoundation
import SwiftUI
import Vision
import shared

/// QR pairing shortcut sheet (Android `ScanQrScreen` mirror): live camera +
/// Vision QR detection. Every decode feeds the shared
/// `PairingViewModel.onQrScanned` (the VM owns parsing via the shared
/// `QrCodeParser` plus the `isPairing`/paired guards, so a valid scan
/// auto-pairs on the identical path as manual entry): a valid scan stops the
/// session and dismisses (the pairing screen's existing `pairedDevice`
/// observer navigates on pair); an invalid one surfaces the shared error
/// string inline while the session keeps running for a re-aim.
struct ScanQrView: View {
    @ObservedObject var state: PairingViewState
    @Environment(\.dismiss) private var dismiss
    @State private var camera: CameraStatus = .checking
    @State private var torchOn = false

    var body: some View {
        ZStack {
            switch camera {
            case .checking:
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            case .ready:
                ZStack {
                    QrCameraView(state: state, torchOn: $torchOn, onAccepted: { dismiss() })
                        .ignoresSafeArea()
                    GeometryReader { geo in
                        let vf = min(geo.size.width - 70, 320)
                        VStack(spacing: 0) {
                            Color.black.opacity(0.6)
                                .overlay(alignment: .top) {
                                    HStack(spacing: 0) {
                                        Button { dismiss() } label: {
                                            Image(systemName: "xmark")
                                                .font(.system(size: 22))
                                                .foregroundColor(.white)
                                                .frame(width: 48, height: 48)
                                        }
                                        Spacer()
                                        Button { torchOn.toggle() } label: {
                                            Image(systemName: torchOn ? "flashlight.on.fill" : "flashlight.off.fill")
                                                .font(.system(size: 22))
                                                .foregroundColor(.white)
                                                .frame(width: 48, height: 48)
                                        }
                                    }
                                    .padding(.horizontal, 8)
                                    .padding(.vertical, 12)
                                }
                            HStack(spacing: 0) {
                                Color.black.opacity(0.6)
                                Color.clear
                                    .frame(width: vf, height: vf)
                                    .overlay { CornerBrackets() }
                                Color.black.opacity(0.6)
                            }
                            .frame(height: vf)
                            Color.black.opacity(0.6)
                                .overlay(alignment: .top) {
                                    VStack(spacing: 4) {
                                        Text("Point at Desky's QR code")
                                            .font(.system(size: 15))
                                            .foregroundColor(.white)
                                        Button("enter code instead") { dismiss() }
                                            .font(.system(size: 15, weight: .bold))
                                            .foregroundColor(.white)
                                        if let error = state.snapshot.error {
                                            Text(error)
                                                .font(.footnote)
                                                .foregroundColor(.red)
                                                .padding(8)
                                                .background(.ultraThinMaterial)
                                                .cornerRadius(8)
                                        }
                                    }
                                    .padding(.top, 24)
                                }
                        }
                        .ignoresSafeArea()
                    }
                }
            case .denied:
                VStack(spacing: 12) {
                    Text("Camera access is only used to read the Desky QR on the robot display.")
                        .font(.body)
                        .multilineTextAlignment(.center)
                    Button("Grant camera access") { requestCamera() }
                        .buttonStyle(.borderedProminent)
                    Text("Camera access was denied. Enable it in Settings to scan, or type the code manually.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                        .multilineTextAlignment(.center)
                    Button("Back to manual entry") { dismiss() }
                        .buttonStyle(.borderedProminent)
                }
                .padding()
            }
        }
        .onAppear(perform: checkCamera)
    }

    private func checkCamera() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            camera = .ready
        case .notDetermined:
            requestCamera()
        default:
            camera = .denied
        }
    }

    private func requestCamera() {
        AVCaptureDevice.requestAccess(for: .video) { granted in
            Task { @MainActor in camera = granted ? .ready : .denied }
        }
    }
}

private enum CameraStatus {
    case checking
    case ready
    case denied
}

private struct CornerBrackets: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            let length: CGFloat = 26
            let thick: CGFloat = 5
            let o = thick / 2
            Path { p in
                p.move(to: CGPoint(x: o, y: length))
                p.addLine(to: CGPoint(x: o, y: o))
                p.addLine(to: CGPoint(x: length, y: o))
                p.move(to: CGPoint(x: w - o, y: length))
                p.addLine(to: CGPoint(x: w - o, y: o))
                p.addLine(to: CGPoint(x: w - length, y: o))
                p.move(to: CGPoint(x: o, y: h - length))
                p.addLine(to: CGPoint(x: o, y: h - o))
                p.addLine(to: CGPoint(x: length, y: h - o))
                p.move(to: CGPoint(x: w - o, y: h - length))
                p.addLine(to: CGPoint(x: w - o, y: h - o))
                p.addLine(to: CGPoint(x: w - length, y: h - o))
            }
            .stroke(.white, lineWidth: thick)
        }
    }
}

/// Camera host: owns the `AVCaptureSession` + Vision QR request. Single-shot
/// stop on the first VALID scan (the VM confirms by entering `isPairing`);
/// consecutive duplicate decodes are dropped so one QR doesn't spam intents.
private struct QrCameraView: UIViewControllerRepresentable {
    @ObservedObject var state: PairingViewState
    @Binding var torchOn: Bool
    var onAccepted: () -> Void

    func makeUIViewController(context: Context) -> QrCameraController {
        let controller = QrCameraController()
        controller.onDecode = makeDecode(controller: controller)
        return controller
    }

    func updateUIViewController(_ controller: QrCameraController, context: Context) {
        controller.onDecode = makeDecode(controller: controller)
        controller.setTorch(torchOn)
    }

    private func makeDecode(controller: QrCameraController) -> (String) -> Void {
        { raw in
            Task { @MainActor in
                // Debounce: ignore frames while a pairing is in flight.
                guard !state.snapshot.isPairing else { return }
                state.onQrScanned(raw)
                if state.snapshot.isPairing {
                    controller.stop()
                    onAccepted()
                }
            }
        }
    }
}

private final class QrCameraController: UIViewController {
    var onDecode: ((String) -> Void)?

    private let session = AVCaptureSession()
    private var previewLayer: AVCaptureVideoPreviewLayer?
    private var torchDevice: AVCaptureDevice?
    private var lastForwarded: String?
    private lazy var qrRequest: VNDetectBarcodesRequest = {
        let request = VNDetectBarcodesRequest { [weak self] req, _ in
            self?.handle(req)
        }
        request.symbologies = [.qr]
        return request
    }()

    override func viewDidLoad() {
        super.viewDidLoad()
        guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back),
              let input = try? AVCaptureDeviceInput(device: device),
              session.canAddInput(input) else { return }
        torchDevice = device
        session.addInput(input)
        let output = AVCaptureVideoDataOutput()
        output.setSampleBufferDelegate(self, queue: DispatchQueue(label: "desky.qr-scan"))
        guard session.canAddOutput(output) else { return }
        session.addOutput(output)
        let layer = AVCaptureVideoPreviewLayer(session: session)
        layer.videoGravity = .resizeAspectFill
        layer.frame = view.bounds
        view.layer.addSublayer(layer)
        previewLayer = layer
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        previewLayer?.frame = view.bounds
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        start()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        stop()
    }

    private func start() {
        guard !session.isRunning else { return }
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            self?.session.startRunning()
        }
    }

    func stop() {
        guard session.isRunning else { return }
        session.stopRunning()
    }

    func setTorch(_ on: Bool) {
        guard let device = torchDevice, device.hasTorch else { return }
        do {
            try device.lockForConfiguration()
            device.torchMode = on ? .on : .off
            device.unlockForConfiguration()
        } catch {
            return
        }
    }

    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection,
    ) {
        guard session.isRunning else { return }
        guard let pixels = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let orientation: CGImagePropertyOrientation = {
            switch UIDevice.current.orientation {
            case .landscapeLeft: return .up
            case .landscapeRight: return .down
            case .portraitUpsideDown: return .left
            default: return .right
            }
        }()
        try? VNImageRequestHandler(cvPixelBuffer: pixels, orientation: orientation)
            .perform([qrRequest])
    }

    private func handle(_ request: VNRequest) {
        guard let results = request.results as? [VNBarcodeObservation],
              let raw = results.first?.payloadStringValue,
              !raw.isEmpty else { return }
        // Drop consecutive duplicates (one QR decodes ~every frame).
        guard raw != lastForwarded else { return }
        lastForwarded = raw
        onDecode?(raw)
    }
}

// The SDK marks this delegate nonisolated while UIKit infers the class
// MainActor-isolated; @preconcurrency keeps Swift 5 checking for the
// crossing. Sound by queue confinement: the delegate fires only on the
// serial scan queue, UI state hops to main via onDecode's Task.
extension QrCameraController: @preconcurrency AVCaptureVideoDataOutputSampleBufferDelegate {}
