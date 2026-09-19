# Include variables from .env if it exists
-include .env
export

.PHONY: clean build build-release upload-monitor upload-monitor-release

clean:
	pio run -e desky -t clean

# Verify compilation (dev)
build:
	pio run -e desky

# Verify compilation (release)
build-release:
	pio run -e desky-release

# Build, upload to ESP32, and monitor serial output (dev)
upload-monitor:
	pio run -e desky -t upload -t monitor

# Build, upload to ESP32, and monitor serial output (release)
upload-monitor-release:
	pio run -e desky-release -t upload -t monitor
