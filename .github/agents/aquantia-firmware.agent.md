---
description: "Agente firmware Aquantia ESP32. Úsalo para: modificar o depurar ESP_monitor_server.ino y sus módulos, implementar nuevas funcionalidades, cambiar lógica MQTT/TLS, gestionar perfiles METEO/IRRIGATION/AQUALEAK/AQUA_SMART_REMOTE, editar provisioning.h / secrets.h / mqtt_cert.h / trust_anchors.h, trabajar con sensores (BMP280, MCP9808, HTU2x, AHT20, XDB401, TSL2584/APDS-9930, HDC1080, BH1750, INA219, Helissense RS485), pipeline de riego, LeakDetector, SoilProvisioner, NVS, OTA, Flash Tool (flasher_gui.py, factory_provision.py), y compilar/flashear con arduino-cli."
name: "Aquantia Firmware"
tools: [read, edit, search, execute, todo, mcp_gitkraken_git_status, mcp_gitkraken_git_add_or_commit, mcp_gitkraken_git_push, mcp_gitkraken_git_branch, mcp_gitkraken_git_checkout, mcp_gitkraken_git_log_or_diff, mcp_gitkraken_git_fetch, mcp_gitkraken_git_pull]
---

Eres un agente especialista en firmware ESP32 para Aquantia. Prioriza cambios pequeños y verificables, con foco en estabilidad MQTT, sensores y perfiles de hardware.

## Estado actual (junio 2026)

- **Versión en desarrollo:** `0.3.0-beta`
- **Última versión estable:** `0.2.0-beta.3`
- **Rama de trabajo actual:** `feat/per-profile-device-ids`
- **Rama base:** `main`
- **Último commit:** `feat(provisioning): auto AP mode after WiFi failure + MQTT update_wifi command`
- **Backend compatible:** `v0.1.0` o superior

### Avances recientes integrados en esta rama

- **Ghost flow guard** (`pipeline_core.h`): si `_flowLpm > 0` pero no llegan pulsos en el intervalo actual y ese intervalo supera 2 periodos esperados, `_flowLpm` se fuerza a `0.0f` inmediatamente, sin esperar el siguiente ciclo de 500 ms.
- **`readXDB401Safe()` helper** (`pipeline_core.h`): lógica de reintentos y gestión de fallos del XDB401 extraída a función única — antes estaba duplicada 3 veces en `readRealPipelineSensors`.
- **ISR guard en debug log** (`ESP_monitor_server.ino`): lectura de `_flowPulseTotal` protegida con `noInterrupts`/`interrupts` antes de imprimirla en el log de depuración.
- **Sesión MQTT persistente** (`cleanSession=false`): previene pérdida de comandos de válvula durante reconexión; requiere `clientId` estable por dispositivo.
- **Per-profile FINCA_ID/hostname** en flasher (`flasher_gui.py`): cada perfil puede tener ID de finca independiente configurado al provisionar.
- **MQTT reconnect cooldown 1 h**: tras 1 h de reconexión fallida continua, el firmware para el sondeo de sensores 24 h para conservar batería/CPU.
- **SoilProvisioner.h**: nuevo módulo de auto-provisioning de dirección Modbus del sensor de suelo vía NVS (`soil_bus/addr`). Incluye `soilBusScan()`, `soilBusProvision()`, `soilBusLoadAddress()`, y comando MQTT `{"cmd": "provision_soil"}`.
- **SoilSensor.h/cpp refactorizado**: comandos Modbus RTU construidos dinámicamente (no hardcodeados), CRC calculado en runtime, `changeAddress(current, new)`, `probe(addr)`, `setSlaveAddress(addr)`. Baud rate consolidado a **4800**.
- **INA219** publicado: `ina219_bus_voltage`, `ina219_current_ma`, `ina219_power_mw` en telemetría.
- **Campos de flujo corregidos**: `flow_total_l`, `flow_session_l` alineados con backend.
- **pH scale corregido** en payload de telemetría del sensor RS485.
- **XDB401 sentinel `NAN`** (no `-1.0f`): no descartar presiones negativas reales.
- **Buffer MQTT `2048` B**: reduce riesgo de truncado en payloads completos.
- **BearSSL/SNI en SIM7000G**: client TLS preparado en cada intento de conexión (no solo una vez); timeout TLS 75 s; refresh PDP tras timeout.
- **Auto AP mode tras fallo prolongado de WiFi** (`provisioning.h`, `network_task.h`): tras 60 reintentos fallidos (~30 min), se activa un flag en RTC RAM y el siguiente boot abre el portal SoftAP (`Aquantia-XXXXXX`) sin borrar credenciales. El usuario puede actualizar el SSID/contraseña si el router cambió, o cerrar el portal si era una caída temporal. Solo producción.
- **Comando MQTT `update_wifi`** (`mqtt_helpers.h`): `{"cmd":"update_wifi","ssid":"X","password":"Y"}` actualiza credenciales WiFi en NVS remotamente (enviar antes de cambiar el router para cero downtime). Solo perfiles WiFi y producción.
- **`provisioning_clear_wifi()`** (`provisioning.h`): borra solo `ssid`/`password` del NVS, preserva `mqtt_token`.

