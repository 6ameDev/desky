#pragma once
#include <Arduino.h>
#include "MotorDriver.h"

enum class WiggleDirection { FORWARD, BACKWARD, IN_PLACE };

// Non-blocking wiggle motion primitive: 1 wiggle = one left+right sway pair.
// The caller owns timing: call start() once, then update() every control tick.
// Tune (speeds, sway, tempo) comes from Config.h. The controller never
// brakes; motor authority on finish/abort stays with the caller.
class WiggleController {
public:
    explicit WiggleController(MotorDriver& motors);
    // Begins a wiggle. Ignored while one is already active (no mid-wiggle restart).
    void start(WiggleDirection dir, int pairs);
    // Drives one sway phase. Returns true exactly on a pair boundary when done.
    bool update();
    void abort();
    bool isActive() const;

private:
    MotorDriver& _motors;
    bool _active = false;
    WiggleDirection _dir = WiggleDirection::BACKWARD;
    int _totalPhases = 0;
    unsigned long _startMs = 0;
    int _baseSpeed = 0;
    // Opening sway side alternates every start() so repeated wiggles don't
    // always kick the same way first. Pair symmetry is unaffected.
    // Toggled at start, so the first wiggle opens left.
    bool _openLeft = false;
};
