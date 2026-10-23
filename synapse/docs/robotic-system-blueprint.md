# Synapse - Robotic System Blueprint

## 1. Network Topology & Identity Management

- **The Access Point:** The **Core (ESP32-S3)** acts as the central Wi-Fi AP, hosting the local network.
- **Identity Handshake:** Devices (Head, Hub, Mobile App) connect to the AP and immediately broadcast a presence announcement containing their MAC address and functional role (e.g., `role: head_cam`).
- **Dynamic Role Mapping:** The Core builds an in-memory routing table mapping roles to their dynamic IPs, eliminating hardcoded address fragility.

## 2. Communication Protocol Strategy (Pub/Sub via Broker Pattern)

- **Decoupled Senders/Receivers:** Senders broadcast to semantic topics rather than specific hardware IDs.
- **Role-Based Consumption:** Actors subscribe only to topics relevant to their capabilities (e.g., the ESP32-CAM head listens to visual commands; the Hub logs analytical data).
- **Reliability Layer:** PicoMQTT (QoS 0) handles core topic distribution, while Synapse's `EventChannel` layers a 3-retry ACK mechanism on top for true application-level QoS 1 reliability.

## 3. Data Lanes Architecture

|**Lane Type**|**Purpose & Data Examples**|**Transport / Delivery Pattern**|
|---|---|---|
|**Events**|Critical state shifts, pairing codes, triggers (e.g., QR code payload, Emergency Stop).|**Ackable / Idempotent (App-Layer QoS 1 with 3 retries)**. Expects confirmation receipt.|
|**Telemetry**|Continuous sensor feedback broken into speeds:<br><br>  <br><br>• _Slow:_ Battery %, system health.<br><br>  <br><br>• _Fast:_ IMU, ToF, cliff sensors.|**Fire-and-Forget (QoS 0)** via optimized periodic UDP bursts.|
|**Streaming**|High-bandwidth binary data (QVGA camera frames, microphone arrays, live audio/video).|**Dedicated Binary TCP Sockets**, completely off the event bus.|

## 4. The "Outbox Cache" Pattern (Handling the Unpredictable Raspberry Pi Hub)

- **Core Buffering:** The Core (S3) maintains a lightweight circular RAM/Flash buffer acting as an **Outbox Queue**.
- **Offline Stash:** When the Pi is offline, telemetry and analytical logs meant for the Pi accumulate in this local queue.
- **Auto-Flush Discovery:** The Pi broadcasts a heartbeat upon booting. Once acknowledged, the S3 flushes the queued backlog in a burst before resuming real-time streaming.

## 5. Advanced Action & Streaming Negotiation Pattern

For complex actions like playing audio or streaming media, use a **Two-Phase Negotiation** pattern:

1. **The Control Phase:** Send a lightweight JSON event wrapped in the Synapse envelope to a functional topic (`robot/audio/command`) specifying either a static resource ID or a TTS string.
2. **The Stream Negotiation Phase (For Live/Dynamic Audio/Video):**
    
    - The requester sends an event: _"Prepare to receive a stream."_
    - The receiver responds with an acknowledgment and opens a dedicated TCP streaming port.
    - Once confirmed, the sender initiates the heavy binary stream directly on that port, keeping the main event bus clean.
