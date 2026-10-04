This backlog outlines architectural refactoring tasks for the `desky` project. It categorizes custom components into two groups:

1. **Modules to Refactor / Replace:** Components where standard, battle-tested open-source frameworks should replace custom implementations to improve maintainability, reduce bug surface, and standardize protocols.
    
2. **Modules to KEEP (Do NOT Replace):** Hardware-bound, timing-critical, or safety-specific custom implementations that must remain intact to preserve high-throughput 1.5M UART performance and prevent CPU starvation or bootloader bricks.
    

## Task List & Implementation Roadmap

### [ ] Task 1: Replace Custom ASCII Command Parsing with Nanopb (Protobuf)

- **Priority:** High
    
- **Target Scope:** Command / Config interface (`poc_config.h`, CLI string parsing, `HEAD SET...` checks)
    
- **Goal:** Replace manual C-string matching and manual binary packing with schema-driven, zero-allocation Protocol Buffers.
    
- **Why:** String parsing in C++ is prone to truncation bugs and overflow errors. Nanopb provides type safety and simple struct generation with strict payload constraints.
    
- **Action Items:**
    
    1. Create `schema/commands.proto` defining command/telemetry messages (`SetMode`, `SetConfig`, `GetStatus`).
        
    2. Integrate `nanopb` into `platformio.ini` (`lib_deps = nanopb/Nanopb`).
        
    3. Replace ASCII string checks in Head/S3 command handlers with `pb_decode()` and `pb_encode()` calls.
        

### [ ] Task 2: Standardize Internal Ring Buffers with ESP-IDF `ringbuf.h`

- **Priority:** Medium
    
- **Target Scope:** Generic queue buffers and custom array pointers in `link_manager.h`
    
- **Goal:** Replace custom pointer-arithmetic ring buffers with FreeRTOS-native ESP-IDF ring buffers.
    
- **Why:** Custom concurrency/ring handling risks subtle race conditions between CPU cores. `freertos/ringbuf.h` handles inter-core memory sync and ISR safety out of the box.
    
- **Action Items:**
    
    1. Identify internal scratch buffers that do not directly touch raw DMA memory.
        
    2. Refactor to use `xRingbufferCreate()` or `xRingbufferCreateStatic()` with type `RINGBUF_TYPE_BYTEBUF` or `RINGBUF_TYPE_NOSPLIT`.
        
    3. Standardize thread-safe pushes and pops using `xRingbufferSend()` and `xRingbufferReceive()`.
        

### [ ] Task 3: Implement Wi-Fi OTA Maintenance Mode

- **Priority:** Medium
    
- **Target Scope:** `head/src/services/` (ESP32-CAM Head Firmware)
    
- **Goal:** Enable Over-The-Air wireless updates so the Head module never has to be unmounted or connected via USB to flash new code.
    
- **Why:** Eliminates physical tear-down cycles during development. Wi-Fi overhead is safely isolated from core streaming performance by running in a dedicated maintenance mode.
    
- **Action Items:**
    
    1. Integrate `ArduinoOTA.h` into the Head firmware.
        
    2. Implement an OTA trigger mode (`HEAD SET MODE=OTA`).
        
    3. Pause camera streaming and DMA activity during OTA mode to dedicate system bandwidth entirely to flashing.
        
    4. Configure PlatformIO upload targets for `upload_protocol = espota`.
        

## Protected Architecture (DO NOT TOUCH / KEEP CUSTOM)

The following custom implementations are **strictly protected**. Do **NOT** replace these with generic libraries, as doing so will break DMA synchronization, starve FreeRTOS background tasks, or brick boot pin states.

```
                     ┌─────────────────────────────────────────────────┐
                     │             CRITICAL CUSTOM LOGIC               │
                     │                 (DO NOT TOUCH)                  │
                     └─────────────────────────────────────────────────┘
                                       │
     ┌─────────────────────────────────┼─────────────────────────────────┐
     ▼                                 ▼                                 ▼
[ Stage-and-Release ]        [ RX Bounded Drain ]           [ RX-Only Startup Guard ]
`grabNext_()` +             `pollLink()` 64-pass            GPIO12 boot pin protection
`pumpStaged_()`             ring drain                      against hardware bricking
```

### 1. Head Stage-and-Release Pipeline (`grabNext_()` + `pumpStaged_()`)

- **Files:** `head/src/link_manager.h`, `head/src/camera_driver.cpp`
    
- **Why it must remain custom:** Standard framework drivers keep the camera frame pointer (`camera_fb_t`) locked while serial transmission occurs. This starves the camera DMA engine. Our custom pipeline immediately clones frame bytes into internal scratch memory and releases the frame buffer back to hardware (`esp_camera_fb_return()`), keeping double-buffered capture running smoothly without dropping frames.
    

### 2. S3 Bounded RX Drain Loop (`pollLink()`)

- **Files:** `s3/src/link_manager.h`, `s3/src/uart_driver.cpp`
    
- **Why it must remain custom:** Standard `Serial.read()` or unconstrained IDF UART loops read continuously until the hardware FIFO is empty. At 1.5M baud, an unconstrained read loop starves Core 0/1 background tasks (like USB CDC logging and motor loops), triggering FreeRTOS task watchdogs (`IDLE task watchdog reset`). The bounded 64-pass / 256-byte drain loop enforces CPU fairness.
    

### 3. RX-Only Boot Guard (GPIO12 Strapping Safety)

- **Files:** `s3/src/hardware_init.cpp`
    
- **Why it must remain custom:** Standard UART initializations attach both TX and RX pins simultaneously at power-on. On the S3 pin setup, driving TX before the Head finishes booting can pull GPIO12 into an invalid state and brick the bootloader sequence. The custom startup guard keeps TX disabled (`TX = -1`) until valid incoming frame CRCs pass verification.