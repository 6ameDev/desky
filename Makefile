# Include variables from .env if it exists
-include .env
export

.PHONY: clean build upload-monitor

clean:
	pio run -t clean

# Verify compilation
build:
	pio run

# Build, upload to ESP32, and monitor serial output
upload-monitor:
	pio run -t upload -t monitor