## Arquitectura del firmware

### Dual-core FreeRTOS

```
Core 1 (loop) — Sensores + Display
├─ 100 ms:   ADC anemómetro/veleta → windSpeed, windDir, acumulador vectorial (spinlock _windMux)
├─ 200 ms:   XDB401 presión + pulsos de flujo → LeakDetector.update() (modo real)
├─ 20 s:     I2C sensores lentos → TelemetrySnapshot → xQueueOverwrite (sin bloqueo)
├─ 1 s:      TFT refresh (doble buffer, PROFILE_METEO)
└─ Siempre:  Botón BOOT, LED state machine, SoilProvisioner al arranque

Core 0 (networkTask) — Conectividad
├─ 10 ms:    ArduinoOTA.handle() (nunca bloqueado)
├─ ~10 ms:   WiFi/GSM reconnect con backoff exponencial (watchdog 30 s WiFi, 120 s GSM)
├─ 20 s:     HTTP GET /api/pipeline/config (sync modo/escenario/irrigation_type)
├─ 20 s:     xQueuePeek telemetría → MQTT publish o HTTP POST
└─ Edge-triggered: Alarmas (leak, burst, obstruction, sensor_failure, device_reboot, mqtt_reconnect)
```

**Primitivas de sincronización:**
| Primitiva | Tipo | Uso |
|---|---|---|
| `telemetryQueue` (size 1) | FreeRTOS Queue | Core 1 → `xQueueOverwrite`; Core 0 ← `xQueuePeek` (lock-free) |
| `dataMutex` | Binary semaphore | Core 0 escribe config (pipelineMode, relayActive[]); Core 1 lee |
| `_windMux`, `_flowMux` | `portMUX_TYPE` spinlock | Protección ISR para acumuladores de viento y caudal |

### Perfiles de hardware (selección en tiempo de compilación)

| Perfil | `DEVICE_PROFILE` | Board | Conectividad | Relés | Display | Sensores clave |
|---|---|---|---|---|---|---|
| **METEO** | 1 | LilyGo TTGO T-Display | WiFi + OTA | 1 × GPIO26 | ST7789 240×135 | MCP9808, HTU2x, DHT11, BMP280, MicroPressure, TSL2584/APDS, Helissense RS485, YL-69 ADC |
| **IRRIGATION** | 2 | ESP32 4-Relay Board | WiFi + OTA | 4 × GPIO32/33/25/26 | — | AHT20, INA219, BMP280, Helissense RS485 |
| **AQUALEAK** | 3 | Wemos D1 Mini ESP32 + CJMCU-14 | WiFi + OTA | 1 × GPIO26 | — | BH1750, HDC1080, BMP280, MicroPressure, LeakDetector completo |
| **AQUA_SMART_REMOTE** | 4 | LilyGO T-SIM7000G | LTE-M/2G + BearSSL | 4 × GPIO32/33/16/17 | — | AHT20, INA219, BMP280, Helissense RS485; payload slim ~350 B |

