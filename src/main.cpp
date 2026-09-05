#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_VL53L0X.h"

#include "Config.h"
#include "core/RobotState.h"
#include "drivers/MotorDriver.h"
#include "web/WebServerManager.h"

RobotStateStore stateStore;
MotorDriver motors(PIN_IN1, PIN_IN2, PIN_IN3, PIN_IN4, PIN_FAULT);
WebServerManager webServer(stateStore);
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// --- Core 1 Task: Hardware Loop (100Hz) ---
void HardwareTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(10); // 10ms loop time

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        bool fault = motors.isFaultActive();

        VL53L0X_RangingMeasurementData_t measure;
        lox.rangingTest(&measure, false);
        int dist = (measure.RangeStatus != 4) ? measure.RangeMilliMeter : 999;

        ControlState state = stateStore.getState();

        bool isCliff = (state.cliffThresholdMM < 500) && 
                       ((measure.RangeStatus == 4) || (dist > state.cliffThresholdMM));

        int left = state.targetLeftSpeed;
        int right = state.targetRightSpeed;
        String status = "STOPPED";

        if (state.isEBrake) {
            status = "E-BRAKE LOCKED";
            motors.applyBrake();
        } else if (fault) {
            status = "DRIVER OVERLOAD!";
            motors.drive(0, 0);
        } else if (millis() - state.lastCommandTime > COMMAND_TIMEOUT_MS && (left != 0 || right != 0)) {
            status = "TIMEOUT STOP";
            stateStore.updateDriveCommand(0, 0);
            motors.drive(0, 0);
        } else if (isCliff && (left > 0 || right > 0)) {
            status = "BLOCKED (CLIFF)";
            motors.applyBrake();
        } else {
            status = (left == 0 && right == 0) ? "STOPPED" : "DRIVING";
            motors.drive(left, right);
        }

        stateStore.updateTelemetry(dist, isCliff, fault, status);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- ESP32 BOOTING UP ---");

    stateStore.begin();
    motors.begin();

    // Reset and initialize I2C bus
    Wire.end();
    Wire.begin(21, 22); 
    Wire.setClock(100000); // 100kHz standard I2C speed

    // 2. Initialize distance sensor
    if (!lox.begin()) {
        Serial.println("[ERROR] Failed to find VL53L0X sensor! Check wiring.");
    } else {
        Serial.println("[OK] VL53L0X sensor initialized.");
    }

    webServer.begin();

    // Start Hardware Task on Core 1
    xTaskCreatePinnedToCore(HardwareTask, "HardwareTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
    // Runs on Core 0 (Network Upkeep)
    webServer.cleanupClients();

    static unsigned long lastPoll = 0;
    if (millis() - lastPoll > 50) {
        lastPoll = millis();
        webServer.pushTelemetryIfNeeded();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}
