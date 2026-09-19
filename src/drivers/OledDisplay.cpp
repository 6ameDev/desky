// Vendored RoboEyes core (FluxGarage, GPL-3.0) adapted for Desky squircle eyes
// Smooth exponential tweening, idle drift, auto-blink, moods.
// Original: https://github.com/FluxGarage/RoboEyes
// Eye presets from playfultechnology/esp32-eyes (GPL-3.0) - see eyes/EyePresets.h
#include "OledDisplay.h"
#include "eyes/EyeConfig.h"
#include "eyes/EyePresets.h"
#include <WiFi.h>

OledDisplay::OledDisplay()
    : _display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1) {
    eyeRxDefault = eyeLxDefault + eyeLwidthCurrent + spaceBetweenDefault;
    eyeRyDefault = eyeLyDefault;
    eyeRx = eyeRxDefault; eyeRy = eyeRyDefault;
    eyeRxNext = eyeRx; eyeRyNext = eyeRy;
    frameInterval = OLED_FPS_MS;
}

bool OledDisplay::probeAddr(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool OledDisplay::begin() {
#if !OLED_ENABLED
    Serial.println("[OLED] Disabled via OLED_ENABLED.");
    return false;
#endif
    uint8_t addr = 0;
    if (probeAddr(OLED_I2C_ADDR_PRIMARY)) {
        addr = OLED_I2C_ADDR_PRIMARY;
        Serial.printf("[OLED] Probe 0x%02X: found.\n", addr);
    } else {
        Serial.printf("[OLED] Probe 0x%02X: miss.\n", OLED_I2C_ADDR_PRIMARY);
        if (probeAddr(OLED_I2C_ADDR_ALT)) {
            addr = OLED_I2C_ADDR_ALT;
            Serial.printf("[OLED] Probe 0x%02X: found.\n", addr);
        } else {
            Serial.printf("[OLED] Probe 0x%02X: miss.\n", OLED_I2C_ADDR_ALT);
        }
    }
    if (addr == 0) {
        Serial.println("[OLED] No display found at 0x3C or 0x3D.");
        _healthy = false;
        return false;
    }
    if (!_display.begin(SSD1306_SWITCHCAPVCC, addr)) {
        Serial.printf("[OLED] SSD1306 begin failed at 0x%02X.\n", addr);
        _healthy = false;
        return false;
    }
    _addr = addr;
    _healthy = true;
    _display.clearDisplay();
    _display.display();
    eyeLheightCurrent = 1; eyeRheightCurrent = 1;
    blinktimer = millis() + (blinkInterval + random(blinkIntervalVariation + 1)) * 1000;
    idleAnimationTimer = millis() + (idleInterval + random(idleIntervalVariation + 1)) * 1000;
    lastActiveMs = millis();
    Serial.printf("[OLED] SSD1306 at 0x%02X ready (%dx%d).\n", addr, OLED_WIDTH, OLED_HEIGHT);
    return true;
}

int OledDisplay::getScreenConstraint_X() { return screenWidth - eyeLwidthCurrent - spaceBetweenCurrent - eyeRwidthCurrent; }
int OledDisplay::getScreenConstraint_Y() { return screenHeight - eyeLheightDefault; }
void OledDisplay::close() { eyeLheightNext = 1; eyeRheightNext = 1; eyeL_open = 0; eyeR_open = 0; }
void OledDisplay::open() { eyeL_open = 1; eyeR_open = 1; }
bool OledDisplay::isTransitioning() const {
    return abs(eyeLwidthCurrent - eyeLwidthNext) > 1 || abs(eyeRwidthCurrent - eyeRwidthNext) > 1 ||
           abs(eyeLheightCurrent - eyeLheightNext) > 1 || abs(eyeRheightCurrent - eyeRheightNext) > 1;
}

void OledDisplay::applyPreset(const EyeConfig& cfg) {
    // Map EyeConfig to RoboEyes targets — also update height target for blink restore
    eyeLwidthNext = cfg.Width; eyeRwidthNext = cfg.Width;
    eyeLheightNext = cfg.Height; eyeRheightNext = cfg.Height;
    eyeLheightTarget = cfg.Height; eyeRheightTarget = cfg.Height;
    eyeLborderRadiusNext = constrain(cfg.Radius_Top, 0, 16);
    eyeRborderRadiusNext = constrain(cfg.Radius_Top, 0, 16);
    if (cfg.OffsetX != 0 || cfg.OffsetY != 0) {
        eyeLxNext = constrain(eyeLxDefault + cfg.OffsetX, 0, getScreenConstraint_X());
        eyeLyNext = constrain(eyeLyDefault + cfg.OffsetY, 0, getScreenConstraint_Y());
    }
    resetBlinkTimer();
}

void OledDisplay::resetBlinkTimer() {
    blinktimer = millis() + (blinkInterval * 1000) + (random(blinkIntervalVariation + 1) * 1000);
}

void OledDisplay::stepEyeState() {
    eyeLheightCurrent = (eyeLheightCurrent * 2 + eyeLheightNext) / 3;
    eyeLy += ((eyeLheightDefault - eyeLheightCurrent) / 2);
    eyeRheightCurrent = (eyeRheightCurrent * 2 + eyeRheightNext) / 3;
    eyeRy += (eyeRheightDefault - eyeRheightCurrent) / 2;

    if (eyeL_open && eyeLheightCurrent <= 2) eyeLheightNext = eyeLheightTarget;
    if (eyeR_open && eyeRheightCurrent <= 2) eyeRheightNext = eyeRheightTarget;

    eyeLwidthCurrent = (eyeLwidthCurrent * 2 + eyeLwidthNext) / 3;
    eyeRwidthCurrent = (eyeRwidthCurrent * 2 + eyeRwidthNext) / 3;
    spaceBetweenCurrent = (spaceBetweenCurrent * 2 + spaceBetweenNext) / 3;
    eyeLx = (eyeLx * 2 + eyeLxNext) / 3;
    eyeLy = (eyeLy * 2 + eyeLyNext) / 3;
    eyeRxNext = eyeLxNext + eyeLwidthCurrent + spaceBetweenCurrent;
    eyeRyNext = eyeLyNext;
    eyeRx = (eyeRx * 2 + eyeRxNext) / 3;
    eyeRy = (eyeRy * 2 + eyeRyNext) / 3;
    eyeLborderRadiusCurrent = (eyeLborderRadiusCurrent * 2 + eyeLborderRadiusNext) / 3;
    eyeRborderRadiusCurrent = (eyeRborderRadiusCurrent * 2 + eyeRborderRadiusNext) / 3;

    if (autoblinker && millis() >= blinktimer) {
        close(); open();
        blinktimer = millis() + (blinkInterval * 1000) + (random(blinkIntervalVariation + 1) * 1000);
    }
    if (idle && millis() >= idleAnimationTimer) {
        int maxX = getScreenConstraint_X();
        int maxY = getScreenConstraint_Y();
        int drift = 2;
        int cx = maxX / 2;
        int cy = maxY / 2;
        eyeLxNext = constrain(cx + (int)random(-drift, drift + 1), 0, maxX);
        eyeLyNext = constrain(cy + (int)random(-drift, drift + 1), 0, maxY);
        idleAnimationTimer = millis() + (idleInterval * 1000) + (random(idleIntervalVariation + 1) * 1000);
    }
    if (hFlicker && millis() >= confusedUntilMs) { hFlicker = 0; }
    if (vFlicker && millis() >= laughUntilMs) { vFlicker = 0; }
    if (hFlicker) {
        if (hFlickerAlternate) { eyeLx += hFlickerAmplitude; eyeRx += hFlickerAmplitude; }
        else { eyeLx -= hFlickerAmplitude; eyeRx -= hFlickerAmplitude; }
        hFlickerAlternate = !hFlickerAlternate;
    }
    if (vFlicker) {
        if (vFlickerAlternate) { eyeLy += vFlickerAmplitude; eyeRy += vFlickerAmplitude; }
        else { eyeLy -= vFlickerAmplitude; eyeRy -= vFlickerAmplitude; }
        vFlickerAlternate = !vFlickerAlternate;
    }
    if (tired) eyelidsTiredHeightNext = eyeLheightCurrent / 2; else eyelidsTiredHeightNext = 0;
    if (angry) eyelidsAngryHeightNext = eyeLheightCurrent / 2; else eyelidsAngryHeightNext = 0;
    if (happy) eyelidsHappyBottomOffsetNext = (eyeLheightTarget * 4) / 5; else eyelidsHappyBottomOffsetNext = 0;

    eyelidsTiredHeight = (eyelidsTiredHeight * 2 + eyelidsTiredHeightNext) / 3;
    eyelidsAngryHeight = (eyelidsAngryHeight * 2 + eyelidsAngryHeightNext) / 3;
    eyelidsHappyBottomOffset = (eyelidsHappyBottomOffset * 2 + eyelidsHappyBottomOffsetNext) / 3;
}

void OledDisplay::drawEyeFrames() {
    _display.clearDisplay();
    _display.fillRoundRect(eyeLx, eyeLy, eyeLwidthCurrent, eyeLheightCurrent, eyeLborderRadiusCurrent, SSD1306_WHITE);
    _display.fillRoundRect(eyeRx, eyeRy, eyeRwidthCurrent, eyeRheightCurrent, eyeRborderRadiusCurrent, SSD1306_WHITE);

    _display.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx, eyeLy + eyelidsTiredHeight - 1, SSD1306_BLACK);
    _display.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy + eyelidsTiredHeight - 1, SSD1306_BLACK);

    _display.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy + eyelidsAngryHeight - 1, SSD1306_BLACK);
    _display.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx, eyeRy + eyelidsAngryHeight - 1, SSD1306_BLACK);

    _display.fillRoundRect(eyeLx - 1, (eyeLy + eyeLheightCurrent) - eyelidsHappyBottomOffset + 1, eyeLwidthCurrent + 2, eyeLheightDefault, eyeLborderRadiusCurrent, SSD1306_BLACK);
    _display.fillRoundRect(eyeRx - 1, (eyeRy + eyeRheightCurrent) - eyelidsHappyBottomOffset + 1, eyeRwidthCurrent + 2, eyeRheightDefault, eyeRborderRadiusCurrent, SSD1306_BLACK);

    _display.display();
}

