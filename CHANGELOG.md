# Changelog — Aquantia Firmware (ESP32)

Todos los cambios notables de este proyecto se documentan en este archivo.
Formato basado en [Keep a Changelog](https://keepachangelog.com/es/1.1.0/).
Versiones siguiendo [Semantic Versioning](https://semver.org/lang/es/).

> **Compatibilidad backend:** Cada versión del firmware indica el backend compatible.
> Ver [app_meteo](https://github.com/alepape1/app_meteo) para las versiones del dashboard.

---

## [Unreleased]

---

## [0.2.0-beta.3] — 2026-05-06

**Backend compatible:** `v0.1.0` o superior · **Rama:** `feature/mqtt-alerts`

### Fixed
- **Relay lógica invertida (activo-HIGH)**

---

## [0.2.0-beta.2] — 2026-05-06

**Backend compatible:** `v0.1.0` o superior · **Rama:** `fix/stability-low-cost`

### Fixed
- **Race condition heap — `pipelineMode` / `pipelineScenario`**: convertidos de `String` a `char[16]`, eliminando las carreras de heap entre Core 0 y Core 1. Escrituras protegidas con `dataMutex`; la escritura del `LeakDetector` en Core 1 también queda bajo mutex. Lecturas siguen siendo eventual-consistent (máx. 1 ciclo).
- **LeakDetector falsos positivos burst/obstruction**: añadidos contadores de confirmación `_burst_hits` / `_obstr_hits` con constantes `BURST_CONFIRM = 2` y `OBSTR_CONFIRM = 2`. Ambas condiciones requieren 2 muestras consecutivas antes de disparar (análogo al `IDLE_CONFIRM = 3` ya existente para fuga en válvula cerrada).
- **BMP280 configuración ruidosa en PROFILE_METEO**: añadida llamada a `setSampling(MODE_NORMAL, SAMPLING_X2, SAMPLING_X16, FILTER_X16, STANDBY_MS_500)` tras `beginBMP280()` en el bloque de setup de METEO, igual que ya hacía PROFILE_AGROMETEO.
- **`pipeline_source` engañoso cuando presión cae al simulador**: el campo `pipeline_source` ahora reporta `"real_flow"` cuando el caudalímetro lee en modo real pero el XDB401 no entrega presión válida, en lugar de `"real"`. Evita confusión en dashboard y logs.
- **Presiones negativas del XDB401 rechazadas como inválidas**: el sentinel de "sin sensor" cambiado de `-1.0f` a `NAN`. El umbral de aceptación de `>= 0.0f` sustituido por `!isnan()`, permitiendo lecturas negativas reales (vacío, golpe de ariete, tubería sin presurizar) llegar a pantalla TFT y payload MQTT.

### Added
- **Diagnóstico LeakDetector en telemetría**: nuevos campos `leak_baseline_pressure`, `leak_baseline_flow` y `leak_warmup_progress` en el payload MQTT/HTTP para facilitar el ajuste de umbrales y depuración remota.

---

## [0.2.0-beta.1] — 2026-05-05

**Backend compatible:** `v0.1.0` o superior · **Rama:** `feat/firmware-stability-phase1`

Beta de la versión 0.2.0. Introduce el perfil AGROMETEO, reescritura completa del driver XDB401, detector de fugas con aprendizaje EMA, arquitectura lock-free dual-core y la herramienta de flash con compilación incremental.

### Herramientas
- **Flash Tool (`flasher_gui.py`) — compilación incremental real:**
  - Añadida constante `BUILD_CACHE_DIR` (`%TEMP%/aquantia_cache`) separada de `BUILD_DIR`.
  - `--build-cache-path BUILD_CACHE_DIR` pasado a `arduino-cli compile`: arduino-cli cachea los `.o` de librerías entre compilaciones; sólo recompila los archivos que cambiaron.
  - Eliminado el `shutil.rmtree(BUILD_DIR)` que se ejecutaba antes de cada compilación y destruía todos los objetos intermedios, forzando una compilación completa (~3-5 min) cada vez.
  - El botón "Limpiar build" limpia ahora ambas carpetas (`BUILD_DIR` y `BUILD_CACHE_DIR`).
  - La primera compilación sigue siendo completa; las siguientes (mismo perfil, misma placa) son incrementales y solo recompilan los archivos modificados (~20-40 s típico).



### Añadido
- **Muestreo rápido de caudalímetro y sensor de presión XDB401** (`PIPELINE_FAST_MS = 500 ms`):
  - Nuevo bloque en `loop()` (paso 2b) que llama a `readRealPipelineSensors()` cada 500 ms cuando `pipelineMode == "real"`, independiente del intervalo de telemetría (`telemetryIntervalMs`).
  - Actualiza `sim_pipeline_pressure` y `sim_pipeline_flow` para que la pantalla TFT refleje el estado real en cada ciclo de 1 s, en vez de esperar el ciclo de telemetría completo (que puede ser 20-30 s).
  - Permite detectar fugas y roturas de tubería con tiempo de reacción visual de ≤ 1 s sin incrementar la carga de envío al servidor.
  - `readRealPipelineSensors()` ya tiene debounce interno de 500 ms: si el bloque rápido cumple el intervalo antes que la telemetría normal, la telemetría devolverá el último valor cacheado, sin solapamiento ni doble conteo de pulsos.
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

### Cambiado (driver XDB401 y alertas de presión)
- **Driver XDB401 reemplazado por librería `pressure_sensor_i2c`** (`pressure_sensor_i2c.h` / `.cpp`):
  - Corregido el trigger I2C invertido: el sketch escribía `0x0A` en registro `0x30`; el protocolo correcto (datasheet) es escribir `0x30` en registro `0x0A`. Éste era el origen de las lecturas incorrectas de presión.
  - El polling activo del bit Sco (reg `0x30`, bit 3) reemplaza el delay fijo de 100ms — más robusto ante variaciones en el tiempo de conversión.
  - La conversión de temperatura usa la fórmula pura del datasheet (`raw/256`); las constantes `XDB401_ADDR_PRIMARY`, `XDB401_ADDR_ALT` y `XDB401_FULLSCALE_KPA` centralizadas en `pressure_sensor_i2c.h`.
  - La API interna del sketch (`xdb401_begin`, `xdb401_read`, `xdb401_readPressureBar`) se mantiene como wrappers finos sobre la librería.
- `Serial.begin(115200)` siempre activo (necesario para OTA y monitor de arranque); el resto de salida serie queda gateado por `DEBUG_MODE`.
- Lógica de botones en `loop()` reescrita con detección de flanco (rising/falling edge) para evitar cambios de vista repetidos por bounce.

### Añadido (XDB401, alertas y pantalla)
- **Umbrales de alerta de presión** como `#define` en la sección de constantes pipeline:
  - `PRESSURE_MIN_NORMAL 1.5 bar` — umbral bajo (ajustado para red pública de Lanzarote)
  - `PRESSURE_MAX_NORMAL 7.0 bar` — umbral de sobrepresión
  - `PRESSURE_DROP_ALERT 1.0 bar` — caída rápida (fuga activa)
  - `PRESSURE_HYDRO_TARGET 3.5 bar` y `PRESSURE_HYDRO_MARGIN 0.5 bar` — control del hidropresor
- **Temperatura del agua via XDB401**: campo `xdb401_temperature` en payload MQTT; pantalla TFT muestra `Taq: XX.X °C`.
- **Vista pipeline en pantalla TFT (`PROFILE_METEO`)**: pulsando cualquier botón se alterna entre vista meteorológica y vista de agua (presión/caudal). Badges `[SIM]` / `[OK]` por cada canal. Puntos de paginación en cabecera.
- **Modo debug configurable** desde Flash Tool GUI: casilla "🐛 Debug (serie)" inyecta `-DDEBUG_MODE=1`. Sin la casilla, toda la salida serie queda silenciada en producción. Flash Tool invalida la caché del binario al cambiar el flag y registra el modo en `build_meta.json`.

### Añadido (perfil AGROMETEO)
- **Nuevo perfil `PROFILE_AGROMETEO` (valor 3)** para placa CJMCU-14 (ESP32 + BH1750 + HDC1080 + BMP280).
- Soporte inline para **HDC1080** (temperatura y humedad, I2C 0x40) con protocolo propio, sin librería externa.
- Soporte para **BH1750** (iluminancia, librería `claws/BH1750`) en AGROMETEO.
- **Parámetros agrometeorológicos calculados** publicados en telemetría MQTT: `dew_point` (Magnus), `heat_index` (solo si T>27 °C y HR>40 %), `abs_humidity` (g/m³).
- `RELAY_COUNT=0` en AGROMETEO: sin relays; array dummy para que el código compile sin cambios.
- Reporte `DEBUG_MODE` actualizado para mostrar estado de HDC1080 y BH1750 en AGROMETEO.
- **LED_PROVISIONING**: triple parpadeo lento (300/300 ms × 3 + pausa 1,8 s) al entrar al portal SoftAP.

### Corregido
- Driver inicial XDB401 basado en protocolo incorrecto sustituido por el protocolo real del datasheet XGZP6847D.

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
