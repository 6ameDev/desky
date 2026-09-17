#pragma once
#include <Arduino.h>
#include "Config.h"

struct ImuReading {
    float pitch = 0;
    float roll = 0;
    float gyroZ = 0;
    float accelMag = 0;
    bool isPickedUp = false;
    bool healthy = false;
};

struct ControlState {
    int targetLeftSpeed = 0;
    int targetRightSpeed = 0;
    int currentDistanceMM = 0;
    int cliffThresholdMM = DEFAULT_CLIFF_LIMIT_MM;
    int maxPowerPercent = DEFAULT_MAX_POWER_PERCENT;
    bool isCliff = false;
    bool isFault = false;
    bool isEBrake = false;
    bool tofFault = false;
    int tofRecoveryRequest = 0;
    // Manual wiggle request from the UI. Direction codes: 0=forward,
    // 1=backward, 2=in-place. dir < 0 means no pending request.
    int wiggleRequestDir = -1;
    int wiggleRequestPairs = 0;
    // Display state (OLED eyes / debug). Manager sets, DisplayTask reads.
    bool displayDebugOn = false;
    uint32_t displayWorriedUntilMs = 0;
    int displayMoodOverride = -1; // -1=auto, 0=DEFAULT,1=TIRED,2=ANGRY,3=HAPPY
    int displayAnimRequest = 0; // 0=none, 1=blink,2=confused,3=laugh
    bool mpuEnabled = (bool)MPU_ENABLED_DEFAULT;
    int imuOrientation = 0;
    bool imuCalibrateRequested = false;
    float imuPitchOffset = 0;
    float imuRollOffset = 0;
    ImuReading imu;
    String status = "STOPPED";
    unsigned long lastCommandTime = 0;
};

class RobotStateStore {
public:
    RobotStateStore();
    void begin();
    ControlState getState();
    
    void updateDriveCommand(int8_t x, int8_t y);
    void toggleEBrake();
    void setCliffThreshold(int thresholdMM);
    void setMaxPower(int percent);
    void requestWiggle(int dir, int pairs);
    bool takeWiggleRequest(int& dir, int& pairs);
    void setDisplayWorried(uint32_t untilMs);
    void toggleDisplayDebug();
    void setDisplayMood(int mood);
    void requestDisplayAnim(int anim);
    bool takeDisplayAnim(int &anim);
    void setMpuEnabled(bool enabled);
    void toggleMpuEnabled();
    void requestTofRecovery(int mode);
    int takeTofRecoveryRequest();
    void cycleImuOrientation();
    void requestImuCalibrate();
    bool takeImuCalibrateRequest();
    void setImuOffsets(float pitchOffset, float rollOffset);
    void updateImu(const ImuReading& reading);
    void updateTelemetry(int distanceMM, bool isCliff, bool isFault, bool tofFault, const String& status);

private:
    ControlState _state;
    SemaphoreHandle_t _mutex;
};