void OledDisplay::drawDebug(const ControlState& state) {
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 0);
    _display.printf("D:%3dmm %s\n", state.currentDistanceMM, state.isCliff ? "CLIFF" : "ok");
    _display.printf("ST:%s\n", state.status.c_str());
    if (!state.imu.healthy) _display.println("IMU:OFF");
    else _display.printf("P:%+.0f R:%+.0f\n", state.imu.pitch, state.imu.roll);
    _display.printf("PWR:%d%%", state.maxPowerPercent);
}

void OledDisplay::drawEyes(const ControlState& state) {
    stepEyeState();
    drawEyeFrames();
}

void OledDisplay::render(const ControlState& state) {
    if (!_healthy) return;

    // Pure motion gate: interpret petting only when not generating motion (DRIVING/WIGGLE!/CLIFF WIGGLE!)
    extern RobotStateStore stateStore;
    InputEvent evt;
    bool hasInput = stateStore.takeInput(evt);
    bool robotMovingForGate = (state.status == "DRIVING" || state.status == "WIGGLE!" || state.status == "CLIFF WIGGLE!");
    bool inMotionCooldown = (millis() - state.lastMotionMs < (unsigned long)MOTION_COOLDOWN_MS);
    bool isAngryGate = state.displayAngryUntilMs != 0 && (long)(millis() - state.displayAngryUntilMs) < 0;
    bool isHappyGate = state.displayHappyUntilMs != 0 && (long)(millis() - state.displayHappyUntilMs) < 0;
    bool worriedGate = state.displayWorriedUntilMs != 0 && (long)(millis() - state.displayWorriedUntilMs) < 0;
    bool isWigglingForGate = state.status == "WIGGLE!" || state.status == "CLIFF WIGGLE!";
    bool isStoppedGate = state.status == "STOPPED";
    bool sleepyGate = isStoppedGate && (millis() - lastActiveMs >= EYE_SLEEPY_AFTER_MS);

    if (hasInput) {
        if (robotMovingForGate || inMotionCooldown) {
            Serial.printf("[DROP] %s gated by motion%s status=%s age=%lums\n", evt.type==INPUT_SHAKEN?"SHAKEN":"NUDGED", robotMovingForGate?"":" cooldown", state.status.c_str(), (unsigned long)(millis()-state.lastMotionMs));
        } else {
            int inputPrec = (evt.type==INPUT_SHAKEN)?2:1;
            int activePrec = isAngryGate?2 : (isHappyGate||worriedGate||isWigglingForGate?1:0);
            bool busy = (activePrec>0);
            bool isSleepyNow = sleepyGate;
            if (isSleepyNow) {
                // Sleeping: any petting wakes + reacts per input (one-frame)
                lastActiveMs = millis();
                if (wasSleepy) { wasSleepy = false; applyPreset(Preset_Normal); }
                if (evt.type==INPUT_SHAKEN) {
                    stateStore.setDisplayAngry(millis() + ANGRY_MOOD_MS);
                } else {
                    stateStore.setDisplayHappy(millis() + HAPPY_MOOD_MS);
                }
                stateStore.wakeFromSleep();
            } else if (!busy) {
                if (evt.type==INPUT_SHAKEN) stateStore.setDisplayAngry(millis() + ANGRY_MOOD_MS);
                else stateStore.setDisplayHappy(millis() + HAPPY_MOOD_MS);
            } else {
                if (inputPrec > activePrec) {
                    // Higher prec overrides (angry overrides happy)
                    if (evt.type==INPUT_SHAKEN) stateStore.setDisplayAngry(millis() + ANGRY_MOOD_MS);
                } else {
                    Serial.printf("[DROP] %s ignored, busy %s prec %d vs %d\n", evt.type==INPUT_SHAKEN?"SHAKEN":"NUDGED", isAngryGate?"angry":(isHappyGate?"happy":(worriedGate?"worried":"wiggling")), inputPrec, activePrec);
                }
            }
        }
    }
    bool debug = state.displayDebugOn;
    bool worried = state.displayWorriedUntilMs != 0 && (long)(millis() - state.displayWorriedUntilMs) < 0;
    bool isWiggling = state.status == "WIGGLE!" || state.status == "CLIFF WIGGLE!";
    bool isStopped = state.status == "STOPPED";

    // Single source for idle: any non-STOPPED resets timer; sync with store's lastMotionMs to avoid core drift
    if (!isStopped) lastActiveMs = millis();
    if (state.lastMotionMs != 0 && state.lastMotionMs > lastActiveMs) lastActiveMs = state.lastMotionMs;
    bool sleepy = isStopped && (millis() - lastActiveMs >= EYE_SLEEPY_AFTER_MS);
    // Gentle wake: reset sleep timer and wasSleepy latch (edge-only log)
    if (state.displayWakeResetMs != 0 && millis() - state.displayWakeResetMs < 1500) {
        bool wasSleepyBefore = wasSleepy;
        lastActiveMs = millis();
        if (wasSleepy) { wasSleepy = false; applyPreset(Preset_Normal); }
        if (wasSleepyBefore) {
            Serial.printf("[WAKE] display woke from sleep via gentle nudge (wakeMs=%lu wasSleepy=1)\n", (unsigned long)state.displayWakeResetMs);
        }
    }
    bool isAngryShake = state.displayAngryUntilMs != 0 && (long)(millis() - state.displayAngryUntilMs) < 0;
    bool isHappyNudge = state.displayHappyUntilMs != 0 && (long)(millis() - state.displayHappyUntilMs) < 0;

    // Forced mood from web UI overrides auto mapping (debug still wins)
    bool hasForced = state.displayMoodOverride >= 0;
    // Edge-trigger preset changes so per-frame applyPreset does not kill blink
    if (hasForced && state.displayMoodOverride != lastMoodOverride) {
        int m = state.displayMoodOverride;
        if (m == 4) applyPreset(Preset_Focused);
        else if (m == 5) applyPreset(Preset_Sleeping);
        else applyPreset(Preset_Normal); // DEFAULT/TIRED/ANGRY/HAPPY all use Normal size
        lastMoodOverride = m;
    } else if (!hasForced && lastMoodOverride != -1) {
        applyPreset(Preset_Normal);
        lastMoodOverride = -1;
    }
    // Auto-sleep edge-trigger (60s idle) — edge-only log
    if (!hasForced) {
        if (sleepy && !wasSleepy) {
            Serial.printf("[SLEEP] idle %lums (threshold %dms) status=%s -> SLEEPING\n", (unsigned long)(millis() - lastActiveMs), EYE_SLEEPY_AFTER_MS, state.status.c_str());
            applyPreset(Preset_Sleeping); wasSleepy = true;
        } else if (!sleepy && wasSleepy) {
            Serial.printf("[WAKE] auto wake after %lums sleep, status=%s -> NORMAL\n", (unsigned long)(millis() - lastActiveMs), state.status.c_str());
            applyPreset(Preset_Normal); wasSleepy = false;
        }
    }
    // Mood mapping: angry shake > forced > happy nudge > worried > sleepy > happy > default (debug always wins)
    if (debug) {
        tired = 0; angry = 0; happy = 0;
        idle = 0; autoblinker = 0;
    } else if (isAngryShake) {
        tired = 0; angry = 1; happy = 0;
        idle = 0; autoblinker = 1;
        if (wasSleepy) { wasSleepy = false; applyPreset(Preset_Normal); }
    } else if (hasForced) {
        int m = state.displayMoodOverride;
        if (m == 4) {
            tired = 0; angry = 0; happy = 0;
            idle = 0; autoblinker = 1;
        } else if (m == 5) {
            tired = 0; angry = 0; happy = 0;
            idle = 0; autoblinker = 0;
        } else {
            tired = (m == 1);
            angry = (m == 2);
            happy = (m == 3);
            idle = (m == 0) ? 1 : 0;
            autoblinker = 1;
        }
    } else if (isHappyNudge) {
        tired = 0; angry = 0; happy = 1;
        idle = 1; autoblinker = 1;
        if (wasSleepy) { wasSleepy = false; applyPreset(Preset_Normal); }
    } else if (worried) {
        // Cliff: open squircle with confused horizontal shiver, no brow
        tired = 0; angry = 0; happy = 0;
        idle = 0; autoblinker = 1;
        // ensure confused shiver is active during worried window
        if (!hFlicker) { hFlicker = 1; confusedUntilMs = state.displayWorriedUntilMs; }
    } else if (sleepy) {
        // Auto sleeping after 60s idle: closed static, no blink/drift
        tired = 0; angry = 0; happy = 0;
        idle = 0; autoblinker = 0;
    } else if (isWiggling || (happyUntilMs != 0 && (long)(millis() - happyUntilMs) < 0)) {
        if (isWiggling) happyUntilMs = millis() + 800;
        tired = 0; angry = 0; happy = 1;
        idle = 1; autoblinker = 1;
    } else {
        tired = 0; angry = 0; happy = 0;
        idle = 1; autoblinker = 1;
    }

    // Step eye state even if frame skipped / I2C busy — blink/idle must progress
    stepEyeState();

    if (millis() - fpsTimer < (unsigned long)frameInterval) return;

    if (_i2cMutex && xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    if (debug) {
        _display.clearDisplay();
        drawDebug(state);
        _display.display();
    } else {
        drawEyeFrames();
    }

    fpsTimer = millis();
    if (_i2cMutex) xSemaphoreGive(_i2cMutex);
}

void OledDisplay::hello() {
    if (!_healthy) return;
    if (_i2cMutex && xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
    _display.clearDisplay();
    _display.setTextSize(1);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 0);
    _display.println("Hello Desky");
    _display.println("----------------");
    if (WiFi.status() == WL_CONNECTED) { _display.print("IP "); _display.println(WiFi.localIP().toString()); }
    else _display.println("WiFi: connecting");
    _display.print("I2C 0x"); _display.println(String(_addr, HEX));
    _display.display();
    if (_i2cMutex) xSemaphoreGive(_i2cMutex);
}

void OledDisplay::triggerAnim(int anim) {
    if (isTransitioning() && anim == 1) return; // drop blink during transition
    switch (anim) {
        case 1: close(); open(); break;
        case 2: hFlicker = 1; confusedUntilMs = millis() + 600; break;
        case 3: vFlicker = 1; laughUntilMs = millis() + 600; break;
        default: break;
    }
}

bool OledDisplay::healthy() const { return _healthy; }
