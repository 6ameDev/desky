# Include variables from .env if it exists
-include .env
export

.PHONY: clean build upload-monitor

clean:
	pio run -e desky -t clean

# Verify compilation
build:
	pio run -e desky

# Build, upload to ESP32, and monitor serial output
upload-monitor:
	pio run -e desky -t upload -t monitor
