# Changelog — Aquantia Firmware (ESP32)

Todos los cambios notables de este proyecto se documentan en este archivo.
Formato basado en [Keep a Changelog](https://keepachangelog.com/es/1.1.0/).
Versiones siguiendo [Semantic Versioning](https://semver.org/lang/es/).

> **Compatibilidad backend:** Cada versión del firmware indica el backend compatible.
> Ver [app_meteo](https://github.com/alepape1/app_meteo) para las versiones del dashboard.

---

## [Unreleased — audit/security-2026-06]

**Backend compatible:** `v0.1.0` o superior · **Rama:** `audit/security-2026-06`

### Security

- **AUDIT PRE**: auditoría de seguridad exhaustiva (12 áreas, puntuación global 4.8/10) — ver [SECURITY.md](SECURITY.md)
- **AUDIT C1** ❌: `secrets - copia.h` con credenciales WiFi y MQTT trackeado en rama `develop` — pendiente limpieza con `git filter-repo` y rotación de credenciales
- **AUDIT C2** ❌: `ArduinoOTA` sin contraseña (`OTA_PASSWORD` no definido) ni verificación de integridad — pendiente migración a OTA autenticado
- **AUDIT C3** ❌: TLS en path celular SIM7000G con `authmode=0` (sin validación CA) en rama `feat/SIM_MODEM` — pendiente corrección a `authmode=1`
- **AUDIT C4** ❌: Flash Encryption y Secure Boot deshabilitados en `sdkconfig` — pendiente habilitación en proceso de fábrica
- **AUDIT C5** ⚠️: Contraseña SoftAP `"aquantia1"` igual en todos los dispositivos — pendiente derivación por serial del dispositivo
- **DOCS**: creados `SECURITY.md` (informe completo PRE + plantilla POST), `wiki/Seguridad.md` (arquitectura de seguridad)
- **DOCS**: actualizados `README.md` (sección Seguridad) y `wiki/Home.md` (enlace a Seguridad)

---

## [Unreleased — feat/per-profile-device-ids]

**Backend compatible:** `v0.1.0` o superior · **Rama:** `feat/per-profile-device-ids`

### Fixed

- **INA219 — voltaje y corriente nunca publicados**: el firmware calculaba correctamente
  `inaVbus` e `inaCurrent` en el `TelemetrySnapshot` pero solo serializaba `ina219_power_mw`
  en ambos payloads MQTT (WiFi completo y GSM slim). Las columnas `ina219_bus_voltage` y
  `ina219_current_ma` siempre llegaban `NULL` al backend.
  Ahora ambos campos se publican en todos los payloads:
  - Full WiFi (`PROFILE_IRRIGATION`): `ina219_bus_voltage` (2 decimales, V) + `ina219_current_ma` (1 decimal, mA)
  - GSM slim (`PROFILE_AQUA_SMART_REMOTE`): ídem

- **Flow counters — `flow_session_l`, `flow_irrig_l`, `flow_leak_l` ausentes en payload WiFi**:
  los tres contadores de caudal se calculaban correctamente en `sensor_read.h` y se almacenaban
  en el snapshot, pero solo `flow_total_l` se añadía al JSON del payload WiFi completo.
  Los tres campos nuevos llegan ahora al backend con precisión de 2 decimales (antes
  `flow_total_l` usaba 1 decimal — uniformizado a 2).

- **pH Halisense escala incorrecta**: el sensor devuelve el valor de pH ×100 (no ×10).
  La división en `halisense_sensor.h` pasó de `reg[3] / 10.0f` a `reg[3] / 100.0f`.
  Afecta a todos los perfiles que incluyen el sensor Halisense RS485.

- **Baud rate sensor suelo YIERYI**: el sensor estaba configurado a 9600 baud pero opera
  a 4800. `SoilSensor::begin()` por defecto pasó de `9600` a `4800`. Corregido también
  en la llamada de `ESP_monitor_server.ino` para `PROFILE_IRRIGATION`.

### Changed

- **`StaticJsonDocument` GSM slim** (`PROFILE_AQUA_SMART_REMOTE`): aumentado de `<512>` a
  `<640>` y `char buf[512]` a `char buf[640]` para acomodar los nuevos campos INA sin
  riesgo de truncación.

- **`char buf` WiFi completo**: aumentado de `char buf[1024]` a `char buf[1280]` por los
  campos `flow_session_l`, `flow_irrig_l`, `flow_leak_l` e INA añadidos. El
  `StaticJsonDocument<1024>` también aumentado a `<1280>`.

- **`soil_baud_changer.ino` repropuesto**: la herramienta pasó de cambiar 9600→4800 a
  cambiar 4800→9600 y reasignar la dirección Modbus de 0x01 a 0x03 en el mismo sketch
  (útil para reprovisionamiento de sensores reconfigurados).

---

## [Unreleased — refactor/modularize-ino]

**Backend compatible:** `v0.1.0` o superior · **Rama:** `refactor/modularize-ino`

### Changed
- **Modularización del firmware**: `ESP_monitor_server.ino` reducido de ~3 900 a ~1 700 líneas
  extrayendo bloques de funciones a siete ficheros `.h` independientes. Sin cambios de
  comportamiento en runtime — puramente organizativo.

  | Fichero nuevo | Contenido |
  |---------------|-----------|
  | `sensor_recovery.h` | Constantes de backoff, helpers BMP280, driver XDB401 completo, `temperatureSourceName` / `pressureSourceName` |
  | `pipeline_core.h` | `driftClamp`, `updateSimulatedValues`, `pipelineNoise`, `updatePipelineSimValues`, `readRealPipelineSensors`, `updatePipelineValues` |
  | `wind_sensor.h` | Buffers de media móvil ADC, `adcToWindSpeed/Deg`, `degToCompass`, acumuladores vectoriales de viento, `accumulateWindVector`, `calcAndResetWindVector` |
  | `http_client.h` | Capa HTTP completa para perfiles WiFi: `serverUseTls`, `serverBaseUrl`, `prepareSecureClient`, `httpGet`, `httpPost`, `syncPipelineScenario`, `checkRelayCommand`, `postDeviceInfo` |
  | `gsm_modem.h` | Init SIM7000G: `sim7000g_uploadCACert`, `prepareGsmTLSClient`, `sim7000g_powerOn` (BearSSL sobre TCP, 3 reintentos GPRS) |
  | `network_task.h` | `takeSnapshot` + `networkTask` completo (FreeRTOS Core 0): WiFi/GSM reconnect, MQTT connect/publish, alertas edge-triggered, OTA |
  | `sensor_read.h` | `readSlowSensors(now)` y `readSoilSensor(now)` — wrappers del ciclo de 20 s llamados desde `loop()` |

### Added
- **`free_heap` + `uptime_s` en payload GSM slim** (PROFILE_AQUA_SMART_REMOTE): los dos
  campos de diagnóstico de sistema se incluyen ahora en el payload reducido para celular
  (512 B), además del payload WiFi completo donde ya estaban presentes.
- **`sketch_size_kb` + `free_sketch_kb` en payload register** (`mqtt_helpers.h`): tamaño
  del sketch compilado y espacio libre de OTA (en KB) añadidos al mensaje de registro al
  arranque. Permite detectar en el dashboard si la partición OTA es suficiente para la
  versión en uso.

---

## [Unreleased]

**Backend compatible:** `v0.1.0` o superior · **Rama:** `feat/helissense-sensor`

### Added (PROFILE_IRRIGATION)
- **AHT20 sensor** (I2C 0x38): temperatura y humedad ambiente para el perfil IRRIGATION.
  Driver directo en `aht20_driver.h`, sin librería externa. En caso de fallo, el firmware
  hace fallback al BMP280 (temperatura) o a valores simulados.
  Campos nuevos en telemetría: `aht20_ok` (bool).
- **INA219 sensor** (I2C 0x40): voltaje de bus, corriente y potencia.
  Driver directo en `ina219_driver.h`, configurado para 32 V bus / ±2 A / 12-bit ADC,
  shunt 0.1 Ω. Campos nuevos en telemetría: `ina219_ok`, `ina219_bus_voltage` (V),
  `ina219_current_ma` (mA), `ina219_power_mw` (mW).
- **`temperature_source` para IRRIGATION**: devuelve `"AHT20"` cuando el sensor está activo,
  `"BMP280"` como fallback, o `"SIM"` si ningún sensor responde.

### Fixed
- **GPIO RX/TX invertidos en PROFILE_IRRIGATION** (Helissense RS485): el constructor
  `SoilSensor(Serial2, rxPin, txPin, dePin)` tenía los pines en orden incorrecto.
  Corregido a `SoilSensor(Serial2, 14, 13, 27)` → RX=GPIO14, TX=GPIO13, DE/RE=GPIO27.
  Actualizado también el comentario en el `.ino` y el PINOUT.md.
- **Boot log de perfil en AQUA_SMART_REMOTE**: el primer mensaje `[TEST] Perfil` en
  `setup()` no contemplaba `PROFILE_AQUA_SMART_REMOTE` y mostraba `IRRIGATION (4)`.
  Se corrigió el mapeo para imprimir `AQUA_SMART_REMOTE (4)`.
- **Cache inicial GSM en debug (`CSQ/GPRS`)**: tras `sim7000g_powerOn()` el estado
  publicado por Core 1 podía arrancar en `CSQ:0 / GPRS:DOWN` hasta que `NetworkTask`
  hiciera el primer polling. Ahora se inicializan `_gprsConnectedFlag` y `_simCsqCache`
  inmediatamente después del attach inicial para reflejar el estado real desde boot.
- **Autotest de boot GSM (`[TEST] GSM/GPRS`)**: en PROFILE_AQUA_SMART_REMOTE el
  mensaje de autodiagnóstico final usaba consulta directa al módem y podía mostrar
  `SIN CONEXION` de forma transitoria. Ahora usa cache thread-safe (`_gprsConnectedFlag`
  y `_simCsqCache`) para mantener coherencia con `[STATUS]`.
- **Caudalímetro en PROFILE_AQUA_SMART_REMOTE**: GPIO34 es solo entrada en la T-SIM7000 y
  no soporta pull-up interna. Se cambió la inicialización a `INPUT` para evitar el error
  `gpio_pullup_en(...)` al arrancar.
- **MQTT TLS en SIM7000G**: se añadió configuración SNI para `meteo.aquantialab.com` y un
  timeout más largo en el cliente TLS del módem para completar el handshake sobre enlaces
  GSM con latencia alta.
- **Diagnóstico TLS SIM7000 (ctx 0/1)**: la configuración `AT+CSSLCFG` ahora usa contexto
  SSL parametrizable y el firmware alterna automáticamente entre `sslCtx=1` y `sslCtx=0`
  cuando `mqttClient.connect()` falla con `CONNECTION_TIMEOUT (-4)`. Esto permite validar
  en campo diferencias de mapeo de contexto TLS entre revisiones de módem/firmware.
- **Arranque SIM7000 más robusto**: se añade espera activa de `SIM_READY` (hasta 30 s)
  antes del primer `gprsConnect()`, para reducir estados PDP inestables justo tras boot.
- **Recuperación PDP tras timeout MQTT**: cuando hay `CONNECTION_TIMEOUT (-4)` con GPRS
  activo, se ejecuta refresh del contexto PDP (`gprsDisconnect()` + `gprsConnect()`) antes
  del siguiente intento MQTT.

### Improved
- **Backoff global de recuperación de sensores**: cuando un sensor entra en fallo,
  el firmware evita reintentos continuos en cada ciclo. Tras 4 intentos de recuperación
  fallidos, aplica un enfriamiento de 5 minutos antes del siguiente intento.
  Aplicado a XDB401, BMP280, MCP9808, MicroPressure, HTU2x, HDC1080, BH1750,
  AHT20, INA219, TSL2584/APDS y Soil RS485.
- **WDT heartbeat logging**: la función `wdt_heartbeat(task, phase)` escribe el nombre de
  tarea y fase actual en RTC RAM antes de operaciones bloqueantes. Si el WDT dispara,
  la alerta `device_reboot` incluirá el contexto exacto del bloqueo para diagnóstico remoto.
- **Sondeo celular SIM7000G con cadencia controlada**: `NetworkTask` deja de ejecutar
  consultas AT/GPRS en cada iteración de 10 ms y pasa a un polling cada 2 s (o inmediato
  si GPRS cae). Reduce la interferencia sobre sockets TLS/MQTT y evita reconexiones espurias.
- **Diagnóstico MQTT celular sin consultas AT extra**: los logs de estado de red durante
  reconexión usan cache (`_gprsConnectedFlag`, `_simCsqCache`) en lugar de preguntar al
  módem en caliente. Reduce comandos AT durante `connect()` y caídas tras `loop()`.
- **Pre-limpieza de socket/buffer antes de `mqttConnect()` (AQUA_SMART_REMOTE)**:
  se cierra explícitamente el cliente TLS/TCP previo y se limpia el stream AT del módem
  antes de cada `CONNECT`, para evitar timeouts por `CONNACK` no leído en SIM7000G.
- **Cooldown de alertas de sensor** (`SENSOR_ALERT_COOLDOWN = 12 h`): evita que un sensor
  persistentemente muerto inunde el broker con alertas repetidas. Tras el primer disparo
  edge-triggered, la alerta se re-emite cada 12 horas mientras el fallo persista.
- **Potencia WiFi fija a 18.5 dBm** (`WIFI_POWER_18_5dBm`): aplicada en arranque y en cada
  reconexión para mejorar estabilidad en instalaciones con cobertura marginal.
- **Reconexión WiFi más robusta**: backoff exponencial (500 ms → 30 s), reset completo del
  stack WiFi cada 10 fallos, y `esp_restart()` automático tras 60 fallos consecutivos (~5 min).

---

## [Unreleased — feature/aqualeak-profile]

**Backend compatible:** `v0.1.0` o superior · **Rama:** `feature/aqualeak-profile`

### Changed
- **`PROFILE_AGROMETEO` renombrado a `PROFILE_AQUALEAK`** (`#define PROFILE_AQUALEAK 3`).
  El perfil mantiene el mismo número (3) para retrocompatibilidad con provisioning NVS.
  El string `device_profile` en el payload MQTT register y en los logs internos cambia de
  `"AGROMETEO"` a `"AQUALEAK"`. El Flash Tool GUI actualiza la etiqueta del perfil.

### Added
- **Relay para AQUALEAK**: `RELAY_COUNT = 1`, GPIO `RELAY_PIN` (GPIO26, Wemos D1 Mini ESP32),
  activo-HIGH. Idéntica configuración a PROFILE_METEO. Permite controlar una electroválvula
  de corte directamente desde el dispositivo AquaLeak.
- **`flow_session_l` en telemetría MQTT**: litros acumulados desde la última apertura del
  relay (GPIO26). Se resetea automáticamente al transicionar `relay OFF → ON` (nueva sesión
  de riego). Se publica en el campo `flow_session_l` del payload MQTT telemetría.
- **`flowSessionReset()`**: función pública para reiniciar el contador de sesión; también
  llamada internamente al abrir el relay.

### Improved
- **Precisión del caudalímetro**: timer del caudal migrado de `millis()` (±1 ms) a
  `micros()` (±1 µs). Reduce el error de cuantización del intervalo `dt` en un factor
  ×1000, especialmente relevante a caudales bajos (<1 L/min) donde el período llega a
  varios segundos. El umbral mínimo de intervalo pasa de 500 ms a 500 000 µs (idéntico
  valor, solo cambio de unidad).
- **Lectura atómica de contadores de pulsos**: reemplazado `noInterrupts()/interrupts()`
  por `portENTER_CRITICAL(&_flowMux)/portEXIT_CRITICAL(&_flowMux)` (spinlock IDF).
  Correcto en contexto dual-core: el ISR se ejecuta en Core 1, `readRealPipelineSensors`
  también en Core 1 — el spinlock garantiza exclusión sin deshabilitar interrupciones
  globalmente del sistema.
- **Spinlock `_flowMux`** declarado junto a los contadores ISR para mantener cohesión.

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
