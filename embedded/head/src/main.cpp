#include <Arduino.h>

// desky-head stub (task 1): hello-only boot banner.
// Face-unit hardware (OV3660, face OLED, talk-wire UART) is NOT touched
// here — camera/OLED/UDP code lands in tasks 2/3.

void setup() {
  Serial.begin(115200);
  Serial.println("[HEAD] desky-head stub alive");
}

void loop() {
  Serial.println("[HEAD] desky-head stub alive");
  delay(2000);
}
