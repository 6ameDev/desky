// Vendored RoboEyes core (FluxGarage, GPL-3.0) adapted for Desky squircle eyes
// Smooth exponential tweening, idle drift, auto-blink, moods.
// Original: https://github.com/FluxGarage/RoboEyes
#include "OledDisplay.h"
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
    // Idle/blink always on, drift fixed to 2 (minimal)
    autoblinker = true;
    idle = true;

    // ---- PRE-CALCULATIONS (RoboEyes tween) ----
    eyeLheightCurrent = (eyeLheightCurrent + eyeLheightNext) / 2;
    eyeLy += ((eyeLheightDefault - eyeLheightCurrent) / 2);
    eyeRheightCurrent = (eyeRheightCurrent + eyeRheightNext) / 2;
    eyeRy += (eyeRheightDefault - eyeRheightCurrent) / 2;

    if (eyeL_open && eyeLheightCurrent <= 2) eyeLheightNext = eyeLheightDefault;
    if (eyeR_open && eyeRheightCurrent <= 2) eyeRheightNext = eyeRheightDefault;

    eyeLwidthCurrent = (eyeLwidthCurrent + eyeLwidthNext) / 2;
    eyeRwidthCurrent = (eyeRwidthCurrent + eyeRwidthNext) / 2;
    spaceBetweenCurrent = (spaceBetweenCurrent + spaceBetweenNext) / 2;
    eyeLx = (eyeLx + eyeLxNext) / 2;
    eyeLy = (eyeLy + eyeLyNext) / 2;
    eyeRxNext = eyeLxNext + eyeLwidthCurrent + spaceBetweenCurrent;
    eyeRyNext = eyeLyNext;
    eyeRx = (eyeRx + eyeRxNext) / 2;
    eyeRy = (eyeRy + eyeRyNext) / 2;
    eyeLborderRadiusCurrent = (eyeLborderRadiusCurrent + eyeLborderRadiusNext) / 2;
    eyeRborderRadiusCurrent = (eyeRborderRadiusCurrent + eyeRborderRadiusNext) / 2;

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
    // One-shot flickers expire
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

    _display.clearDisplay();
    _display.fillRoundRect(eyeLx, eyeLy, eyeLwidthCurrent, eyeLheightCurrent, eyeLborderRadiusCurrent, SSD1306_WHITE);
    _display.fillRoundRect(eyeRx, eyeRy, eyeRwidthCurrent, eyeRheightCurrent, eyeRborderRadiusCurrent, SSD1306_WHITE);

    if (tired) eyelidsTiredHeightNext = eyeLheightCurrent / 2; else eyelidsTiredHeightNext = 0;
    if (angry) eyelidsAngryHeightNext = eyeLheightCurrent / 2; else eyelidsAngryHeightNext = 0;
    if (happy) eyelidsHappyBottomOffsetNext = eyeLheightCurrent / 2; else eyelidsHappyBottomOffsetNext = 0;

    eyelidsTiredHeight = (eyelidsTiredHeight + eyelidsTiredHeightNext) / 2;
    _display.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx, eyeLy + eyelidsTiredHeight - 1, SSD1306_BLACK);
    _display.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy + eyelidsTiredHeight - 1, SSD1306_BLACK);

    eyelidsAngryHeight = (eyelidsAngryHeight + eyelidsAngryHeightNext) / 2;
    _display.fillTriangle(eyeLx, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy - 1, eyeLx + eyeLwidthCurrent, eyeLy + eyelidsAngryHeight - 1, SSD1306_BLACK);
    _display.fillTriangle(eyeRx, eyeRy - 1, eyeRx + eyeRwidthCurrent, eyeRy - 1, eyeRx, eyeRy + eyelidsAngryHeight - 1, SSD1306_BLACK);

    eyelidsHappyBottomOffset = (eyelidsHappyBottomOffset + eyelidsHappyBottomOffsetNext) / 2;
    _display.fillRoundRect(eyeLx - 1, (eyeLy + eyeLheightCurrent) - eyelidsHappyBottomOffset + 1, eyeLwidthCurrent + 2, eyeLheightDefault, eyeLborderRadiusCurrent, SSD1306_BLACK);
    _display.fillRoundRect(eyeRx - 1, (eyeRy + eyeRheightCurrent) - eyelidsHappyBottomOffset + 1, eyeRwidthCurrent + 2, eyeRheightDefault, eyeRborderRadiusCurrent, SSD1306_BLACK);



    // Happy sparkle (big proper tear shine) only when HAPPY mood is active
    if (happy && eyelidsHappyBottomOffset > 4) {
        for (int i = 0; i < 2; i++) {
            int ex = (i == 0) ? eyeLx : eyeRx;
            int ey = (i == 0) ? eyeLy : eyeRy;
            int cx = ex + eyeLwidthCurrent - 12;
            int cy = ey + 8;
            _display.fillCircle(cx, cy, 7, SSD1306_WHITE);
            _display.fillCircle(cx, cy, 4, SSD1306_BLACK);
            _display.fillCircle(cx - 2, cy - 2, 2, SSD1306_WHITE);
        }
    }

    _display.display();
}

