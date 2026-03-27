# XIAO ESP32-S3 + ADAU1466 custom branch notes

This branch contains a working custom `squeezelite-esp32` setup for a XIAO ESP32-S3 used with an ADAU1466 + SSM3582 audio system.

## Included features

- working I2S output
- PWM-based master volume backend
- ADC resistor-ladder media buttons (ADKEY)

## Current GPIO assignments

- I2S BCLK = GPIO6
- I2S LRCK = GPIO5
- I2S SDATA = GPIO4
- PWM master volume = GPIO9
- ADKEY input = GPIO7

## PWM master volume

The firmware includes a PWM-based master-volume backend that drives an ADAU AUXADC input.

Current behavior:

- minimum volume -> about 0V
- maximum volume -> about 3.29V

This PWM path is used as a global master-volume control path for the ADAU DSP.

## ADKEY resistor ladder

The branch supports a 3-button resistor ladder on GPIO7.

Current mapping:

- PREV
- PLAY/PAUSE
- NEXT

## Important hardware note

On the tested board revision, the ADKEY line did not have a usable pull-up on the main board.

A working external fix was required:

- 18k resistor from 3.3V to ADKEY

Measured voltages after adding the external pull-up:

- idle ≈ 3.2V
- button 1 ≈ 0.46V
- button 2 ≈ 0.84V
- button 3 ≈ 1.0V

This external pull-up is required for stable ADC button detection on the tested board revision.

## ADKEY software notes

The ADKEY path currently uses:

- ADC-based polling
- simple software debounce
- fixed tuned thresholds based on measured hardware values
- optional debug logging through `CONFIG_ADKEY_DEBUG`

## Build environment

This branch is built using the local Docker image:

- `local/espressif-idf:4.4-pwm`

Current tested build mode:

- `DEPTH=32`

Example build flow:

- run `idf.py build -DDEPTH=32` inside the Docker image
- use `PROJECT_VER` environment variable if a custom visible firmware version tag is needed

## Flashing

The tested workflow flashes only the application image:

- `build/squeezelite.bin`
- flash address: `0x150000`

This keeps the rest of the device layout intact.

## Branch purpose

This branch is a hardware-tested checkpoint for the combined XIAO control feature set.

It intentionally includes both:

- PWM master volume control
- ADKEY resistor-ladder button control

It should be treated as a practical working branch, not as an upstream-ready minimal feature split.

