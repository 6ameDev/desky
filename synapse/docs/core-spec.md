# Synapse: Unified Transport Abstraction Layer

**Synapse** is a Distributed Middleware / Inter-Process Communication (IPC) Abstraction Layer designed to unify your multi-brain robotic system (ESP32 Core, ESP32 Head, Raspberry Pi Hub, and KMP Mobile App).

## The Vision: Architecture Levels

- **Level 1: Raw Networking**
    
    - _What it is:_ Sockets, IP addresses, TCP handshakes, UDP packet drops, WiFi AP connections, and byte arrays.
        
    - _Why it's hidden:_ Eliminates the 90% of development time typically spent debugging low-level network drops and connection states.
        
- **Level 2: Transport Protocols**
    
    - _What it is:_ PicoMQTT brokers, topic routing, JSON serialization, and stream ports handling how data moves across the air.
        
- **Level 3: Synapse Middleware (The Abstraction Layer)**
    
    - _What it is:_ A domain-specific orchestration layer that hides Level 1 and Level 2 entirely from your robot's brain.
        
    - Your robot's behavior code simply calls unified APIs (e.g., _“Tell the head to display this QR code”_ or _“Stream IMU telemetry”_) without managing underlying transports.
        
- **Level 4: The Robot Domain Layer (The Ultimate Consumer)**
    
    - The actual behavior logic: obstacle avoidance, facial tracking, speech synthesis, and mobile app UI rendering.
        

## Summary of What Synapse Provides

1. **Transport Agnosticism:** Automatically routes fast sensor data over UDP, critical commands over MQTT with application-level retries/ACKs, and heavy bytes over raw TCP sockets.
    
2. **State & Lifecycle Management:** Handles peer discovery, role mapping (Core, Head, Hub), and outbox queuing (caching messages when the Pi hub is offline).
    
3. **Cross-Language Uniformity:** Provides a consistent API design whether writing C++ on an ESP32, Python/C++ on a Raspberry Pi, or Kotlin in a mobile app.
    

## Message Envelope Specification (v1.0)

Every structured message (Events and Commands) passing through the Synapse bus is wrapped in this standard JSON envelope.

### 1. The Core Envelope Fields

|**Field Name**|**Data Type**|**Description**|
|---|---|---|
|`v`|Integer|Protocol version (currently `1`). Essential for future backward compatibility.|
|`id`|String|Unique Message ID (e.g., timestamp-hash like `msg_171892`). Crucial for app-layer QoS 1 ACKs and deduplication.|
|`src`|String|Sender Node ID / Role (e.g., `core_s3`, `head_cam`, `hub_pi`, `app_ios`). Tells the consumer who sent it.|
|`dst`|String|Target Node ID or Broadcast wildcard (`*`).|
|`type`|String|Category of message: `event`, `command`, `telemetry`, or `ack`.|
|`action`|String|Specific domain instruction (e.g., `display_qr`, `play_tts`, `sys_status`).|
|`ts`|Long|Epoch timestamp in milliseconds (for ordering and latency/timeout checks).|
|`payload`|Object / String|The actual data payload required by the action.|

### 2. Concrete JSON Examples

#### Example A: A Critical Event/Command (Requires App-Layer QoS 1 ACK)

_Scenario: The Core (S3) telling the Head (CAM) to display the pairing QR code._

JSON

```
{
  "v": 1,
  "id": "msg_984123",
  "src": "core_s3",
  "dst": "head_cam",
  "type": "command",
  "action": "display_qr",
  "ts": 1782345678910,
  "payload": {
    "pin": "482910",
    "format": "qr"
  }
}
```

#### Example B: The Acknowledgment (The ACK Response)

_Scenario: The Head confirming it received and processed `msg_984123`._

JSON

```
{
  "v": 1,
  "id": "ack_558912",
  "src": "head_cam",
  "dst": "core_s3",
  "type": "ack",
  "action": "ack_response",
  "ts": 1782345679050,
  "payload": {
    "original_id": "msg_984123",
    "status": "success"
  }
}
```

### 3. How the Envelope Adapts Across the 3 Data Lanes

- **The Event Lane (MQTT over TCP + App QoS 1):** Uses the Full JSON Envelope defined above. Handled by `EventChannel`, which tracks IDs, fires automatic 3-retries on missing ACKs, and routes actions to registered listeners.
    
- **The Telemetry Lane (UDP):** Bypasses heavy JSON parsing to maximize throughput for 50Hz sensor streams. Uses optimized binary structs or stripped-down mini-envelopes (`src`, `ts`, sensor arrays).
    
- **The Stream Lane (Raw TCP Sockets):** Uses a tiny header indicating stream type and length, followed immediately by raw binary bytes (JPEG frames or PCM audio chunks) for zero-latency streaming.
