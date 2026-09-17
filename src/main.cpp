#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_VL53L0X.h"
#include "mpu6500.h"

#include "Config.h"
#include "core/RobotState.h"
#include "drivers/MotorDriver.h"
#include "drivers/WiggleController.h"
#include "drivers/OledDisplay.h"
#include "web/WebServerManager.h"

RobotStateStore stateStore;
MotorDriver motors(PIN_IN1, PIN_IN2, PIN_IN3, PIN_IN4, PIN_FAULT);
WebServerManager webServer(stateStore);
OledDisplay oled;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bfs::Mpu6500 mpu;
uint8_t mpuAddr = 0;
SemaphoreHandle_t i2cMutex = nullptr;

bool mpuProbeAddr(uint8_t addr) {
    mpu.Config(&Wire, addr == MPU_I2C_ADDR_FALLBACK ? bfs::Mpu6500::I2C_ADDR_SEC : bfs::Mpu6500::I2C_ADDR_PRIM);
    if (!mpu.Begin()) {
        return false;
    }
    if (!mpu.ConfigAccelRange(bfs::Mpu6500::ACCEL_RANGE_4G)) {
        return false;
    }
    if (!mpu.ConfigGyroRange(bfs::Mpu6500::GYRO_RANGE_500DPS)) {
        return false;
    }
    if (!mpu.ConfigDlpfBandwidth(bfs::Mpu6500::DLPF_BANDWIDTH_20HZ)) {
        return false;
    }
    mpuAddr = addr;
    return true;
}

bool mpuProbe() {
    return mpuProbeAddr(MPU_I2C_ADDR_PRIMARY) || mpuProbeAddr(MPU_I2C_ADDR_FALLBACK);
}

bool tofReadingValid(const VL53L0X_RangingMeasurementData_t& measure, VL53L0X_Error err) {
    return err == VL53L0X_ERROR_NONE &&
           measure.RangeStatus != 4 &&
           measure.RangeMilliMeter <= TOF_VALID_MAX_MM;
}

bool tofReinit() {
    Wire.end();
    Wire.begin(21, 22);
    Wire.setTimeOut(I2C_TIMEOUT_MS);
    Wire.setClock(400000);
    if (!lox.begin()) {
        Serial.println("[ToF] Re-init FAILED.");
        return false;
    }
    VL53L0X_RangingMeasurementData_t trial;
    if (!tofReadingValid(trial, lox.rangingTest(&trial, false))) {
        Serial.println("[ToF] Re-init OK but trial reading invalid.");
        return false;
    }
    Serial.println("[ToF] Sensor back online.");
    return true;
}

void tofPulseClock() {

    pinMode(22, OUTPUT);
    pinMode(21, INPUT_PULLUP);
    digitalWrite(22, HIGH);
    delayMicroseconds(10);
    for (int i = 0; i < 9; i++) {
        digitalWrite(22, LOW);
        delayMicroseconds(5);
        digitalWrite(22, HIGH);
        delayMicroseconds(5);
    }
}

bool tofRecoverSoft() {
    Serial.println("[ToF] Soft reset (no XSHUT)...");
    tofPulseClock();
    return tofReinit();
}

bool tofRecoverHard() {
    Serial.println("[ToF] Hard reset (XSHUT)...");
    digitalWrite(TOF_XSHUT_PIN, LOW);
    delay(TOF_XSHUT_SHUTDOWN_MS);
    tofPulseClock();
    digitalWrite(TOF_XSHUT_PIN, HIGH);
    delay(TOF_XSHUT_BOOT_MS);
    return tofReinit();
}

void i2cScanBus() {
    Serial.println("[I2C] Scanning bus...");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[I2C] Found device at 0x%02X.\n", addr);
            found++;
        }
    }
    if (found == 0) {
        Serial.println("[I2C] No devices found.");
    }
}

