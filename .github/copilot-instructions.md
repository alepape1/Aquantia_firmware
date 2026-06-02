# Aquantia firmware context for Copilot

This repository contains the ESP32 firmware for Aquantia devices.

## Current verified state (June 2026)
- Last validated commit: `8c50eb45a6becc9c2ccdc8e4135b5bb527bac46e`.
- Active work branch for this state: `feat/SIM_MODEM`.
- Firmware line in use: `0.2.0-beta.3`.
- Backend compatibility target: `v0.1.0` or newer.

## Recent integrated work (SIM modem + MQTT)
- Added/maintained `PROFILE_AQUA_SMART_REMOTE (4)` for SIM7000G cellular operation.
- SIM7000 TLS hardening: SNI, longer TLS timeout, and SSL context diagnostics/switching (`ctx 0/1`).
- TLS client preparation now runs on every MQTT connect attempt (not one-time only).
- PDP refresh after MQTT timeout is part of recovery flow.
- MQTT tuning updated in latest commit:
  - `mqttClient.setBufferSize(2048)`
  - telemetry JSON + serialization buffer constrained to `1280`
  - payload fields pruned to reduce truncation risk.

## Files to read first (token-efficient)
1. MQTT/TLS on SIM7000G:
	- `ESP_monitor_server/ESP_monitor_server.ino` (`networkTask`, `mqttConnect`, TLS/TCP path)
	- `ESP_monitor_server/mqtt_helpers.h`
	- `ESP_monitor_server/secrets.h` (dev/local only)
	- `ESP_monitor_server/mqtt_cert.h` (when CA/TLS validation is relevant)
2. Pipeline and leak diagnostics:
	- `ESP_monitor_server/LeakDetector.h`
	- `ESP_monitor_server/pressure_sensor_i2c.h`
	- `ESP_monitor_server/pressure_sensor_i2c.cpp`
3. Provisioning/flashing:
	- `ESP_monitor_server/provisioning.h`
	- `ESP_monitor_server/partitions.csv`
	- `tools/factory_provision.py`
	- `tools/flasher_gui.py`

## Operational rules
- Keep edits minimal and focused; do not refactor unrelated firmware areas.
- Do not treat `secrets.h` as production config (dev only with `DEV_MODE`).
- Keep cross-core shared state off `String`; use `char[]`/atomics and guard writes with `dataMutex`.
- Keep XDB401 invalid sentinel as `NAN` (never revert to `-1.0f`).
- Keep I2C at 50 kHz and `Wire.setTimeOut(200)` for long-cable stability.
- Keep MQTT alerts edge-triggered to avoid broker spam.

## Token-saving policy
- Do not scan full sketch blindly; search first (`mqttConnect`, `networkTask`, `PROFILE_*`, `LeakDetector`).
- Do not open sensor drivers for pure MQTT incidents unless logs show loop blocking.
- Do not inspect sibling `app_meteo` repo unless topic/payload/endpoint contract mismatch is suspected.
- Avoid heavy or irrelevant paths: `wiki/assets`, `.claude/worktrees`, build outputs.

## Quick diagnostic reference
For fast triage, use the ultra-short checklist at:
- `.github/agents/aquantia-firmware-quick-diagnostics.md`