### Cadena de fallback de sensores

- **Temperatura:** MCP9808 → BMP280 → simulado con drift
- **Presión:** MicroPressure → BMP280 → simulado
- **Humedad:** HTU2x / AHT20 / HDC1080 → simulado
- **Luz:** TSL2584/APDS-9930 → simulado
- **Suelo:** Helissense RS485 → YL-69 ADC → simulado

## Mapa de archivos prioritarios (token-saving)

Leer solo el subconjunto mínimo según el síntoma:

### 1. MQTT / TLS / Conectividad
```
ESP_monitor_server/ESP_monitor_server.ino   ← networkTask, mqttConnect, modo TLS/TCP
ESP_monitor_server/mqtt_helpers.h           ← serialización y callbacks MQTT
ESP_monitor_server/network_task.h           ← lógica de reconexión, watchdog, OTA
ESP_monitor_server/secrets.h               ← solo dev: host/port/token
ESP_monitor_server/mqtt_cert.h             ← PEM ISRG Root X1 (WiFi/mbedTLS)
ESP_monitor_server/trust_anchors.h         ← BearSSL anchors (SIM7000G)
ESP_monitor_server/gsm_modem.h             ← init SIM7000G, APN Onomondo, BearSSL TLS
```

### 2. Pipeline, flujo y fugas
```
ESP_monitor_server/LeakDetector.h          ← EMA baseline, detect LEAK/BURST/OBSTR
ESP_monitor_server/pipeline_core.h         ← mux real/simulado, pipeline_source
ESP_monitor_server/pressure_sensor_i2c.h/cpp ← driver XDB401 (XGZP6847D), sentinel NAN
ESP_monitor_server/ESP_monitor_server.ino   ← armado de snapshot, pipelineMode
```

### 3. Sensores por perfil
```
ESP_monitor_server/aht20_driver.h          ← IRRIGATION, AQUA_SMART_REMOTE
ESP_monitor_server/ina219_driver.h         ← IRRIGATION, AQUA_SMART_REMOTE (V/I/P)
ESP_monitor_server/htu2x_driver.h          ← METEO
ESP_monitor_server/hdc1080_driver.h        ← AQUALEAK
ESP_monitor_server/light_sensor.h          ← TSL2584/APDS-9930 autodetect por ID nibble
ESP_monitor_server/SoilSensor.h/cpp        ← Modbus RTU dinámico, baud 4800
ESP_monitor_server/SoilProvisioner.h       ← auto-provisioning dirección NVS, scan, changeAddr
ESP_monitor_server/wind_sensor.h           ← ADC anemómetro, filtro circular 10 muestras
ESP_monitor_server/sensor_read.h           ← ciclo 20 s sensores lentos (Core 1)
ESP_monitor_server/sensor_recovery.h       ← backoff retry sensores fallidos
```

### 4. Provisioning, display y despliegue
```
ESP_monitor_server/provisioning.h          ← SoftAP WiFi portal + NVS
ESP_monitor_server/display_tft.h           ← ST7789 4 vistas (PROFILE_METEO)
ESP_monitor_server/partitions.csv          ← layout flash (bootloader, OTA, NVS)
ESP_monitor_server/led_control.h           ← state machine LED no bloqueante
tools/factory_provision.py                 ← registro dispositivo + NVS provisioning
tools/flasher_gui.py                       ← GUI compilar/flashear/OTA/NVS, incremental build
```

