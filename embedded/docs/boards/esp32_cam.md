## Pin Categories Reference Table

| Pin Category | What it is Great For | Where NOT to Use | Completely Avoid Using For |
| :--- | :--- | :--- | :--- |
| **Exposed & Free** <br>*(GPIO 12, 13, 14, 15, 16)* | I2C peripherals (OLED), basic sensors, buttons, or soft-serial. | High-power switching if using MicroSD (GPIO 14/15 clash with SD SPI). | Internal Camera Bus (DO NOT tap into hidden camera pins 4, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35). |
| **High Power Onboard** <br>*(GPIO 4)* | Driving the onboard bright flash LED (`Flashlight`). | External logic or precision analog reading (it is hardwired to a transistor circuit). | General purpose inputs/outputs (will rapidly cycle/flash during boot). |
| **Strapping Pins** <br>*(Boot Config: GPIO 0)* | Internal clock management for the OV2640 camera module. | Anything that pulls it `LOW` during boot (blocks standard code uploads). | **Crucial inputs** that pull low natively; if forced low at reset, the board enters flashing mode. |
| **Serial Debug / Flash** <br>*(UART0: GPIO 1 & 3)* | Programming via external FTDI/U2T adapter, `Serial.print()` logs. | General sensors or I2C communication lines. | Peripherals that transmit continuous data at power-on (will corrupt flashing process). |

## Pin Hardware Mapping & Allocation

| Pin Label | Pin Family Categories | Factory & Project Assignment Status |
| :--- | :--- | :--- |
| **5V** | Power Input / Output | ⚙️ **Factory:** Connects to onboard 5V-to-3.3V regulator <br>📌 **Project:** Input power from external 5V source or FTDI programmer |
| **GND** | Ground Plane | ⚙️ **Factory:** System Ground <br>📌 **Project:** Master Ground Line |
| **GPIO 12 / HS2_DATA2** | Free GPIO / MicroSD Data 2 | 🟢 **Highly Available:** Safe pin for digital inputs, sensors, or relays |
| **GPIO 13 / HS2_DATA3** | Free GPIO / MicroSD Data 3 | 🟢 **Highly Available:** Safe pin for digital inputs, sensors, or I2C/SPI alternatives |
| **GPIO 15 / HS2_CMD** | Strapping (Must be HIGH at boot), MicroSD Command | ⚠️ **Conditional:** Must be pulled HIGH during boot. Can be used for I2C SCL if SD card is omitted |
| **GPIO 14 / HS2_CLK** | Free GPIO / MicroSD Clock | 🟢 **Highly Available:** Excellent choice for I2C SDA if SD card is omitted |
| **GPIO 2 / HS2_DATA0** | Strapping (Must be LOW at boot), MicroSD Data 0 | 🛑 **Restricted:** Hardwired to an internal 10k resistor. Leave floating or use purely for MicroSD |
| **GPIO 4 / HS2_DATA1** | MicroSD Data 1 / Onboard Flash | 🔦 **Reserved:** Hardwired to the **Onboard Bright Flash LED** (High current draw) |
| **VCC (3V3)** | Power Output (Regulated) | ⚙️ **Factory:** 3.3V Output line directly from the internal LDO voltage regulator |
| **GND** | Ground Plane | ⚙️ **Factory:** System Ground |
| **GPIO 16 / U2RXD** | Free GPIO / Hardware UART2 RX | 🟢 **Highly Available:** Wired to the PSRAM chip. Safe as an input/output button if PSRAM isn't maxed out |
| **GPIO 0 / CLK** | Strapping (Must be HIGH for sketch execution), Camera Clock | ⚠️ **Critical Boot Pin:** Hardwired to the `IO0` button/pad. Connected to camera master clock. Must be HIGH to run code |
| **GPIO 3 / U0RXD** | Serial Debug (UART0 RX) | ⚙️ **Factory:** Hardwired to the programming interface line (connect to FTDI TX) |
| **GPIO 1 / U0TXD** | Serial Debug (UART0 TX) | ⚙️ **Factory:** Hardwired to the programming interface line (connect to FTDI RX) |
| **VOR / VCC_CAM** | Camera Power Domain | ⚙️ **Factory:** Typically outputs 3.3V or 2.8V to power the OV2640 module |
| **GND** | Ground Plane | ⚙️ **Factory:** System Ground |
