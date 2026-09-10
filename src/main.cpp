#include <Arduino.h>
#include <Wire.h>
#include "Adafruit_VL53L0X.h"
#include "mpu6500.h"

#include "Config.h"
#include "core/RobotState.h"
#include "drivers/MotorDriver.h"
#include "web/WebServerManager.h"

RobotStateStore stateStore;
MotorDriver motors(PIN_IN1, PIN_IN2, PIN_IN3, PIN_IN4, PIN_FAULT);
WebServerManager webServer(stateStore);
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bfs::Mpu6500 mpu;
uint8_t mpuAddr = 0;

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
    Wire.setClock(100000);
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
    int tick = 0;
    ImuReading imu;
    unsigned long lastMpuReprobeMs = 0;

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

        tick++;
        if (tick >= TOF_READ_EVERY_N_TICKS) {
            tick = 0;
            VL53L0X_RangingMeasurementData_t measure;
            VL53L0X_Error err = lox.rangingTest(&measure, false);
            if (tofReadingValid(measure, err)) {
                consecErrors = 0;
                lastGoodStatus = measure.RangeStatus;
                lastGoodDist = measure.RangeMilliMeter;
            } else {
                consecErrors++;
            }
        }

        int dist = lastGoodDist;
        bool tofFault = (consecErrors >= TOF_MAX_CONSECUTIVE_ERRORS);

        if (tick == MPU_READ_TICK_OFFSET) {
            if (mpuAddr == 0) {
                if (millis() - lastMpuReprobeMs >= MPU_REPROBE_INTERVAL_MS) {
                    lastMpuReprobeMs = millis();
                    if (mpuProbe()) {
                        Serial.printf("[OK] MPU6050 found at 0x%02X.\n", mpuAddr);
                    }
                }
            } else {
                bool readOk = mpu.Read();
                if (readOk && !imu.healthy) {
                    readOk = mpuProbe() && mpu.Read();
                }
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
                    imu.isPickedUp = (imu.accelMag > PICKED_UP_ACCEL_G) ||
                                     (fabsf(imu.pitch) > PICKED_UP_TILT_DEG) ||
                                     (fabsf(imu.roll) > PICKED_UP_TILT_DEG);
                    imu.healthy = true;
                } else {
                    imu.healthy = false;
                }
            }
            stateStore.updateImu(imu);
        }

        ControlState state = stateStore.getState();

        bool isCliff = (state.cliffThresholdMM < 500) && 
                       ((lastGoodStatus == 4) || (dist > state.cliffThresholdMM));

        int left = state.targetLeftSpeed;
        int right = state.targetRightSpeed;
        String status = "STOPPED";

        if (state.isEBrake) {
            status = "E-BRAKE LOCKED";
            motors.applyBrake();
        } else if (fault) {
            status = "DRIVER OVERLOAD!";
            motors.drive(0, 0);
        } else if (tofFault) {
            status = "TOF SENSOR FAULT";
            if (left > 0 || right > 0) {
                motors.applyBrake();
            } else {
                motors.drive(left, right);
            }
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

        stateStore.updateTelemetry(dist, isCliff, fault, tofFault, status);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- ESP32 BOOTING UP ---");

    stateStore.begin();
    motors.begin();

    pinMode(TOF_XSHUT_PIN, OUTPUT);
    digitalWrite(TOF_XSHUT_PIN, HIGH);

    // Reset and initialize I2C bus
    Wire.end();
    Wire.begin(21, 22); 
    Wire.setTimeOut(I2C_TIMEOUT_MS);
    Wire.setClock(100000); // 100kHz standard I2C speed
    i2cScanBus();

    // 2. Initialize distance sensor
    if (!lox.begin()) {
        Serial.println("[ERROR] Failed to find VL53L0X sensor! Check wiring.");
    } else {
        Serial.println("[OK] VL53L0X sensor initialized.");
    }

    // 3. Initialize IMU (non-fatal: robot runs fine without it)
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