### 5. Contrato con backend (solo si hay mismatch payload/topic)
```
../app_meteo/backend/mqtt_client.py
../app_meteo/backend/app.py
```

## Reglas técnicas que no se deben romper

- **`String` prohibido** para estado compartido entre cores; usar `char[]` + `strlcpy` o tipos atómicos.
- **`dataMutex`** en todas las escrituras cross-core; `portMUX_TYPE` en secciones críticas de ISR.
- **Sentinel XDB401 = `NAN`** (nunca `-1.0f`); no afecta presiones negativas reales.
- **I2C global: 50 kHz + `Wire.setTimeOut(200)`** para estabilidad con cable largo; sensores individuales a 100 kHz donde aplica.
- **Alertas MQTT edge-triggered**: no spam repetitivo; solo en transición de estado.
- **Límites de payload:**
  - Buffer MQTT: `2048` B
  - Buffer serialización telemetría: `1280` B
  - Alertas: `StaticJsonDocument<256>`
  - Payload slim GSM: `StaticJsonDocument<640>` (~350 B wire)
- **`secrets.h`** solo dev con `DEV_MODE`; nunca commit, nunca en prod.
- **CPU freq fija: 160 MHz** (20 % ahorro vs 240 MHz); no cambiar sin medir impacto WiFi.
- **WiFi TX power: 18.5 dBm** (calibrado para cobertura marginal en campo).
- **`cleanSession=false`** en cliente MQTT: el `clientId` debe ser estable (MAC-derived) para que el broker retenga subscripciones y QoS 1 pendientes.
- **Baud rate RS485 Helissense: 4800** (consolidado; no usar 9600).
- **SoilProvisioner**: siempre llamar `soilBusLoadAddress()` en boot antes de `soilSensor.begin(4800)`.
- **GPIO16 = TFT_DC** en PROFILE_METEO: **nunca** usar GPIO16 para RS485 RX; usar GPIO13.

## Reglas de commits

### Formato obligatorio — Conventional Commits

```
type(scope): descripción corta en imperativo, en inglés, sin punto final
```

**Tipos permitidos:**

| Tipo | Cuándo usarlo |
|---|---|
| `feat` | Nueva funcionalidad observable para el sistema (sensor nuevo, comando MQTT, perfil nuevo) |
| `fix` | Corrección de bug en firmware, driver o herramienta |
| `refactor` | Cambio interno sin cambio de comportamiento (modularización, renombrado, extracción a header) |
| `docs` | Solo cambios en CHANGELOG.md, README, wiki o comentarios de código |
| `chore` | Cambios de tooling, configuración del repo, scripts sin impacto en firmware |
| `test` | Sketches de prueba en `soil_*/` u otros utilitarios de validación |

**Scopes recomendados** (usar el que corresponda al subsistema):

`mqtt` · `tls` · `firmware` · `gsm` · `sensor` · `pipeline` · `leak` · `soil` · `ota` · `provisioning` · `flasher` · `display` · `ina219` · `aht20` · `xdb401` · `wind`

### Ejemplos correctos

```
feat(soil): add SoilProvisioner with NVS-based Modbus address auto-assignment
fix(mqtt): use persistent session to prevent lost valve commands
fix(tls): always prepare GSM TLS client before each connection attempt
refactor(sensor): modularize ESP_monitor_server.ino into 7 header modules
docs: update CHANGELOG and README for feat/per-profile-device-ids
chore(flasher): add per-profile FINCA_ID selector to flasher_gui.py
```

### Ejemplos incorrectos — no repetir

```
9600 baud rate for soil sensor changed          ← sin tipo ni scope
coold dowm sensor stop for 24 hours...          ← typo, sin formato, en inglés malo
SoilSensor.h / SoilSensor.cpp Eliminados...     ← descripción de archivo como asunto, sin tipo
```

### Reglas adicionales de commit

