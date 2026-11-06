#!/usr/bin/env bash
# Build the spike (compile only). Needs arduino-cli and the esp32 core 3.3.x; same board settings as hardware/flash.sh.
# Output: spikes/abs-touch/build/. All four variants are in one image; see README.md.
set -euo pipefail
cd "$(dirname "$0")"
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB"
"${ARDUINO_CLI:-arduino-cli}" ${ARDUINO_CONFIG:+--config-file "$ARDUINO_CONFIG"} compile -b "$FQBN" --output-dir build abs_touch