// --- Core 1 Task: Hardware Loop (100Hz) ---
void HardwareTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(10); // 10ms loop time

    int lastGoodDist = 999;
    uint8_t lastGoodStatus = 4;
    int consecErrors = 0;
    int tofTick = 0;
    int mpuTick = 0;
    ImuReading imu;
    unsigned long lastMpuReprobeMs = 0;
    int gentleCount = 0;
    int angryCount = 0;
    WiggleController wiggle(motors);
    bool cliffLatched = false;
    bool wiggleIsCliff = false;

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        bool fault = motors.isFaultActive();

        int recoveryReq = stateStore.takeTofRecoveryRequest();
        if (recoveryReq == 1) {
            consecErrors = 0;
            tofRecoverSoft();
        } else if (recoveryReq == 2) {
            consecErrors = 0;
            tofRecoverHard();
        }

        tofTick++;
        mpuTick++;
        bool doTofRead = (tofTick >= TOF_READ_EVERY_N_TICKS);
        if (doTofRead) {
            tofTick = 0;
            if (i2cMutex != nullptr && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
                // Bus busy (OLED holding ~22ms) — skip this cycle, keep lastGood, don't count as sensor fault
            } else {
                VL53L0X_RangingMeasurementData_t measure;
                VL53L0X_Error err = lox.rangingTest(&measure, false);
                if (i2cMutex) xSemaphoreGive(i2cMutex);
                if (tofReadingValid(measure, err)) {
                    consecErrors = 0;
                    lastGoodStatus = measure.RangeStatus;
                    lastGoodDist = measure.RangeMilliMeter;
                } else {
                    consecErrors++;
                }
            }
        }

        int dist = lastGoodDist;
        bool tofFault = (consecErrors >= TOF_MAX_CONSECUTIVE_ERRORS);

        bool doMpuRead = (mpuTick >= MPU_READ_EVERY_N_TICKS);
        if (doMpuRead) {
            mpuTick = 0;
            ControlState mpuCfg = stateStore.getState();
            if (!mpuCfg.mpuEnabled) {
                imu.healthy = false;
            } else if (mpuAddr == 0) {
                if (millis() - lastMpuReprobeMs >= MPU_REPROBE_INTERVAL_MS) {
                    lastMpuReprobeMs = millis();
                    if (i2cMutex == nullptr || xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                        bool ok = mpuProbe();
                        if (i2cMutex) xSemaphoreGive(i2cMutex);
                        if (ok) Serial.printf("[OK] MPU6050 found at 0x%02X.\n", mpuAddr);
                    }
                }
            } else {
                bool got = (i2cMutex == nullptr) || (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(5)) == pdTRUE);
                if (!got) {
                    // Bus busy (OLED ~22ms) — skip this cycle, keep last imu
                } else {
                    bool readOk = false;
                    readOk = mpu.Read();
                    if (readOk && !imu.healthy) {
                        readOk = mpuProbe() && mpu.Read();
                    }
                    if (i2cMutex) xSemaphoreGive(i2cMutex);
                    if (readOk) {
                        float sx = mpu.accel_x_mps2() / 9.80665f;
                        float sy = mpu.accel_y_mps2() / 9.80665f;
                        float sz = -mpu.accel_z_mps2() / 9.80665f;
                        ControlState imuCfg = stateStore.getState();
                        float fwd, right;
                        switch (imuCfg.imuOrientation & 3) {
                            case 0: fwd = -sx; right = sy; break;
                            case 1: fwd = sy; right = sx; break;
                            case 2: fwd = sx; right = -sy; break;
                            default: fwd = -sy; right = -sx; break;
                        }
                        float up = sz;
                        float rawPitch = atan2(fwd, sqrt(right * right + up * up)) * 180.0f / PI;
                        float rawRoll = atan2(right, up) * 180.0f / PI;
                        if (stateStore.takeImuCalibrateRequest()) {
                            stateStore.setImuOffsets(rawPitch, rawRoll);
                            imuCfg.imuPitchOffset = constrain(rawPitch, -45.0f, 45.0f);
                            imuCfg.imuRollOffset = constrain(rawRoll, -45.0f, 45.0f);
                        }
                        imu.pitch = rawPitch - imuCfg.imuPitchOffset;
                        imu.roll = rawRoll - imuCfg.imuRollOffset;
                        imu.gyroZ = -mpu.gyro_z_radps() * 57.29578f;
                        imu.accelMag = sqrt(fwd * fwd + right * right + up * up);
                        imu.healthy = true;
                    } else {
                        imu.healthy = false;
                    }
                }
            }
            stateStore.updateImu(imu);
            // Pure motion gate + 500ms cooldown after motion: inertial decel not mis-tagged as nudge
            if (imu.healthy) {
                ControlState shakeCfg = stateStore.getState();
                unsigned long now = millis();
                bool robotMoving = (shakeCfg.status == "DRIVING" || shakeCfg.status == "WIGGLE!" || shakeCfg.status == "CLIFF WIGGLE!" || wiggle.isActive());
                bool inMotionCooldown = (now - shakeCfg.lastMotionMs < (unsigned long)MOTION_COOLDOWN_MS);
                if (robotMoving || inMotionCooldown) {
                    gentleCount = 0;
                    angryCount = 0;
                } else {
                    bool angryCooldown = (now - shakeCfg.lastShakenMs < (unsigned long)SHAKE_COOLDOWN_MS);
                    bool gentleCooldown = (now - shakeCfg.lastNudgedMs < (unsigned long)SHAKE_COOLDOWN_MS);
                    float jerk = fabsf(imu.accelMag - 1.0f);
                    bool angryHit = (jerk >= shakeCfg.shakeAngryG - 1.0f) || (fabsf(imu.gyroZ) >= shakeCfg.shakeAngryGyro);
                    bool gentleHit = (jerk >= shakeCfg.shakeGentleG - 1.0f) || (fabsf(imu.gyroZ) >= shakeCfg.shakeGentleGyro) ||
                                     (fabsf(imu.pitch) >= SHAKE_TILT_DEG) || (fabsf(imu.roll) >= SHAKE_TILT_DEG);
                    if (!angryCooldown && angryHit) {
                        angryCount++;
                        gentleCount = 0;
                    } else if (!angryCooldown) {
                        angryCount = 0;
                        if (!gentleCooldown && gentleHit) gentleCount++;
                        else if (!gentleCooldown) gentleCount = 0;
                    } else {
                        // In angry cooldown, still allow gentle counting if not in gentle cooldown
                        if (!gentleCooldown && gentleHit) gentleCount++;
                        else if (!gentleCooldown) gentleCount = 0;
                    }
                    if (!angryCooldown && angryCount >= SHAKE_ANGRY_N) {
                        Serial.printf("[SHAKEN] accel=%.2fg (thr %.2fg) gyro=%.0fdps (thr %.0fdps) counts a=%d/%d pitch=%.1f roll=%.1f\n",
                                      imu.accelMag, shakeCfg.shakeAngryG, imu.gyroZ, shakeCfg.shakeAngryGyro, angryCount, SHAKE_ANGRY_N, imu.pitch, imu.roll);
                        stateStore.announceInput(INPUT_SHAKEN, imu.accelMag, imu.gyroZ, imu.pitch, imu.roll);
                        gentleCount = 0;
                        angryCount = 0;
                    } else if (!gentleCooldown && gentleCount >= SHAKE_GENTLE_N) {
                        Serial.printf("[NUDGED] accel=%.2fg (thr %.2fg) gyro=%.0fdps (thr %.0fdps) counts g=%d/%d pitch=%.1f roll=%.1f\n",
                                      imu.accelMag, shakeCfg.shakeGentleG, imu.gyroZ, shakeCfg.shakeGentleGyro, gentleCount, SHAKE_GENTLE_N, imu.pitch, imu.roll);
                        stateStore.announceInput(INPUT_NUDGED, imu.accelMag, imu.gyroZ, imu.pitch, imu.roll);
                        gentleCount = 0;
                        angryCount = 0;
                    }
                    if (angryCooldown && angryCount>0) angryCount=0;
                    if (gentleCooldown && gentleCount>0) gentleCount=0;
                }
            }
        }

        ControlState state = stateStore.getState();

        bool isCliff = (state.cliffThresholdMM < 500) && 
                       ((lastGoodStatus == 4) || (dist > state.cliffThresholdMM));

        int left = state.targetLeftSpeed;
        int right = state.targetRightSpeed;
        String status = "STOPPED";

        // Manager-level cliff hold: after one cliff wiggle, stay braked while
        // the user keeps holding forward, even if the sensor loses the cliff
        // mid-manoeuvre. Re-arms only when the user releases to stop/reverse.
        if (left <= 0 && right <= 0) {
            cliffLatched = false;
        }
        int wiggleReqDir, wiggleReqPairs;
        if (stateStore.takeWiggleRequest(wiggleReqDir, wiggleReqPairs) && !wiggle.isActive()) {
            WiggleDirection reqDir = (wiggleReqDir == 0) ? WiggleDirection::FORWARD
                : (wiggleReqDir == 1) ? WiggleDirection::BACKWARD : WiggleDirection::IN_PLACE;
            wiggle.start(reqDir, wiggleReqPairs);
            wiggleIsCliff = false;
            stateStore.setDisplayHappy(millis() + HAPPY_MOOD_MS);
        }
        if (!cliffLatched && !wiggle.isActive() && isCliff && (left > 0 || right > 0) &&
            !state.isEBrake && !fault && !tofFault) {
            wiggle.start(WiggleDirection::BACKWARD, CLIFF_WIGGLE_PAIRS);
            wiggleIsCliff = true;
            stateStore.setDisplayWorried(millis() + OLED_WORRIED_DURATION_MS);
        }

        if (state.isEBrake) {
            status = "E-BRAKE LOCKED";
            wiggle.abort();
            wiggleIsCliff = false;
            motors.applyBrake();
        } else if (fault) {
            status = "DRIVER OVERLOAD!";
            wiggle.abort();
            wiggleIsCliff = false;
            motors.drive(0, 0);
        } else if (tofFault) {
            status = "TOF SENSOR FAULT";
            wiggle.abort();
            wiggleIsCliff = false;
            if (left > 0 || right > 0) {
                motors.applyBrake();
            } else {
                motors.drive(left, right);
            }
        } else if (wiggle.isActive()) {
            if (wiggle.update()) {
                if (wiggleIsCliff) {
                    status = "BLOCKED (CLIFF)";
                    cliffLatched = true;
                    motors.applyBrake();
                } else {
                    // Simple button wiggle: hand straight back to the stick.
                    status = (left == 0 && right == 0) ? "STOPPED" : "DRIVING";
                    motors.drive(left, right);
                }
                wiggleIsCliff = false;
            } else {
                status = wiggleIsCliff ? "CLIFF WIGGLE!" : "WIGGLE!";
            }
        } else if (millis() - state.lastCommandTime > COMMAND_TIMEOUT_MS && (left != 0 || right != 0)) {
            status = "TIMEOUT STOP";
            stateStore.updateDriveCommand(0, 0);
            motors.drive(0, 0);
        } else if ((isCliff || cliffLatched) && (left > 0 || right > 0)) {
            status = "BLOCKED (CLIFF)";
            motors.applyBrake();
        } else {
            status = (left == 0 && right == 0) ? "STOPPED" : "DRIVING";
            motors.drive(left, right);
        }

        stateStore.updateTelemetry(dist, isCliff, fault, tofFault, status);
        stateStore.updateTelemetryMotion(status);
    }
}

