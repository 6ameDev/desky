#pragma once
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "Config.h"
#include "core/RobotState.h"
#include "eyes/EyeConfig.h"

// Calm squircle eyes - vendored RoboEyes tween (FluxGarage, GPL-3.0) adapted
// for Desky. Smooth exponential transitions, idle drift, auto-blink.
// Manager (RobotState) only sets worried timer + debug flag; renderer owns
// geometry and blink/idle.
class OledDisplay {
public:
    OledDisplay();
    bool begin();
    void render(const ControlState& state);
    void hello();
    bool healthy() const;
    void setI2cMutex(SemaphoreHandle_t m) { _i2cMutex = m; }
    void triggerAnim(int anim);

private:
    Adafruit_SSD1306 _display;
    bool _healthy = false;
    uint8_t _addr = 0;
    SemaphoreHandle_t _i2cMutex = nullptr;
    bool probeAddr(uint8_t addr);
    void drawDebug(const ControlState& state);
    void drawEyes(const ControlState& state);
    void stepEyeState();
    void drawEyeFrames();
    void applyPreset(const EyeConfig& cfg);
    void resetBlinkTimer();

    // ---- RoboEyes geometry (vendored, GPL) ----
    int screenWidth = OLED_WIDTH;
    int screenHeight = OLED_HEIGHT;
    int frameInterval = OLED_FPS_MS;
    unsigned long fpsTimer = 0;

    bool tired = 0, angry = 0, happy = 0;
    bool eyeL_open = 1, eyeR_open = 1;

    int eyeLwidthDefault = EYE_W, eyeLheightDefault = EYE_H;
    int eyeLwidthCurrent = EYE_W, eyeLheightCurrent = EYE_H;
    int eyeLwidthNext = EYE_W, eyeLheightNext = EYE_H;
    int eyeLheightOffset = 0;
    byte eyeLborderRadiusDefault = EYE_RADIUS;
    byte eyeLborderRadiusCurrent = EYE_RADIUS;
    byte eyeLborderRadiusNext = EYE_RADIUS;

    int eyeRwidthDefault = EYE_W, eyeRheightDefault = EYE_H;
    int eyeRwidthCurrent = EYE_W, eyeRheightCurrent = EYE_H;
    int eyeRwidthNext = EYE_W, eyeRheightNext = EYE_H;
    int eyeRheightOffset = 0;
    byte eyeRborderRadiusDefault = EYE_RADIUS;
    byte eyeRborderRadiusCurrent = EYE_RADIUS;
    byte eyeRborderRadiusNext = EYE_RADIUS;

    int eyeLxDefault = ((OLED_WIDTH)-(EYE_W+10+EYE_W))/2;
    int eyeLyDefault = ((OLED_HEIGHT-EYE_H)/2);
    int eyeLx = eyeLxDefault, eyeLy = eyeLyDefault;
    int eyeLxNext = eyeLxDefault, eyeLyNext = eyeLyDefault;
    int eyeRxDefault = 0, eyeRyDefault = 0;
    int eyeRx = 0, eyeRy = 0;
    int eyeRxNext = 0, eyeRyNext = 0;

    byte eyelidsHeightMax = EYE_H/2;
    byte eyelidsTiredHeight = 0, eyelidsTiredHeightNext = 0;
    byte eyelidsAngryHeight = 0, eyelidsAngryHeightNext = 0;
    byte eyelidsHappyBottomOffsetMax = (EYE_H/2)+3;
    byte eyelidsHappyBottomOffset = 0, eyelidsHappyBottomOffsetNext = 0;
    int spaceBetweenDefault = EYE_GAP;
    int spaceBetweenCurrent = EYE_GAP, spaceBetweenNext = EYE_GAP;

    bool autoblinker = 1;
    int blinkInterval = BLINK_INTERVAL_SEC;
    int blinkIntervalVariation = BLINK_VARIATION_SEC;
    unsigned long blinktimer = 0;

    bool idle = 1;
    int idleInterval = IDLE_INTERVAL_SEC;
    int idleIntervalVariation = IDLE_VARIATION_SEC;
    unsigned long idleAnimationTimer = 0;

    bool hFlicker = 0, hFlickerAlternate = 0;
    byte hFlickerAmplitude = 8;
    bool vFlicker = 0, vFlickerAlternate = 0;
    byte vFlickerAmplitude = 4;

    unsigned long happyUntilMs = 0;
    unsigned long lastActiveMs = 0;
    unsigned long confusedUntilMs = 0, laughUntilMs = 0;
    int8_t lastMoodOverride = -2;
    int eyeLheightTarget = EYE_H, eyeRheightTarget = EYE_H;
    bool wasSleepy = false;

    int getScreenConstraint_X();
    int getScreenConstraint_Y();
    void close();
    void open();
    bool isTransitioning() const;
};
