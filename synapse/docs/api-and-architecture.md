# Synapse: Architecture Entities & Implementation Strategy

To unite your ESP32 Core, ESP32 Head, Raspberry Pi Hub, and KMP Mobile App under a single cohesive communication framework, Synapse provides standard architecture components and a unified domain API.

## 1. Key Entities & Architecture Naming

Mirror these core components across your folder structures for C++ (ESP32), Python/C++ (Pi), and Kotlin (KMP):

- **`MessageBus` (or `TransportDispatcher`):** The primary entry-point facade. Your domain code interacts _only_ with this class to send or receive data, agnostic of the underlying transport layer.

- **`EventChannel` (MQTT + App QoS 1):** Manages reliable, ack-tracked commands and state changes. Handles message IDs, local timers, and automatic 3-retry loops on top of PicoMQTT's QoS 0 broker.

- **`TelemetryChannel` (UDP):** Manages high-frequency, non-blocking packet broadcast and reception for sensor streams.

- **`StreamManager` (Raw TCP):** Manages lifecycle bindings for heavy binary streaming (opening ports, buffer chunking, closing connections).

- **`NodeRegistry` (Discovery):** Tracks online nodes on the AP network, their IP addresses, and their active functional capabilities.


## 2. The Unified Domain API

Your domain code interacts with a clean, transport-agnostic interface:

C++

```
// Publishing a Reliable Event (e.g., Sending the QR code from Core to Head)
Synapse::getInstance().publishEvent(
    /* target role */ "head", 
    /* action */ "display_qr", 
    /* payload */ "{ \"pin\": \"123456\" }", 
    /* expectAck */ true // Triggers the App-Layer QoS 1 3-retry loop automatically!
);

// Listening for Events on the Head (ESP32-CAM)
Synapse::getInstance().onEvent("display_qr", [](const JsonDocument& payload) {
    String pin = payload["pin"];
    renderQRCode(pin);
});

// Pushing Fast Telemetry (UDP)
TelemetryPacket pkt = { .imu_x = 0.12, .battery = 85 };
Synapse::getInstance().sendTelemetry("sensors/imu", pkt);
```

## 3. Cross-Platform Implementation Strategy

- **A. ESP32 Modules (Core & Head) — C++ (FreeRTOS):** Wraps `PicoMQTT` (broker on Core / client on Head), `AsyncUDP`, and `AsyncTCP`. Uses a dedicated background FreeRTOS task (`commsTask`) to process retry timers and inbound queues without blocking core loops.

- **B. Raspberry Pi Hub — Python / C++:** Runs a standard Mosquitto broker (supporting native QoS 1 and offline queuing when active), alongside `asyncio` sockets for UDP telemetry and TCP streams.

- **C. Mobile Apps — Kotlin Multiplatform (KMP):** Connects to the broker via multiplatform MQTT libraries for events, utilizes Ktor network sockets for raw TCP streams, and runs shared state machine logic entirely in `commonMain`.


## 4. Repository Folder Structure

Plaintext

```
my-robot-repo/
└── synapse/                   <-- Unified Communication Framework
    ├── protocols/             <-- Message envelope schemas & serialization
    ├── transport-esp32/       <-- C++ implementation (PicoMQTT, AsyncUDP, AsyncTCP wrappers)
    ├── transport-hub/         <-- Python/C++ implementation for Raspberry Pi
    └── transport-kmp/         <-- Kotlin Multiplatform socket/MQTT wrappers
```