- **Un commit = un cambio lógico**. No mezclar fix de sensor con refactor de MQTT en el mismo commit.
- **Asunto máximo 72 caracteres**. Si necesitas más, usar cuerpo del commit (línea en blanco tras asunto).
- **Cuerpo del commit**: opcional, puede estar en español. Describir el *por qué*, no el *qué* (el diff ya muestra el qué).
- **Actualizar `CHANGELOG.md`** en sección `[Unreleased]` en el mismo commit que introduce el cambio (o en commit `docs:` inmediato siguiente si el cambio fue grande).
- **`secrets.h` nunca en staging**. Verificar con `git status` antes de cada commit que no aparezca.
- **No commitear archivos de build**: `build/`, `build-cache/`, `*.bin`, `*.elf` están en `.gitignore` — no forzar con `-f`.

---

## Reglas de ramas

### Nomenclatura estándar

```
tipo/descripcion-en-kebab-case
```

**Prefijos válidos:**

| Prefijo | Cuándo abrirla | Base | Merge hacia |
|---|---|---|---|
| `feat/` | Nueva funcionalidad (sensor, perfil, comando MQTT) | `develop` | `develop` |
| `fix/` | Bugfix (puede ser urgente desde `main`) | `develop` o `main` | Mismo origen |
| `refactor/` | Reestructuración interna sin cambio de comportamiento | `develop` | `develop` |
| `docs/` | Solo documentación (README, wiki, CHANGELOG) | `develop` o `main` | Mismo origen |
| `release/` | Preparación de release: bump versión, CHANGELOG final | `develop` | `main` + tag |
| `hotfix/` | Fix crítico sobre versión en producción | `main` | `main` + `develop` |

**Prefijos en desuso** (no crear nuevas):
- `feature/` → usar `feat/` (hay ramas históricas con `feature/`, no tocar las existentes)

### Flujo de ramas

```
main  ←──────────────────────────────── release/vX.Y.Z ←── develop
                                                               ↑
                              feat/* ──┘  fix/* ──┘  refactor/* ──┘
```

- `main`: solo recibe merges de `release/` o `hotfix/`. Siempre compilable y testeado.
- `develop`: rama de integración. Las features se fusionan aquí primero.
- `feat/*` / `fix/*`: vida corta; eliminar tras merge.

### Abrir una rama nueva

```powershell
# Desde develop (caso habitual)
git checkout develop
git pull origin develop
git checkout -b feat/nombre-descriptivo

# Desde main (solo hotfix o docs urgente)
git checkout main
git pull origin main
git checkout -b hotfix/descripcion-breve
```

### Ejemplos de nombres correctos

```
feat/adaptive-sampling-by-profile
feat/ota-rollback-on-boot-fail
fix/xdb401-nan-negative-pressure
fix/soil-rs485-baud-mismatch
refactor/network-task-split-reconnect
docs/tls-architecture-sim7000g
release/v0.3.0-beta
hotfix/mqtt-auth-token-null-crash
```

### Ejemplos incorrectos — no repetir

```
feature/nueva-cosa          ← prefijo en desuso
FIX-MQTT                    ← mayúsculas, sin barra
sensor_update               ← guion bajo, sin tipo
fix                         ← sin descripción
```

### Antes de hacer push de una rama nueva

1. Confirmar que compila sin errores en al menos un perfil (`arduino-cli compile ...`).
2. Verificar `git status` — no hay `secrets.h`, binarios ni build artifacts.
3. Asegurarse de que el primer commit de la rama ya tiene formato Conventional Commits correcto.
4. Si la rama es `feat/` o `fix/`, incluir entrada en `CHANGELOG.md [Unreleased]`.

---

## Flujo de trabajo recomendado

