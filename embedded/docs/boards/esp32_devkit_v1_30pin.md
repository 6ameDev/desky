## Pin Categories Reference Table

| Pin Category | What it is Great For | Where NOT to Use | Completely Avoid Using For |
| :--- | :--- | :--- | :--- |
| **Safe Output / PWM** <br>*(Standard GPIO)* | Motor inputs (`IN1-IN4`), servos, buzzer, status LEDs. | Pins requiring constant high logic during deep sleep. | Internal SPI flash (pins 6–11, not exposed on this board). |
| **Input-Only** <br>*(GPI)* | Reading battery voltage, analog sensors, buttons (needs external pull-up). | Actuators, sleep lines, motor direction, or internal software pull-up tasks. | **Any Output task.** (Writing `HIGH` or `LOW` in code does nothing). |
| **RTC / Deep Sleep** <br>*(Low-Power Domain)* | **Motor Sleep (`EEP`)**, wakeup buttons, capacitive touch triggers. | High-speed, continuous hardware protocols if short on pins. | Do not use strapping RTC pins (`D0`, `D2`, `D12`, `D15`) for critical outputs. |
| **Strapping Pins** <br>*(Boot Config)* | Temporary status lines or logic that defaults low/high naturally. | Sleep lines or control lines that pull against the required boot state. | **Crucial inputs/outputs** that hold a specific voltage during a hardware reset. |
| **Serial Debug** <br>*(UART0)* | Flashing code, `Serial.print()` debugging via micro-USB. | Anything else unless absolutely desperate for pins. | Hardware that sends constant signals at boot (blocks code uploads). |

## Pin Hardware Mapping & Allocation


| Pin Label | Pin Family Categories | Factory & Project Assignment Status |
| :--- | :--- | :--- |
| **3V3** | Power Output (Regulated) | ⚙️ **Factory:** 3.3V Output Line <br>📌 **Project:** Supplies clean power to `3V3` Net Bus |
| **GND (Top Left)** | Ground Plane | ⚙️ **Factory:** System Ground <br>📌 **Project:** Master Ground Line |
| **D15** | Strapping (Must be LOW at boot), PWM, RTC | ⚠️ **Conditional:** Avoid for critical states |
| **D2** | Strapping (Must be LOW at boot), PWM, RTC | ⚙️ **Factory:** **Onboard Blue LED** (Avoid using for external logic) |
| **D4** | Safe Output / PWM, RTC | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **RX2 / D16** | Safe Output / PWM, Hardware UART2 RX | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **TX2 / D17** | Safe Output / PWM, Hardware UART2 TX | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **D5** | Safe Output / PWM, Hardware SPI CS | 🔄 **Reserved:** Standard SPI Bus Chip Select |
| **D18** | Safe Output / PWM, Hardware SPI CLK | 🔄 **Reserved:** Standard SPI Bus Clock |
| **D19** | Safe Output / PWM, Hardware SPI MISO | 🔄 **Reserved:** Standard SPI Bus Data In |
| **D21** | Safe Output / PWM, Hardware I2C SDA | 🔄 **Reserved:** Standard I2C Bus Data (IMU / OLED Display) |
| **RX0 / D3** | Serial Debug (UART0 RX) | ⚙️ **Factory:** Hardwired to Micro-USB (Do not connect components) |
| **TX0 / D1** | Serial Debug (UART0 TX) | ⚙️ **Factory:** Hardwired to Micro-USB (Do not connect components) |
| **D22** | Safe Output / PWM, Hardware I2C SCL | 🔄 **Reserved:** Standard I2C Bus Clock (IMU / OLED Display) |
| **D23** | Safe Output / PWM, Hardware SPI MOSI | 🔄 **Reserved:** Standard SPI Bus Data Out |
| **EN** | Reset Line | ⚙️ **Factory:** Hardware Reset Button |
| **VP / D36** | Input-Only, Analog ADC1 | 🛑 **Input Only:** Great for reading battery divider/sensors |
| **VN / D39** | Input-Only, Analog ADC1 | 🛑 **Input Only:** Great for reading battery divider/sensors |
| **D34** | Input-Only, Analog ADC1 | 🛑 **Input Only:** Great for reading battery divider/sensors |
| **D35** | Input-Only, Analog ADC1 | 🛑 **Input Only:** Great for reading battery divider/sensors |
| **D32** | Safe Output / PWM, RTC / Deep Sleep | 📌 **Project:** **Assigned to DRV8833 Sleep (`EEP`)** |
| **D33** | Safe Output / PWM, RTC / Deep Sleep | 🟢 *Available for general use / wakeup triggers* |
| **D25** | Safe Output / PWM, Analog DAC1 | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **D26** | Safe Output / PWM, Analog DAC2 | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **D27** | Safe Output / PWM | 📌 **Project:** **Assigned to DRV8833 Fault (`ULT`)** |
| **D14** | Safe Output / PWM, Hardware SPI | 🟢 *Available for Motor Control (`IN1-IN4`)* |
| **D12** | Strapping (Must be LOW at boot), PWM | ⚠️ **Conditional:** Must boot LOW. Do not use for `EEP` |
| **D13** | Safe Output / PWM, Hardware SPI | 🟢 *Available for general use / PWM* |
| **GND (Bottom Right)**| Ground Plane | ⚙️ **Factory:** System Ground <br>📌 **Project:** Connects to the main `GND` Flag |
| **VIN** | Raw Power Input (5V Max) | ⚙️ **Factory:** Main Power In <br>📌 **Project:** Connects to `+5V_BUS` flag from switch |
