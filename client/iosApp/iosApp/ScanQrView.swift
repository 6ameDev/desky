import AVFoundation
import SwiftUI
import Vision
import shared

/// QR pairing shortcut sheet (Android `ScanQrScreen` mirror): live camera +
/// Vision QR detection. Every decode feeds the shared
/// `PairingViewModel.onQrScanned` (the VM owns parsing via the shared
/// `QrCodeParser` plus the `isPairing`/bonded guards, so a valid scan
/// auto-pairs on the identical path as manual entry): a valid scan stops the
/// session and dismisses (the pairing screen's existing `pairedDevice`
/// observer navigates on bond); an invalid one surfaces the shared error
/// string inline while the session keeps running for a re-aim.
struct ScanQrView: View {
    @ObservedObject var state: PairingViewState
    @Environment(\.dismiss) private var dismiss
    @State private var camera: CameraStatus = .checking

    var body: some View {
        NavigationStack {
            ZStack(alignment: .bottom) {
                switch camera {
                case .checking:
                    ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
                case .ready:
                    QrCameraView(state: state, onAccepted: { dismiss() })
                        .ignoresSafeArea()
                    VStack(spacing: 8) {
                        Text("Point at the QR on the robot display")
                            .font(.footnote)
                            .padding(8)
                            .background(.ultraThinMaterial)
                            .cornerRadius(8)
                        if let error = state.snapshot.error {
                            Text(error)
                                .font(.footnote)
                                .foregroundColor(.red)
                                .padding(8)
                                .background(.ultraThinMaterial)
                                .cornerRadius(8)
                        }
                    }
                    .padding(.bottom, 24)
                case .denied:
                    VStack(spacing: 12) {
                        Text("Camera access is only used to read the Desky QR on the robot display.")
                            .font(.body)
                            .multilineTextAlignment(.center)
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
            .navigationTitle("Scan QR")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
            }
        }
        .onAppear(perform: checkCamera)
    }

    private func checkCamera() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            camera = .ready
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                Task { @MainActor in camera = granted ? .ready : .denied }
            }
        default:
            camera = .denied
        }
    }
}

private enum CameraStatus {
    case checking
    case ready
    case denied
}

/// Camera host: owns the `AVCaptureSession` + Vision QR request. Single-shot
/// stop on the first VALID scan (the VM confirms by entering `isPairing`);
/// consecutive duplicate decodes are dropped so one QR doesn't spam intents.
private struct QrCameraView: UIViewControllerRepresentable {
    @ObservedObject var state: PairingViewState
    var onAccepted: () -> Void

    func makeUIViewController(context: Context) -> QrCameraController {
        let controller = QrCameraController()
        controller.onDecode = makeDecode(controller: controller)
        return controller
    }

    func updateUIViewController(_ controller: QrCameraController, context: Context) {
        controller.onDecode = makeDecode(controller: controller)
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

private final class QrCameraController: UIViewController, AVCaptureVideoDataOutputSampleBufferDelegate {
    var onDecode: ((String) -> Void)?

    private let session = AVCaptureSession()
    private var previewLayer: AVCaptureVideoPreviewLayer?
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