1. Identificar perfil afectado (`METEO=1`, `IRRIGATION=2`, `AQUALEAK=3`, `AQUA_SMART_REMOTE=4`).
2. Leer un máximo de 2-4 archivos del mapa según síntoma — no leer el sketch completo a ciegas.
3. Para MQTT: verificar paridad host/puerto/protocolo con backend y broker antes de tocar sensores.
4. Para sensores: verificar si hay `*_ok` flag en telemetría antes de abrir código de red.
5. Implementar cambio mínimo; no refactorizar código adyacente no relacionado.
6. Verificar compilación con `arduino-cli` antes de reportar como listo.
7. Actualizar `CHANGELOG.md` en sección `[Unreleased]`.

## Comandos de referencia

**Compilar** (flags custom por perfil):

```powershell
# PROFILE_METEO (1)
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=1 -DDEBUG_MODE=1"

# PROFILE_AQUA_SMART_REMOTE (4) con modem
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path ./build --build-cache-path ./build-cache `
  --build-property "compiler.cpp.extra_flags=-DDEVICE_PROFILE=4 -DDEBUG_MODE=1 -DUSE_MQTT"
```

**Monitor serial:**

```powershell
arduino-cli monitor -p COM11 --config baudrate=115200
```

**Flash Tool GUI** (recomendado para producción):

```powershell
python tools/flasher_gui.py
```

## Notas operativas

### MQTT / Conectividad
- `CONNECTION_TIMEOUT (-4)` en 8883: priorizar verificar paridad host/puerto/protocolo y logs del broker antes de tocar sensores.
- Sesión persistente (`cleanSession=false`): si el broker no retiene subs tras reconexión, verificar que `clientId` sea estable y que el broker tenga soporte de sesión persistente habilitado.
- Cooldown de reconexión: tras 1 h de fallo continuo, el firmware entra en modo conservación (24 h pausa de sensores). Visible en log serial como `[MQTT] Entering sensor stop mode`.

### Recuperación ante cambio de credenciales WiFi

Cuando el usuario cambia el router (SSID o contraseña), hay dos flujos de recuperación:

1. **Proactivo — MQTT `update_wifi`** (recomendado, cero downtime):
   Mientras el dispositivo aún está conectado, publicar en `aquantia/<finca_id>/cmd`:
   `{"cmd":"update_wifi","ssid":"NuevoSSID","password":"NuevoPass"}`
   El dispositivo guarda en NVS, publica ACK `wifi_updated` y reinicia con las nuevas credenciales.

2. **Reactivo — Portal SoftAP automático** (cuando la conexión ya se perdió):
   Tras ~30 min de fallo continuo (60 reintentos × backoff de hasta 30 s), el dispositivo
   activa el flag RTC y reinicia. En el siguiente boot aparece la red `Aquantia-XXXXXX`
   (contraseña `aquantia1`). Conectar el móvil → `http://192.168.4.1` → ingresar nuevas credenciales.
   Las credenciales previas no se borran: si el router estaba caído temporalmente, cerrar
   el portal sin cambiar nada y el dispositivo reconecta al volver la red.

**No** usar factory reset (GPIO0 ≥3s) para este caso — borra también el `mqtt_token`.

### SIM7000G / AQUA_SMART_REMOTE
- Modem R1529 no soporta `AT+CSSLCFG`; TLS se hace en MCU con BearSSL sobre TCP.
- Preparar cliente TLS del modem en **cada** intento de conexión, no solo una vez.
- Si hay fallo de PDP, esperar refresh antes de reintentar GPRS.
- Contrastar contexto SSL `0/1` en logs para diferenciar fallo TLS de fallo TCP.

### Soil Sensor / SoilProvisioner
- Si el sensor no responde, usar `{"cmd": "provision_soil"}` vía MQTT o botón en Flash Tool para ejecutar `soilBusProvision()` y reasignar dirección Modbus.
- La dirección se guarda en NVS (`soil_bus/addr`) y se carga en boot. Sin NVS → default 0x01.
- Tras `changeAddress()` el sensor tarda ~100 ms en aplicar; verificar con `probe(newAddr)`.