void DisplayTask(void *pvParameters) {
    TickType_t xLast = xTaskGetTickCount();
    const TickType_t xFreq = pdMS_TO_TICKS(OLED_FPS_MS);
    for (;;) {
        vTaskDelayUntil(&xLast, xFreq);
        int anim = 0;
        if (stateStore.takeDisplayAnim(anim)) {
            oled.triggerAnim(anim);
        }
        ControlState s = stateStore.getState();
        oled.render(s);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- ESP32 BOOTING UP ---");

    stateStore.begin();
    motors.begin();

    i2cMutex = xSemaphoreCreateMutex();
    oled.setI2cMutex(i2cMutex);

    pinMode(TOF_XSHUT_PIN, OUTPUT);
    digitalWrite(TOF_XSHUT_PIN, HIGH);

    // Reset and initialize I2C bus - 400kHz shared (ToF/MPU/OLED + future camera)
    Wire.end();
    Wire.begin(21, 22); 
    Wire.setTimeOut(I2C_TIMEOUT_MS);
    Wire.setClock(400000); // 400kHz fast I2C
    i2cScanBus();

    // 2. Initialize distance sensor
    if (!lox.begin()) {
        Serial.println("[ERROR] Failed to find VL53L0X sensor! Check wiring.");
    } else {
        Serial.println("[OK] VL53L0X sensor initialized.");
    }

    // 3. Initialize IMU (non-fatal: robot runs fine without it)
    ControlState mpuCheck = stateStore.getState();
    if (!mpuCheck.mpuEnabled) {
        Serial.println("[MPU] Disabled via settings.");
    } else {
        bool mpuOk = false;
        for (int attempt = 0; attempt < 3 && !mpuOk; attempt++) {
            if (attempt > 0) {
                delay(100);
            }
            mpuOk = mpuProbe();
        }
        if (mpuOk) {
            Serial.printf("[OK] MPU6050 initialized at 0x%02X.\n", mpuAddr);
        } else {
            Serial.println("[WARN] MPU6050 not found, IMU disabled.");
            uint8_t who = 0xFF;
            Wire.beginTransmission(MPU_I2C_ADDR_PRIMARY);
            Wire.write(0x75);
            if (Wire.endTransmission(false) == 0 && Wire.requestFrom(MPU_I2C_ADDR_PRIMARY, (uint8_t)1) == 1) {
                who = Wire.read();
            }
            Serial.printf("[MPU] WHO_AM_I (0x75) reads 0x%02X (expect 0x68).\n", who);
        }
    }

    // 4. Initialize OLED
    if (oled.begin()) {
        oled.hello();
    }

    // WiFi must connect before display shows IP; keep hello then start renderer
    webServer.begin();

    // Start tasks - DisplayTask prio 1 on Core0 to avoid WebServer starvation (Layer2)
    xTaskCreatePinnedToCore(HardwareTask, "HardwareTask", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(DisplayTask, "DisplayTask", 4096, NULL, 1, NULL, 0);
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
