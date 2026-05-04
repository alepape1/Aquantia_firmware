# Changelog — Aquantia Firmware (ESP32)

Todos los cambios notables de este proyecto se documentan en este archivo.
Formato basado en [Keep a Changelog](https://keepachangelog.com/es/1.1.0/).
Versiones siguiendo [Semantic Versioning](https://semver.org/lang/es/).

> **Compatibilidad backend:** Cada versión del firmware indica el backend compatible.
> Ver [app_meteo](https://github.com/alepape1/app_meteo) para las versiones del dashboard.

---

## [Unreleased] — feat/firmware-stability-phase1

### Añadido
- **Watchdog de aplicación en `networkTask`** (`esp_task_wdt`, timeout 30s, reset en pánico):
  - Si `networkTask` se bloquea (handshake TLS colgado, broker con TCP half-open, etc.) el ESP32 hace reset automático sin intervención humana.
  - `esp_task_wdt_reset()` se llama al inicio de cada iteración del bucle.
- **`telemetryQueue`** (FreeRTOS Queue, tamaño 1): arquitectura lock-free para la telemetría entre cores.
  - Core 1 construye el `TelemetrySnapshot` completo (incluyendo `calcAndResetWindVector()`) y publica con `xQueueOverwrite` — nunca bloquea.
  - Core 0 lee con `xQueuePeek` — nunca bloquea.
  - `dataMutex` queda reservado exclusivamente para proteger escrituras de config desde Core 0 (`pipelineScenario`, `telemetryIntervalMs`, `relayActive[]`) y la lectura brevísima (5ms max) de `relayActive[]` al construir el snapshot.
  - Elimina la latencia de hasta 50ms del antiguo `xSemaphoreTake(dataMutex, 50ms)` en `takeSnapshot()`.
- **Retry controlado para XDB401**: tras 5 fallos consecutivos de lectura el sensor se suspende 30s y reintenta `xdb401_begin()` automáticamente, en lugar de caer en modo simulación permanente sin aviso.

### Cambiado
- `takeSnapshot()` simplificada: solo hace `xQueuePeek` + actualización de `heap`/`rssi`/`uptime`. Toda la construcción del snapshot se mueve a Core 1 (loop) al finalizar la lectura de sensores.
- Lectura de sensores I2C en `loop()` ya no adquiere `dataMutex` (los sensores solo los escribe Core 1).
- Comentario obsoleto en `updatePipelineValues()` sobre `dataMutex` actualizado.

---

## [Unreleased] — feature/agrometeo-profile

### Cambiado
- **Driver XDB401 reemplazado por librería `pressure_sensor_i2c`** (`pressure_sensor_i2c.h` / `.cpp`):
  - Corregido el trigger I2C invertido: el sketch escribía `0x0A` en registro `0x30`; el protocolo correcto (datasheet) es escribir `0x30` en registro `0x0A`. Éste era el origen de las lecturas incorrectas de presión.
  - El polling activo del bit Sco (reg `0x30`, bit 3) reemplaza el delay fijo de 100ms — más robusto ante variaciones en el tiempo de conversión.
  - La conversión de temperatura usa la fórmula pura del datasheet (`raw/256`) sin offset empírico; verificar en hardware si el offset de +10°C sigue siendo necesario.
  - Las constantes `XDB401_ADDR_PRIMARY`, `XDB401_ADDR_ALT` y `XDB401_FULLSCALE_KPA` eliminadas del sketch; toda la configuración queda centralizada en `pressure_sensor_i2c.h` (`PRESSURE_SENSOR_I2C_ADDR = 0x6D`, `PRESSURE_SENSOR_FULLSCALE = 400.0f`).
  - La API interna del sketch (`xdb401_begin`, `xdb401_read`, `xdb401_readPressureBar`) se mantiene como wrappers finos sobre la librería — sin cambios en el resto del sketch.

### Añadido
- **Umbrales de alerta de presión** como `#define` en la sección de constantes pipeline:
  - `PRESSURE_MIN_NORMAL 1.5 bar` — umbral bajo (ajustado para red pública de Lanzarote que puede bajar a ~2 bar en verano)
  - `PRESSURE_MAX_NORMAL 7.0 bar` — umbral de sobrepresión
  - `PRESSURE_DROP_ALERT 1.0 bar` — caída rápida (fuga activa)
  - `PRESSURE_HYDRO_TARGET 3.5 bar` y `PRESSURE_HYDRO_MARGIN 0.5 bar` — control del hidropresor
- **Temperatura del agua via XDB401**: el sensor de presión reporta temperatura interna del fluido.
  - Variable global `xdb401Temperature` (NAN si sensor no disponible).
  - `readRealPipelineSensors()` actualiza `xdb401Temperature` en cada lectura real (usa `xdb401_read()` en lugar de `xdb401_readPressureBar()`).
  - Campo `xdb401_temperature` añadido al payload MQTT de telemetría (solo si el sensor está presente y la lectura es válida).
  - Pantalla TFT pipeline view: muestra `Taq: XX.X °C` en la franja inferior cuando XDB401 está activo.

### Corregido
- Driver inicial basado en protocolo incorrecto (requestFrom directo sin trigger, fórmula 14-bit) sustituido por el protocolo real del datasheet XGZP6847D.

---

## [Unreleased] — feature/agrometeo-profile

### Añadido
- **Modo debug configurable** desde el Flash Tool GUI: nueva casilla "🐛 Debug (serie)" que inyecta `-DDEBUG_MODE=1` al compilar. Sin la casilla, toda la salida serie queda silenciada en producción.
- Macros `DLOGF` / `DLOGLN` / `DLOG` que mapean a `Serial.printf/println/print` cuando `DEBUG_MODE` está definido y son no-ops en caso contrario. Todos los `Serial.printf/println/print` del sketch sustituidos por las macros.
- El Flash Tool GUI invalida la caché del binario automáticamente al cambiar la casilla de debug, y registra en `build_meta.json` si el binario fue compilado en modo debug o release.
- **Vista pipeline en pantalla TFT (`PROFILE_METEO`)**: pulsando BTN_LEFT o BTN_RIGHT con la pantalla ya encendida se alterna entre la vista meteorológica y una nueva vista de agua que muestra `pipeline_pressure` (bar) y `pipeline_flow` (L/min). Si el modo pipeline es `"sim"`, los valores se muestran en naranja (`C_SIM`) con badge `[SIM]`, igual que el resto de sensores sin hardware real. Puntos de paginación en la cabecera indican la vista activa.

### Cambiado
- `Serial.begin(115200)` siempre activo (necesario para OTA y monitor de arranque); el resto de salida serie queda gateado por `DEBUG_MODE`.
- Lógica de botones en `loop()` reescrita con detección de flanco (rising/falling edge) para evitar cambios de vista repetidos por bounce.

---

## [feature/agrometeo-profile] — en desarrollo

### Añadido
- **Nuevo perfil `PROFILE_AGROMETEO` (valor 3)** para placa CJMCU-14 (ESP32 + BH1750 + HDC1080 + BMP280).
- Soporte inline para **HDC1080** (temperatura y humedad, I2C 0x40) con protocolo propio, sin librería externa, guarded para no interferir con HTU2x en METEO/IRRIGATION.
- Soporte para **BH1750** (iluminancia, librería `claws/BH1750`) como sensor de luz en AGROMETEO.
- BMP280 reutilizado en AGROMETEO (existente en METEO) para temperatura secundaria y presión atmosférica (kPa).
- **Parámetros agrometeorológicos calculados** publicados en telemetría MQTT: `dew_point` (punto de rocío, Magnus), `heat_index` (índice de calor, solo si T>27°C y HR>40%), `abs_humidity` (humedad absoluta g/m³).
- `RELAY_COUNT=0` en AGROMETEO: sin relays ni control de válvulas; pipeline pressure/flow preparado para futuros sensores (stub `readRealPipelineSensors`).
- Log serie `[1s]` específico para AGROMETEO con todos los parámetros derivados.
- Reporte DEBUG_MODE actualizado para mostrar estado de HDC1080 y BH1750 en AGROMETEO.
- **Estado LED `LED_PROVISIONING`**: triple parpadeo lento (300/300 ms × 3, pausa 1,8 s) que se activa al entrar al portal captivo SoftAP. Distingue visualmente "esperando configuración WiFi del usuario" del parpadeo rápido de búsqueda de red (`LED_WIFI_CONNECTING`).

### Cambiado
- `RELAY_PINS` usa array dummy de tamaño 1 en AGROMETEO para evitar array de longitud cero.
- `relayActive` usa `RELAY_COUNT > 0 ? RELAY_COUNT : 1` como tamaño para el mismo motivo.
- `temperatureSourceName()` y `pressureSourceName()` extendidas con rama `PROFILE_AGROMETEO`.
- Bloque de init HTU2x en `setup()` y loop guarded con `#if DEVICE_PROFILE != PROFILE_AGROMETEO`.
- Bloque de init TSL2584/APDS-9930 guarded igual; en AGROMETEO `tsl_ok=false` siempre.
- Cadenas de nombre de perfil en DEBUG_MODE actualizadas para incluir "AGROMETEO".

---

## [v0.1.0] — 2026-04-22

**Backend compatible:** `v0.1.0`

Primera versión estable del firmware Aquantia. Consolida todas las funcionalidades desarrolladas durante el ciclo beta: MQTT/TLS, perfiles METEO/IRRIGATION, provisioning SoftAP, Flash Tool GUI y simulador pipeline.

### Cambiado
- Versión de release alineada con backend `v0.1.0`

---

## [v0.1.0-beta.3] — 2026-04-16

**Backend compatible:** `v0.1.0-beta.3`

Beta de estabilización centrada en dejar la conectividad segura y el flujo de flash más robustos antes del siguiente despliegue.

### Cambiado
- Se endurece la preparación del cliente TLS y la sincronización del reloj antes del handshake MQTT seguro
- El flujo de compilación y flash queda más afinado para placas TTGO/ESP32 y pruebas locales
- La herramienta de flasheo muestra mejores pistas para conectar con backend y broker en entorno local

### Corregido
- Se evita el fallo intermitente de conexión MQTT sobre TLS por cadena CA incompleta o reloj aún no sincronizado
- El firmware queda listo para salir como siguiente beta sin requerir cambios extra en el backend

## [v0.1.0-beta.2] — 2026-04-16

**Backend compatible:** `v0.1.0-beta.2`

Beta enfocada en dejar el firmware listo para convivir con el simulador actual y el futuro caudalímetro real.

### Añadido
- Soporte de `pipeline_mode` con valores `sim` y `real`
- Comando MQTT dirigido por MAC para aplicar configuración de pipeline al dispositivo correcto
- Nuevos campos de telemetría: `pipeline_mode`, `pipeline_source`, `pipeline_pressure_ok`, `pipeline_flow_ok`

### Cambiado
- Sincronización del pipeline mejorada: el ESP32 consulta [GET] `/api/pipeline/config` y mantiene compatibilidad con el esquema anterior
- El simulador pasa a ser fallback automático cuando el modo hardware está activo pero el sensor aún no está montado

### Corregido
- El cambio de escenario ya no depende solo de esperar al próximo ciclo largo de polling
- La base del firmware queda preparada para integrar el driver real del sensor sin romper el flujo existente

## [v0.1.0-beta.1] — 2026-04-13

**Backend compatible:** `v0.1.0-beta.1`

Primera versión beta pública. Firmware completo con MQTT/TLS, perfiles de dispositivo,
provisioning SoftAP, Flash Tool GUI y sistema de autenticación DEV/PROD.

### Añadido
- **MQTT sobre TLS** (puerto 8883): `PubSubClient` + `WiFiClientSecure`, certificado CA configurable
- **FreeRTOS dual-core**: Core 1 = sensores + display, Core 0 = red/MQTT (`networkTask`)
- **Perfiles de dispositivo en tiempo de compilación**:
  - `PROFILE_METEO` (defecto): MCP9808, HTU2x, MicroPressure, TSL2584/APDS-9930, DHT11, YL-69, pantalla TFT 240×135
  - `PROFILE_IRRIGATION`: 4 relays (GPIO 32/33/25/26), sin sensores meteo
- **Provisioning SoftAP**: captive portal con lista WiFi clicable, credenciales guardadas en NVS, factory reset con botón BOOT
- **DEV/PROD mode**: `#define DEV_MODE` usa `secrets.h` directamente; sin `DEV_MODE` usa MAC + token NVS para MQTT
- **Timestamp NTP**: campo `ts` (epoch Unix) en telemetría — el backend lo usa como timestamp real de la medición
- **Flash Tool GUI** (`flasher_gui.py`): flash USB, OTA, perfiles, DEV/PROD toggle, registro CSV, QR de claim, botón borrar NVS
- **Serial de dispositivo**: formato `AQ-{MAC}` (sin Flash ID), generado desde NVS
- **QR de claim**: codifica la URL completa `https://meteo.aquantialab.com/claim?serial=AQ-...`
- **Sensor YL-69**: humedad de suelo en GPIO 33, campo `soil_moisture` en payload MQTT
- **Simulador pipeline**: presión y caudal con ruido sinusoidal determinista (coincide con `pipeline_sim.py` del backend)
- **Modo simulación por sensor**: cada sensor tiene un flag `_ok`; si falla, usa valores simulados con drift lento
- **OTA seguro**: apaga todos los relays antes de iniciar flash, contraseña OTA opcional vía `secrets.h`
- **Debug Mode**: `#define DEBUG_MODE` activa reporte completo cada 5s con estado de todos los subsistemas

### Cambiado
- Perfiles `PROFILE_METEO`/`PROFILE_IRRIGATION` movidos al inicio del .ino (antes de cualquier `#if`)
- `mqtt_user`/`mqtt_pass` solo se sobreescriben con MAC+token NVS dentro de `#ifndef DEV_MODE`
- Redondeo de floats en payload MQTT: 2 decimales para temperatura/humedad/viento, 1 para dirección/luz/suelo (ahorro ~15% tamaño)
- Autodetección de sensor de luz: TSL2584 (ID `0xAx`) y APDS-9930 (ID `0x3x`) en dirección I2C `0x39`
- HTU2x implementado sin librería externa (I2C directo), con calentamiento de arranque 3s
- Pantalla TFT apagada tras 60s de inactividad, cualquier botón la enciende

### Corregido
- Guard `#ifndef DEV_MODE` para asignar `mqtt_user` = MAC (antes se sobreescribía siempre)
- Pantalla AP mostrada antes de `provisioning_start_ap()` (evita pantalla negra durante configuración)
- Progress bar del Flash Tool se movía incorrectamente
- `python.exe` en lugar de `pythonw.exe` para OTA (evita WinError 10053 en Windows)
- Tabla de particiones real leída antes de borrar NVS (evita borrar partición incorrecta)
- `windMux` declarado antes de `accumulateWindVector` (crash en compilación)

### Perfiles de hardware soportados

| Componente | PROFILE_METEO | PROFILE_IRRIGATION |
|------------|:---:|:---:|
| MCP9808 (T exterior) | ✓ | — |
| HTU2x (T+H interior) | ✓ | ✓ |
| SparkFun MicroPressure | ✓ | — |
| TSL2584 / APDS-9930 (luz) | ✓ | ✓ |
| DHT11 (T+H backup) | ✓ | — |
| YL-69 (humedad suelo) | ✓ | — |
| Anemómetro + veleta | ✓ | — |
| Pantalla TFT 240×135 | ✓ | — |
| Relays | 1 (GPIO 26) | 4 (GPIO 32/33/25/26) |

---

## [Sin versión — rama main] — 2026-03-26 a 2026-04-01

Primera versión operativa con FreeRTOS. Incluida en `v0.1.0-beta.1`.

- Refactor firmware v4: FreeRTOS dual-core
- Perfiles METEO/IRRIGATION con `DEVICE_PROFILE`
- Multi-relay (bitmask `relay_active`)
- Sensor YL-69 (campo 16 en CSV HTTP legacy)
- Simulador pipeline en firmware
- Header `X-Device-MAC` en HTTP POST
- OTA scripts (`ota_flash.sh`) compatibles Linux/Windows

---

[v0.1.0]: https://github.com/alepape1/weather-station-ESP/releases/tag/v0.1.0
[v0.1.0-beta.3]: https://github.com/alepape1/weather-station-ESP/releases/tag/v0.1.0-beta.3
[v0.1.0-beta.2]: https://github.com/alepape1/weather-station-ESP/releases/tag/v0.1.0-beta.2
[v0.1.0-beta.1]: https://github.com/alepape1/weather-station-ESP/releases/tag/v0.1.0-beta.1
