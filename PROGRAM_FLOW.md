# Flujo del programa — Firmware Aquantia ESP32

Este documento describe el recorrido secuencial del firmware desde el arranque hasta el estado estacionario, con el detalle de qué ocurre en cada core, en qué orden y bajo qué condiciones.

---

## Índice

- [Arranque — `setup()`](#arranque--setup)
- [Core 1 — `loop()`](#core-1--loop)
- [Core 0 — `networkTask()`](#core-0--networktask)
- [Sincronización entre cores](#sincronización-entre-cores)
- [Máquinas de estado](#máquinas-de-estado)
- [Rutas de error y recuperación](#rutas-de-error-y-recuperación)

---

## Arranque — `setup()`

`setup()` corre íntegramente en **Core 1** antes de que el scheduler de FreeRTOS arranque. Se ejecuta una sola vez y en secuencia estricta:

```
setup()
│
├─ 1. Serial.begin(115200) — consola de debug
├─ 2. setCpuFrequencyMhz(160) — ahorro energético (~20% vs 240 MHz)
├─ 3. esp_reset_reason() → g_rebootReason — capturar causa del ultimo reset ANTES de que nada la sobreescriba
│
├─ 4. Wire.begin(SDA=21, SCL=22)
│       Wire.setClock(50 000 Hz) — velocidad reducida por cable ~1m del XDB401
│       Wire.setTimeOut(200ms)   — evita bloqueo indefinido en bus colgado
│
├─ 5. [HAS_DISPLAY] TFT init
│       pinMode(TFT_BL=4, OUTPUT) + HIGH — encender backlight
│       tft.init() + tft.setRotation(1)
│       spr.createSprite(240, 135) — reservar buffer en RAM (doble buffer)
│       pinMode(BTN_LEFT=0, INPUT_PULLUP)
│       pinMode(BTN_RIGHT=35, INPUT)
│       provisioning_register_ap_display(drawAPScreen) — callback para portal
│       drawBootScreen("Iniciando...")
│
├─ 6. Provisioning (credenciales WiFi)
│       ┌─ DEV_MODE activo ──────────────────────────────────────────────────┐
│       │  Usa WIFI_SSID / WIFI_PASSWORD de secrets.h directamente.         │
│       │  Salta todo el bloque de NVS.                                      │
│       └───────────────────────────────────────────────────────────────────┘
│       ┌─ PROD (sin DEV_MODE) ──────────────────────────────────────────────┐
│       │  provisioning_check_factory_reset()  — botón mantenido en boot?    │
│       │  provisioning_load()                 — leer NVS (ssid + password)  │
│       │  Si NVS vacío:                                                      │
│       │    setLedState(LED_PROVISIONING)     — triple blink lento           │
│       │    [HAS_DISPLAY] drawAPScreen(SSID, serial)                         │
│       │    provisioning_start_ap()  ← BLOQUEA hasta que el usuario         │
│       │                               configure el WiFi por el portal       │
│       │  ssid = prov_ssid, password = prov_password                         │
│       └───────────────────────────────────────────────────────────────────┘
│
├─ 7. [PROFILE_AQUALEAK] Qwiic Power Switch (PCA9536 en 0x41)
│       qwiic_ps.begin(Wire) → qwiic_ps.powerOn()
│       delay(50) — estabilizar alimentación del bus de sensores
│
├─ 8. Escáner I2C — detectar dispositivos (1–127), solo log
│
├─ 9. GPIO periféricos
│       LED onboard: pinMode + LOW
│       Relays: pinMode(OUTPUT) + LOW por cada relay (todos en OFF, seguro)
│       [SOIL_PIN] analogSetPinAttenuation(11dB)
│       [FLOW_PIN] INPUT_PULLUP + attachInterrupt(FALLING, flowPulseISR)
│               _flowLastCalcUs = micros()
│
├─ 10. Init sensores I2C (en función del perfil)
│
│    PROFILE_METEO:
│      MCP9808.begin(0x19) → setResolution(3)
│      BMP280.begin(0x76 o 0x77 autodetectado) → setSampling(NORMAL, X2T, X16P, FILTER_X16, 500ms)
│      barometer.begin() [MicroPressure 0x18]
│      → Si MicroPressure falla: intentar usar BMP280 como barómetro
│      → Si ambos fallan: modo simulación
│
│    PROFILE_AQUALEAK:
│      barometer.begin() [MicroPressure 0x18]
│      BMP280.begin() → setSampling(NORMAL, X2T, X16P, FILTER_X16, 500ms)
│      hdc1080_init() [HDC1080 0x40] — temperatura + humedad primaria
│      bh1750.begin(CONTINUOUS_HIGH_RES_MODE) → delay(180ms) primera conversión
│
│    Todos los perfiles:
│      htu_begin() [HTU21D 0x40] — solo METEO e IRRIGATION
│      tsl_begin() [TSL2584/APDS-9930 0x39 autodetectado] — solo si no AQUALEAK
│      xdb401_begin() → 3 intentos con bus recovery entre cada uno
│        Si OK: xdb401_ok = true, pipelineMode = "real"
│        Si falla: xdb401_ok = false, pipelineMode = "sim"
│
│    PROFILE_METEO adicional:
│      DHT.setup(GPIO15, DHTesp::DHT11)
│      soilSensor (SoilSensor RS485, Serial2, RX=13, TX=17, DE/RE=27)
│      Precalentar filtros ADC: 10 lecturas YL-69 (soilValues[]) + 10 lecturas anemómetro
│
├─ 11. [HAS_DISPLAY] Precalentar filtro anemómetro
│       10 lecturas analogRead(ANEMOMETER_PIN=36) → anemometerValues[]
│
├─ 12. leakDetector.begin(IRRIG_SPRINKLER) — estado inicial warm-up
│
├─ 13. FreeRTOS — crear primitivas de sincronización
│       dataMutex = xSemaphoreCreateBinary() + xSemaphoreGive()
│       telemetryQueue = xQueueCreate(1, sizeof(TelemetrySnapshot))
│
├─ 14. [USE_MQTT] WiFi.begin(ssid, password)
│           configTime(0, 0, "pool.ntp.org", "time.cloudflare.com") — NTP
│           Esperar WL_CONNECTED (hasta timeout)
│
│    [sin USE_MQTT / HTTP]
│           WiFi.begin(ssid, password) + bucle de espera
│
├─ 15. ArduinoOTA.begin() — habilitar actualizaciones OTA
│         onStart: isUpdatingOTA = true, relays OFF
│         onEnd:   isUpdatingOTA = false
│
├─ 16. Lanzar networkTask en Core 0
│       xTaskCreatePinnedToCore(networkTask, "networkTask", 8192B stack, prioridad 2, Core 0)
│
└─ 17. setup() termina → FreeRTOS cede control a loop() en Core 1
```

> **Nota OTA en setup:** los callbacks de ArduinoOTA se registran aquí pero `ArduinoOTA.handle()` se llama dentro de `networkTask` (Core 0) en cada iteración del bucle de red.

---

## Core 1 — `loop()`

`loop()` corre en **Core 1** (el mismo que `setup()`). No gestiona red ni OTA — solo sensores, display y producción de datos para Core 0.

El bucle no tiene `delay()` — avanza de forma no-bloqueante comparando `millis()` contra marcas de tiempo para cada temporizador independiente.

### Secuencia de cada iteración

```
loop() — se ejecuta continuamente sin delay
│
├─ A. ledTick()
│       Avanza la máquina de estados del LED (ver sección Máquinas de estado).
│       Sin bloqueo, sin delay — calcula el siguiente flanco ON/OFF por millis().
│
├─ B. [HAS_DISPLAY] Gestión de botones
│       Leer GPIO0 (BTN_LEFT) + GPIO35 (BTN_RIGHT) — nivel bajo = pulsado.
│       Si cualquier botón está pulsado:
│         Si pantalla OFF → encender backlight (HIGH en TFT_BL), displayOn = true
│         Actualizar lastActivityTime = now
│       Si flanco descendente detectado (curBtn LOW y prevBtn HIGH):
│         Si pantalla ya encendida y han pasado ≥400ms desde último cambio:
│           displayView = (displayView + 1) % 4   [0=Meteo, 1=Pipeline, 2=Info, 3=Suelo]
│       Si displayTimeoutMs > 0 y (now − lastActivityTime ≥ displayTimeoutMs):
│         Apagar backlight (LOW en TFT_BL), displayOn = false
│
├─ C. Lectura viento — cada 100ms (WIND_MS)
│       [HAS_DISPLAY / METEO]
│         rawAne = analogRead(ANEMOMETER_PIN=36)
│         windSpeed = (rawAne / 4096.0) * (ADC_VOLTAGE_REF / ADC_VOLTAGE_REF) * 30 m/s
│         windSpeedFiltered = filteredADC(rawAne)  [media circular 10 muestras]
│         rawVane = analogRead(VANE_PIN=37)
│         currentWindDirDeg = adcToWindDeg(rawVane)  [cuantiza a 8 rumbos × 45°]
│       [Sin HAS_DISPLAY]
│         windSpeed = windSpeedFiltered = sim_windSpeed
│         currentWindDirDeg = sim_windDir
│       → accumulateWindVector(currentWindDirDeg)
│           Añade (cos(deg), sin(deg)) al acumulador con portENTER_CRITICAL(windMux)
│           windSumX += cos; windSumY += sin; windSampleCount++
│
├─ D. Sensores I2C lentos — cada telemetryIntervalMs (default 20s)
│   │   [Mismo ciclo que construye el TelemetrySnapshot]
│   │
│   ├─ D1. [PROFILE_METEO] BMP280
│   │       Si !bmp_ok → intentar reinit (recovery automático)
│   │       Leer temperatura y presión; validar rango
│   │       Si falla: bmp_ok = false (alerta MQTT en Core 0 detectará el cambio)
│   │
│   ├─ D2. [PROFILE_METEO] MCP9808 (temperatura exterior)
│   │       Si !mcp_ok → intentar reinit
│   │       tempsensor.wake() → readTempC() → tempsensor.shutdown_wake(1)
│   │       Prioridad: MCP9808 > BMP280 > simulación
│   │
│   ├─ D3. [PROFILE_METEO] MicroPressure (barómetro)
│   │       Si !micropressure_ok → intentar reinit
│   │       barometer.readPressure(KPA), validar 50–120 kPa
│   │       Prioridad: MicroPressure > BMP280 > simulación
│   │
│   ├─ D4. [no PROFILE_AQUALEAK] HTU2x (temperatura interior + humedad)
│   │       Si !htu_ok → intentar reinit
│   │       htu_readTemp() + htu_readHumidity()
│   │       Si falla: temperatureDHT = sim_tempDHT, humidity = sim_humidity
│   │
│   ├─ D5. [PROFILE_METEO] DHT11
│   │       dht.getTempAndHumidity()
│   │       Si ERROR_NONE: dht_ok = true, temperatureDHT11, humidityDHT11
│   │       Si falla: valores simulados
│   │
│   ├─ D6. [PROFILE_AQUALEAK] HDC1080 + BMP280 + BH1750 + parámetros calculados
│   │       Reiniciar solo los sensores que fallaron en el ciclo anterior
│   │       HDC1080: readTemp() + readHum() → temperatureMCP, humidity
│   │       MicroPressure + BMP280: fallback encadenado
│   │       BH1750: readLightLevel(); si < 0: un reintento tras 50ms
│   │       Parámetros derivados (si T y H válidos):
│   │         agroDewPoint  = Magnus(T, H)
│   │         agroAbsHum   = Humedad absoluta (g/m³)
│   │         agroHeatIndex = fórmula empírica (solo si T > 27°C y H > 40%)
│   │
│   ├─ D7. [no PROFILE_AQUALEAK] TSL2584 / APDS-9930 (luz)
│   │       tsl_readLux(); si falla: lightLevel = sim_light
│   │
│   ├─ D8. [PROFILE_METEO sin Helissense] YL-69 ADC (humedad suelo fallback)
│   │       Solo si halisenseData.ok == false
│   │       raw = analogRead(SOIL_PIN=33)
│   │       filtRaw = filteredSoilADC(raw)  [media circular 10 muestras]
│   │       soilMoisture = (SOIL_RAW_DRY − filtRaw) / (SOIL_RAW_DRY − SOIL_RAW_WET) × 100%
│   │
│   ├─ D9. updateSimulatedValues()
│   │       Avanza todos los valores simulados con drift aleatorio acotado (driftClamp)
│   │       Solo afecta a los que no tienen sensor real disponible
│   │
│   ├─ D10. [pipeline_mode == "sim"] updatePipelineValues()
│   │        Calcula presión y caudal del simulador determinista (tres ondas sinusoidales)
│   │        según pipelineScenario (normal / leak / burst / obstruction)
│   │
│   └─ D11. Construir TelemetrySnapshot y publicar en telemetryQueue
│             snap.tempMCP, pressure, tempDHT, humidity...
│             snap.windSpeed, windDir, windSpeedFilt
│             snap.avgWindDir = calcAndResetWindVector()
│               → Consume el acumulador vectorial (portENTER_CRITICAL) → atan2(Y,X) → deg
│               → Resetea windSumX = windSumY = 0, windSampleCount = 0
│             snap.pipePressure = sim_pipeline_pressure
│             snap.pipeFlow     = sim_pipeline_flow
│             [FLOW_PIN] Con portENTER_CRITICAL:
│               snap.flowTotalL   = _flowPulseTotal   / K
│               snap.flowSessionL = (_flowPulseTotal − _flowSessionBase) / K
│               snap.flowIrrigL   = _flowIrrigPulses  / K
│               snap.flowLeakL    = _flowLeakPulses    / K
│             snap.relayMask: leer relayActive[] con dataMutex (5ms timeout)
│             snap.soilIrrigMode = anyRelayActive() OR en ventana post-riego
│             xQueueOverwrite(telemetryQueue, &snap)  ← sin bloqueo
│
├─ E. Sensor suelo RS485 — muestreo adaptativo (bloque 2c)
│       Detectar flanco OFF del relay:
│         prevRelayOn → relayOn: si OFF→ON: soilPostIrrigEndMs = 0 (reset)
│                                si ON→OFF: soilPostIrrigEndMs = now
│       Calcular soilInterval:
│         relay ON     → SOIL_FAST_MS  (3 000 ms)
│         post-riego   → SOIL_FAST_MS  (3 000 ms) durante 120 s
│         reposo       → SOIL_SLOW_MS (20 000 ms)
│       Si (now − lastSoilReadMs ≥ soilInterval):
│         soilSensor.readAllVariables() → protocolo Modbus RTU a 4800 baud
│           Si OK: halisenseData.ok = true
│                  moisture (÷10), temperature (÷10), EC (µS/cm→dS/m ÷1000, TDS ppm ×0.5)
│                  pH (÷10), N, P, K (directo)
│                  soilMoisture = halisenseData.moisture
│           Si falla: halisenseData.ok = false → soilMoisture desde YL-69 en ciclo D8
│
├─ F. Pipeline rápido — cada 200ms (PIPELINE_FAST_MS)
│       Solo si pipelineMode == "real"
│       readRealPipelineSensors(pressureBar, flowLpm):
│         Caudalímetro:
│           dt = micros() − _flowLastCalcUs
│           Si dt < 500 000µs (< 0.5s): reutilizar _flowLpm (sin recalcular)
│           Si dt ≥ 500 000µs:
│             noInterrupts() → pulses = _flowPulseCount; _flowPulseCount = 0; interrupts()
│             _flowLpm = (pulses × 60) / (dt_s × K_FACTOR)  [µs para precisión]
│             Si relay ON: _flowIrrigPulses += pulses
│             Si relay OFF: _flowLeakPulses += pulses
│             g_flowSessionL = (_flowPulseTotal − _flowSessionBase) / K
│           XDB401 (cada iteración, independiente del dt del caudal):
│             Si !xdb401_ok y millis() ≥ xdb401_retry_at → xdb401_begin()
│             Si xdb401_ok → xdb401_read(pressureBar, xdb401Temperature)
│               Si falla ≥ XDB401_MAX_FAILURES (8):
│                 xdb401_ok = false, xdb401_retry_at = now + 15 000ms
│         Resultado:
│           sim_pipeline_flow = max(0, flowLpm)
│           Si pressureBar no es NAN:
│             sim_pipeline_pressure = pressureBar; pipelineSource = "real"
│           Si pressureBar es NAN (sin XDB401):
│             updatePipelineSimValues() para presión; restaurar caudal real
│             pipelineSource = "real_flow"
│         leakDetector.update(pressure, flow, anyRelayActive())
│           → EMA warmup o detección activa (ver Máquinas de estado)
│         strlcpy(pipelineScenario, leakDetector.scenario())
│
└─ G. [HAS_DISPLAY] Refresco pantalla — cada 1s (SCREEN_MS)
        Si displayOn:
          view 0 → drawScreen()       [Meteo: T, H, P, viento, luz]
          view 1 → drawPipelineScreen() [presión, caudal, temp agua, modo]
          view 2 → drawInfoScreen()    [firmware, IP, MAC, uptime, conexión]
          view 3 → drawSueloScreen()   [humedad, T suelo, pH, NPK]
        Doble buffer: spr.pushSprite(0,0) al final → sin parpadeo
```

---

## Core 0 — `networkTask()`

`networkTask()` corre en **Core 0** con prioridad 2. Arranca inmediatamente después de que `setup()` lo crea con `xTaskCreatePinnedToCore`. El watchdog (TWDT) está configurado a 30s con `trigger_panic = true`.

### Inicialización de la tarea (una sola vez)

```
networkTask() — inicialización
│
├─ Registrar en TWDT: esp_task_wdt_add(NULL)
├─ [USE_MQTT] Configurar cliente MQTT:
│    Si mqtt_port == 8883: mqttTLSClient + ISRG Root X1 cert (PROGMEM)
│    Si otro puerto: mqttTCPClient (sin TLS)
│    mqttClient.setServer(mqtt_server, mqtt_port)
│    mqttClient.setCallback(mqttCallback)
│    mqttClient.setBufferSize(1280)  — payload hasta 1280 bytes
└─ Entrar en bucle infinito
```

### Bucle principal

```
networkTask() — bucle for(;;)
│
├─ esp_task_wdt_reset()  ← reset watchdog (cada ~10ms)
├─ ArduinoOTA.handle()  ← primera instrucción, siempre ejecutada
│
├─ Si isUpdatingOTA == true:
│     vTaskDelay(10ms) → continue
│     [Core 1 sigue leyendo sensores y refrescando pantalla]
│
├─ Si WiFi desconectado:
│     setLedState(LED_WIFI_CONNECTING)
│     wifiStableSince = 0
│     WiFi.reconnect()
│     vTaskDelay(wifiRetryDelayMs)  [backoff exponencial 500ms → 5s]
│     → continue
│
├─ [WiFi conectado ≥ 10s] Resetear backoff WiFi a 500ms
│
├─ [USE_MQTT] Si reloj no sincronizado y han pasado ≥60s desde último intento:
│     configTime(0, 0, "pool.ntp.org", "time.cloudflare.com")
│     lastNtpRetry = now
│
├─ Sincronización de config (cada configSyncIntervalMs, default 20s)
│     GET /api/pipeline/config → JSON con:
│       scenario, mode, telemetry_interval_s, config_sync_interval_s,
│       display_timeout_s, irrigation_type, reset_flow_counters
│     Si reset_flow_counters == true:
│       portENTER_CRITICAL → _flowIrrigPulses = _flowLeakPulses = 0
│                             _flowSessionBase = _flowPulseTotal  ← sesión también
│       portEXIT_CRITICAL
│       POST /api/flow/reset-ack?mac=<MAC>
│     Actualizar bajo dataMutex: pipelineScenario, pipelineMode, irrigationType
│     Si irrigationType cambió: leakDetector.begin(nuevoTipo) ← reset baseline
│     Actualizar sin mutex: telemetryIntervalMs, configSyncIntervalMs, displayTimeoutMs
│
├─ ── MODO MQTT ─────────────────────────────────────────────────────────────────
│   │
│   ├─ Si !mqttClient.connected():
│   │     setLedState(LED_MQTT_CONNECTING)
│   │     [puerto 8883] prepareSecureClient(mqttTLSClient, 10s)
│   │     esp_task_wdt_reset()
│   │     mqttConnect():
│   │       client_id = "aquantia-{MAC sin :}"
│   │       mqttClient.connect(client_id, user, pass)
│   │       Si conectado: subscribe "aquantia/<finca_id>/cmd" QoS 1
│   │     Si falla:
│   │       vTaskDelay(mqttRetryDelayMs)  [backoff 2s → 15s, +2s/intento]
│   │       → continue
│   │     setLedState(LED_IDLE), mqttRetryDelayMs = 2000
│   │     Si primera conexión (deviceInfoSent == false):
│   │       mqttPublishRegister() → topic register — MAC, IP, chip, relay_count, fw, perfil
│   │       deviceInfoSent = true
│   │       Guardar g_lastConnectStr (timestamp NTP formateado)
│   │       mqttPublishAlert("device_reboot", "info", "Dispositivo reiniciado: {g_rebootReason}")
│   │     Si reconexión posterior:
│   │       mqttPublishAlert("mqtt_reconnect", "info", "Dispositivo reconectado al broker MQTT")
│   │
│   ├─ mqttClient.loop()  ← procesa mensajes entrantes → mqttCallback()
│   │
│   ├─ Alarmas edge-triggered (comparan estado actual vs última vez publicado):
│   │     Pipeline: pipelineScenario cambió →
│   │       "leak"        → alert("leak", warning)
│   │       "burst"       → alert("burst", critical)
│   │       "obstruction" → alert("obstruction", warning)
│   │       "normal"      → alert("pipeline_ok", info)
│   │     Sensores: para cada sensor monitorizado,
│   │       si ok → !ok: alert("sensor_failure", warning, "{nombre} sin respuesta")
│   │       si !ok → ok:  alert("sensor_ok", info, "{nombre} recuperado")
│   │       Sensores monitorizados: XDB401, MCP9808 (METEO), BMP280 (METEO),
│   │         MicroPressure (METEO+AQUALEAK), HTU2x (no AQUALEAK),
│   │         HDC1080 (AQUALEAK), BH1750 (AQUALEAK)
│   │     Heap: si ESP.getFreeHeap() < 30 000 B y no había alerta previa:
│   │       alert("low_heap", warning, "Heap libre < 30 KB")
│   │
│   └─ Telemetría (cada telemetryIntervalMs):
│         takeSnapshot() → xQueuePeek(telemetryQueue, &_netSnap, 0)
│         También copia heap, rssi, uptime directamente (valores 32-bit, seguro)
│         Serializar JSON con roundf(x × 100) / 100 (2 dec) o × 10 / 10 (1 dec)
│         mqttClient.publish("aquantia/<finca_id>/telemetry", buf, false)
│         setLedState(ok ? LED_TX_OK : LED_TX_ERROR)
│
├─ ── MODO HTTP LEGACY ──────────────────────────────────────────────────────────
│   │
│   ├─ Si !deviceInfoSent: POST /api/device_info (JSON con chip info, MAC, IP)
│   │
│   ├─ Cada 2s (RELAY_MS): checkRelayCommand()
│   │     GET /api/relay/command?mac=<MAC>
│   │     Parsear bitmask numérico de la respuesta
│   │     Para cada bit: si estado cambia → digitalWrite(RELAY_PIN, HIGH/LOW)
│   │     Si relay OFF→ON: flowSessionReset()
│   │     Si hubo cambio: POST /api/relay/ack con bitmask actual
│   │
│   └─ Cada telemetryIntervalMs: takeSnapshot() → POST /send_message
│         CSV 19 campos: tempMCP, pressure, tempDHT, humidity, windSpeed,
│         windDirection, windSpeedFilt, avgWindDir, light, tempDHT11, humDHT11,
│         rssi, heap, uptime, relayMask, pipePressure, pipeFlow, soil, flowTotalL
│
└─ vTaskDelay(10ms)  ← yield al scheduler, permite que otras tareas e IDLE corran
```

### `mqttCallback()` — comandos entrantes

Se dispara desde `mqttClient.loop()` cuando llega un mensaje en `aquantia/<finca_id>/cmd`:

```
mqttCallback(topic, payload, length)
│
├─ deserializeJson(payload)
├─ Si campo "mac" presente y no coincide con self → ignorar
├─ Si campo "relay" + "state":
│     Si state y !wasActive (OFF→ON): flowSessionReset()
│     relayActive[relay] = state
│     digitalWrite(RELAY_PINS[relay], state ? HIGH : LOW)
│     setLedState(anyRelayActive() ? LED_RELAY_ON : LED_IDLE)
│
├─ Bajo dataMutex:
│     "pipeline_scenario" → strlcpy(pipelineScenario, ...)
│     "pipeline_mode"     → strlcpy(pipelineMode, ...)
│     "irrigation_type"   → irrigationType = ...; leakDetector.begin(nuevoTipo)
│
└─ Sin mutex (tipos básicos):
      "telemetry_interval_s"  → telemetryIntervalMs [5–3600 s]
      "config_sync_interval_s" → configSyncIntervalMs [5–3600 s]
      "display_timeout_s"     → displayTimeoutMs [0–3600 s] (solo HAS_DISPLAY)
```

---

## Sincronización entre cores

| Primitiva | Tipo | Quién escribe | Quién lee | Qué protege |
|-----------|------|---------------|-----------|-------------|
| `telemetryQueue` | FreeRTOS Queue tamaño 1 | Core 1 (`xQueueOverwrite`) | Core 0 (`xQueuePeek`) | `TelemetrySnapshot` completo |
| `dataMutex` | Semáforo binario | Core 0 (MQTT callback, syncPipeline) | Core 1 (toma 5ms para relayMask) | `relayActive[]`, `pipelineMode`, `pipelineScenario`, `irrigationType` |
| `windMux` | `portMUX_TYPE` spinlock | Core 1 (accumulateWindVector, calcAndResetWindVector) | Core 1 (mismo core) | `windSumX`, `windSumY`, `windSampleCount` |
| `_flowMux` | `portMUX_TYPE` spinlock | ISR (`flowPulseISR`), Core 1 (flowSessionReset, readRealPipelineSensors) | Core 1 (snapshot) | `_flowPulseCount`, `_flowPulseTotal`, `_flowSessionBase`, `_flowIrrigPulses`, `_flowLeakPulses` |
| `char[16]` | Variables estáticas | Core 0 (bajo `dataMutex`) | Core 1 (eventual-consistent) | `pipelineMode`, `pipelineScenario` — evita races de heap de `String` |
| `isUpdatingOTA` | `volatile bool` | Core 0 (callback onStart/onEnd) | Core 0 (bucle principal) | Pausar telemetría durante flash |

### Flujo de datos sensores → backend

```
ISR (Core cualquiera) ──_flowPulseCount++──────────────────────────────────┐
                        _flowPulseTotal++                                   │
                                                                            ▼
Core 1 (loop, 100ms)                                               _flowMux (noInterrupts)
  analogRead → windSpeed/dir                                                │
  accumulateWindVector → windMux                                            │
                                                                            │
Core 1 (loop, 200ms, pipeline_mode=real)                                   │
  readRealPipelineSensors: consume _flowPulseCount, lee XDB401 ←───────────┘
  leakDetector.update(pressure, flow, relay)
  strlcpy(pipelineScenario, leakDetector.scenario())

Core 1 (loop, 20s)
  Lee sensores I2C lentos (MCP9808, BMP280, HTU2x, DHT11, luz, suelo)
  calcAndResetWindVector() → windMux
  Lee contadores flow ← _flowMux
  Lee relayActive[] ← dataMutex (5ms)
  xQueueOverwrite(telemetryQueue, &snap)  ←── SIN BLOQUEO
                                                    │
                                                    ▼
Core 0 (networkTask, 20s)
  xQueuePeek(telemetryQueue, &_netSnap, 0)  ←── SIN BLOQUEO (copia)
  + heap/rssi/uptime frescos
  → JSON → mqttClient.publish(telemetry)
```

---

## Máquinas de estado

### LED (`led_control.h`)

`ledTick()` se llama desde `loop()` en cada iteración. Gestiona el LED onboard (GPIO 23 en IRRIGATION, GPIO 2 en AQUALEAK; `-1` en METEO que no tiene LED onboard) con parpadeos no-bloqueantes:

| Estado (`LedState`) | Comportamiento | Cuándo |
|---------------------|----------------|--------|
| `LED_PROVISIONING` | Triple blink lento (300ms ON, 300ms OFF × 3, pausa 1s) | Portal SoftAP activo |
| `LED_WIFI_CONNECTING` | Blink rápido (100ms ON, 100ms OFF) | Intentando conectar a WiFi |
| `LED_MQTT_CONNECTING` | Blink medio (250ms ON, 250ms OFF) | Intentando conectar al broker |
| `LED_IDLE` | ON fijo | WiFi + MQTT conectados, sin actividad |
| `LED_TX_OK` | Pulso breve ON (200ms) → IDLE | Telemetría publicada con éxito |
| `LED_TX_ERROR` | Doble blink rápido → IDLE | Error de publicación |
| `LED_RELAY_ON` | ON fijo (modo relay activo) | Algún relay está activo |

### LeakDetector (EMA + clasificación)

`leakDetector.update(pressure, flow, valveOpen)` se llama en Core 1 cada 200ms (modo real).

```
Estado: WARMUP (leakDetector.hasBaseline() == false)
│
│  Recibir muestra con válvula abierta:
│    baselinePressure = EMA(pressure, α=0.05)
│    baselineFlow     = EMA(flow, α=0.05)
│    warmupProgress++
│  Si warmupProgress ≥ 20 → Estado: ACTIVE
│
Estado: ACTIVE (hasBaseline() == true)
│
├─ Válvula cerrada (valveOpen == false):
│     Si flow > leak_idle_threshold_lpm durante IDLE_CONFIRM (3) muestras consecutivas:
│       scenario = "leak"
│     Si no: scenario = "normal", actualizar EMA con muestra actual
│
└─ Válvula abierta (valveOpen == true):
      Si (baselinePressure − pressure) / baselinePressure ≥ burst_pressure_drop_pct
         durante BURST_CONFIRM (2) muestras:
        scenario = "burst"
      Si flow > baselineFlow × (1 + leak_on_deviation_pct)
         durante BURST_CONFIRM (2) muestras:
        scenario = "leak"
      Si (baselineFlow − flow) / baselineFlow ≥ obstruction_flow_drop_pct
         durante OBSTR_CONFIRM (2) muestras:
        scenario = "obstruction"
      Si no: scenario = "normal", actualizar EMA

Transición de irrigation_type:
  leakDetector.begin(nuevoTipo) → estado = WARMUP, warmupProgress = 0
  (puede venir de Core 0 por MQTT o syncPipelineScenario)
```

### Pipeline simulador

`updatePipelineSimValues()` corre en Core 1 cuando `pipeline_mode = "sim"`. Usa tres ondas sinusoidales en función de `millis()` para producir ruido reproducible:

```
pipelineNoise(t, ch) = sin(t×7.3 + ch×1.7)×0.55 + sin(t×13.1 + ch×3.2)×0.30 + sin(t×31.7 + ch×5.1)×0.15

pipelineScenario == "normal":
  válvula abierta:  P = PIPELINE_DYNAMIC_P + noise × PIPELINE_NOISE_P
                    Q = PIPELINE_NOMINAL_Q + noise × PIPELINE_NOISE_Q
  válvula cerrada:  P = PIPELINE_STATIC_P  + noise × PIPELINE_NOISE_P
                    Q = |noise| × 0.05

pipelineScenario == "leak":
  válvula abierta:  P = PIPELINE_DYNAMIC_P − 0.18 + noise
                    Q = PIPELINE_NOMINAL_Q − 0.45 + noise
  válvula cerrada:  P = PIPELINE_STATIC_P  − 0.10 + noise
                    Q = 0.28 + |noise| × 0.35

pipelineScenario == "burst":
  P = 0.25 + noise × 0.4  (muy baja)
  Q = válvula abierta: NOMINAL × 0.08   (poca presión, poco caudal)

pipelineScenario == "obstruction":
  válvula abierta:  P = PIPELINE_STATIC_P (no cae — obstruida)
                    Q = |noise| × 0.04   (casi cero)
  válvula cerrada:  igual que "normal"
```

---

## Rutas de error y recuperación

### Sensor I2C falla en runtime

```
Ciclo 20s: lectura falla
→ flag sensorX_ok = false
→ valor = simulación (sim_tempMCP, sim_pressure, etc.)
→ Core 0 detecta edge !ok: mqttPublishAlert("sensor_failure", "warning", "{sensor}")

Ciclo 20s siguiente: retry automático sensorX.begin()
→ Si OK: flag = true, mqtt alert "sensor_ok"
→ Si no: seguir con simulación otro ciclo
```

### XDB401 falla en runtime (lógica especial)

```
Cada 200ms: xdb401_read() falla → xdb401_failures++
Si xdb401_failures >= 8:
  xdb401_ok = false
  xdb401_retry_at = now + 15 000ms  (15s espera)
  [Core 0] mqttPublishAlert("sensor_failure", ...)

En cualquier iteración de 200ms si !xdb401_ok y now >= xdb401_retry_at:
  xdb401_begin():  [3 intentos, con bus recovery entre cada uno]
    Intento 1: pressureSensor_isPresent()
    Intento 2: pressureSensor_recover() [9 pulsos SCL + condición STOP] → isPresent()
    Intento 3: recover() → isPresent()
  Si OK: xdb401_failures = 0, xdb401_ok = true
         [Core 0] mqttPublishAlert("sensor_ok", ...)
  Si falla: xdb401_retry_at = now + 15 000ms (nuevo backoff)
```

### WiFi pierde conexión

```
Core 0 detecta WiFi.status() != WL_CONNECTED:
  setLedState(LED_WIFI_CONNECTING)
  wifiStableSince = 0
  WiFi.reconnect()
  vTaskDelay(wifiRetryDelayMs)  — empieza en 500ms, dobla hasta 5s
  → continue (no intenta MQTT ni telemetría)

Core 1 sigue funcionando: sensores, display, LeakDetector — sin pausa.
```

### MQTT pierde conexión

```
Core 0 detecta !mqttClient.connected():
  setLedState(LED_MQTT_CONNECTING)
  [TLS] prepareSecureClient() — recarga cert en RAM
  mqttConnect() — backoff 2s → 15s (+2s por intento)
  Si OK:
    subscribe "cmd"
    mqttPublishAlert("mqtt_reconnect", "info", ...)
    deviceInfoSent ya es true — NO republica register
```

### OTA activa

```
ArduinoOTA.onStart():
  isUpdatingOTA = true
  Para cada relay: digitalWrite(RELAY_PINS[i], LOW)  — seguridad

networkTask: while isUpdatingOTA → vTaskDelay(10ms), continue
  [Telemetría pausada, MQTT pausa, watchdog sigue reseteándose]

loop(): no hay guard — Core 1 sigue leyendo sensores y pantalla
  [Display puede mostrar "OTA" si drawBootScreen se llama — depende de callback]

ArduinoOTA.onEnd():
  isUpdatingOTA = false
  ESP.restart()
```