void OledDisplay::render(const ControlState& state) {
    if (!_healthy) return;
    if (millis() - fpsTimer < (unsigned long)frameInterval) return;
    fpsTimer = millis();

    bool debug = state.displayDebugOn;
    bool worried = state.displayWorriedUntilMs != 0 && (long)(millis() - state.displayWorriedUntilMs) < 0;
    bool isWiggling = state.status == "WIGGLE!" || state.status == "CLIFF WIGGLE!";
    bool isStopped = state.status == "STOPPED";

    if (!isStopped) lastActiveMs = millis();
    bool sleepy = isStopped && (millis() - lastActiveMs >= EYE_SLEEPY_AFTER_MS);

    // Forced mood from web UI overrides auto mapping (debug still wins)
    bool hasForced = state.displayMoodOverride >= 0;
    // Mood mapping: worried = confused shiver (no brow), sleepy = tired, happy = wiggle + big tear
    if (debug) {
        tired = 0; angry = 0; happy = 0;
        idle = 0; autoblinker = 0;
    } else if (hasForced) {
        tired = (state.displayMoodOverride == 1);
        angry = (state.displayMoodOverride == 2);
        happy = (state.displayMoodOverride == 3);
        idle = (state.displayMoodOverride == 0) ? 1 : 0;
        autoblinker = 1;
    } else if (worried) {
        // Cliff: open squircle with confused horizontal shiver, no brow
        tired = 0; angry = 0; happy = 0;
        idle = 0; autoblinker = 1;
        // ensure confused shiver is active during worried window
        if (!hFlicker) { hFlicker = 1; confusedUntilMs = state.displayWorriedUntilMs; }
    } else if (sleepy) {
        tired = 1; angry = 0; happy = 0;
        idle = 0; autoblinker = 1;
    } else if (isWiggling || (happyUntilMs != 0 && (long)(millis() - happyUntilMs) < 0)) {
        if (isWiggling) happyUntilMs = millis() + 800;
        tired = 0; angry = 0; happy = 1;
        idle = 1; autoblinker = 1;
    } else {
        tired = 0; angry = 0; happy = 0;
        idle = 1; autoblinker = 1;
    }

    if (_i2cMutex && xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != pdTRUE) return;

    if (debug) {
        _display.clearDisplay();
        drawDebug(state);
        _display.display();
    } else {
        drawEyes(state);
    }

    if (_i2cMutex) xSemaphoreGive(_i2cMutex);
}

void OledDisplay::hello() {
    if (!_healthy) return;
    if (_i2cMutex && xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) != pdTRUE) return;
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
    switch (anim) {
        case 1: close(); open(); break;
        case 2: hFlicker = 1; confusedUntilMs = millis() + 600; break;
        case 3: vFlicker = 1; laughUntilMs = millis() + 600; break;
        default: break;
    }
}

bool OledDisplay::healthy() const { return _healthy; }
