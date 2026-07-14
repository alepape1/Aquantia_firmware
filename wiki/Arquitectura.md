# Arquitectura del código fuente

El firmware compila como una única unidad de traducción (modelo Arduino). El sketch principal
`ESP_monitor_server.ino` contiene solo variables globales, `setup()` y `loop()`; toda la
lógica de funciones vive en módulos `.h` incluidos en orden estricto.

---

## Estructura de ficheros

```
ESP_monitor_server/
├── ESP_monitor_server.ino   ← globals + setup() + loop()
│
├── secrets.h                ← credenciales dev (no subir al repo)
├── mqtt_cert.h              ← CA cert PEM (Let's Encrypt ISRG Root X1)
├── trust_anchors.h          ← BearSSL trust anchors para SSLClient (GSM)
│
├── provisioning.h           ← SoftAP + NVS — primer arranque sin DEV_MODE
├── led_control.h            ← máquina de estados LED no-bloqueante
├── display_tft.h            ← pantalla ST7789 240×135 (solo PROFILE_METEO)
│
├── pressure_sensor_i2c.h/cpp ← protocolo I2C XGZP6847D (XDB401)
├── LeakDetector.h           ← detección de fugas/rotura/obstrucción (EMA)
│
├── aht20_driver.h           ← AHT20 temperatura+humedad (I2C, sin lib)
├── ina219_driver.h          ← INA219 voltaje/corriente/potencia (I2C, sin lib)
├── hdc1080_driver.h         ← HDC1080 temperatura+humedad (I2C, sin lib)
├── htu2x_driver.h           ← HTU21/HTU2x temperatura+humedad (I2C, sin lib)
├── light_sensor.h           ← TSL2584/APDS-9930/BH1750 iluminancia
├── SoilSensor.h/cpp         ← Helissense Modbus RTU RS485 (4800 baud, 8N1)
├── SoilProvisioner.h        ← auto-provisioning dirección Modbus vía NVS (soil_bus/addr)
│
├── sensor_recovery.h        ← backoff de reintentos, helpers BMP280, driver XDB401
├── pipeline_core.h          ← simulación presión/caudal + lectura real XDB401
├── wind_sensor.h            ← media móvil ADC anemómetro, promedio vectorial viento
├── http_client.h            ← capa HTTP WiFi (excluida en AQUA_SMART_REMOTE)
├── gsm_modem.h              ← init SIM7000G, GPRS, BearSSL TLS (solo AQUA_SMART_REMOTE)
├── network_task.h           ← takeSnapshot() + networkTask() FreeRTOS (Core 0)
├── sensor_read.h            ← readSlowSensors() + readSoilSensor() para loop()
└── mqtt_helpers.h           ← mqttConnect, mqttCallback, mqttPublishRegister/Alert
```

---

## Módulos principales

### `sensor_recovery.h`
Gestión de fallos de sensor: backoff exponencial (4 intentos → cooldown 5 min), helpers
de init/lectura del BMP280 con `setSampling()`, driver completo del XDB401 (detección
de dirección I2C 0x6D/0x7F, lectura de 5 bytes presión+temperatura 24+16 bit),
`temperatureSourceName()` / `pressureSourceName()` para el log de telemetría.

### `pipeline_core.h`
Dos modos de operación para presión y caudal de tubería:
- **Simulación** (`pipeline_mode = "sim"`): `driftClamp` + `updateSimulatedValues` +
  `pipelineNoise` generan valores que derivan suavemente con ruido sinusoidal.
- **Real** (`pipeline_mode = "real"`): `readRealPipelineSensors` lee el XDB401 cada 200 ms
  y el caudalímetro YF-B9 (ISR + spinlock `_flowMux`) en micros.
`updatePipelineValues` despacha al modo activo y alimenta al `LeakDetector`.

**`readXDB401Safe(float&)`** — helper interno que centraliza la lógica de reintentos y
gestión de fallos del XDB401 (antes duplicada 3 veces). Asigna `NAN` si el sensor falla.

**Ghost flow guard**: si `_flowLpm > 0` pero no llegan pulsos en el intervalo actual y ese
intervalo supera 2 periodos esperados al caudal previo, `_flowLpm` se zerifica inmediatamente
en lugar de esperar hasta 500 ms (útil para detección rápida de parada de bomba).

### `wind_sensor.h`
- **Media móvil circular** (10 muestras, 100 ms) sobre ADC anemómetro y veleta.
- **Promedio vectorial**: `accumulateWindVector` suma componentes sen/cos; `calcAndResetWindVector`
  calcula `atan2` y resetea los acumuladores. Evita el error de la media aritmética cerca
  del norte (350° + 10° ≠ 180°).
- `adcToWindSpeed`, `adcToWindDeg`, `degToCompass` para conversión y etiquetado.

### `http_client.h`
Capa HTTP solo para perfiles WiFi. Incluye `serverUseTls/serverBaseUrl/prepareSecureClient`
para TLS con `WiFiClientSecure`, y las funciones de polling de config y relay:
`syncPipelineScenario`, `checkRelayCommand`, `postDeviceInfo`. No compila en AQUA_SMART_REMOTE.

### `gsm_modem.h`
Init del módem SIM7000G (solo `PROFILE_AQUA_SMART_REMOTE`):
1. Pulso PWRKEY, `Serial1.begin(115200)`, `modemSIM.init()`
2. Espera activa `SIM_READY` hasta 30 s
3. Tres intentos `gprsConnect()` con delay 5 s entre ellos
4. `sim7000g_uploadCACert()`: sube el PEM al filesystem interno del modem (si no existe)
5. `prepareGsmTLSClient()`: configura `SSLClient` (BearSSL en ESP32) con timeout 75 s

El SIM7000G R1529 no soporta `AT+CSSLCFG` para verificación de CA en hardware; todo el
handshake TLS ocurre en el MCU sobre un socket TCP plano.

### `network_task.h`
FreeRTOS task en Core 0 (prioridad 2):
- **Conectividad**: backoff exponencial 500 ms→30 s; reset WiFi cada 10 fallos; `esp_restart`
  tras 60 fallos. En GSM: polling AT/GPRS cada 2 s, refresh PDP tras timeout MQTT.
- **MQTT**: `prepareSecureClient/prepareGsmTLSClient` en cada intento de conexión (no una sola vez),
  `mqttConnect`, `mqttPublishRegister`, alertas al arranque, `mqttClient.loop()` continuo.
- **Alertas edge-triggered**: pipeline, sensores individuales, `soil_dry`, `low_heap` —
  todas con cooldown 12 h para re-emisión si el fallo persiste.
- **Telemetría**: cada `telemetryIntervalMs` llama `takeSnapshot()` y publica en
  `aquantia/<finca_id>/telemetry`. WiFi: payload completo 1280 B. GSM: payload slim 640 B.
- **OTA**: `ArduinoOTA.handle()` sin bloquear sensores (solo perfiles WiFi).

### `sensor_read.h`
Wrappers del `loop()` de Core 1:
- `readSlowSensors(now)`: ejecuta el ciclo completo de 20 s condicionado a
  `now - lastSlowSensorRead >= telemetryIntervalMs`; lee todos los sensores I2C según perfil,
  construye `TelemetrySnapshot` y lo publica en `telemetryQueue` con `xQueueOverwrite`.
- `readSoilSensor(now)`: muestreo adaptativo RS485 Helissense (3 s en riego/post-riego, 20 s
  en reposo). Solo compila para METEO, IRRIGATION y AQUA_SMART_REMOTE.

### `mqtt_helpers.h`
- `mqttCallback`: decodifica comandos relay + config de pipeline desde `aquantia/<finca_id>/cmd`.
  Filtrado por MAC opcional. Protege escrituras con `dataMutex`.
  Acepta `telemetry_interval_s` para cambiar el ritmo de envío en caliente (p. ej. 5 s durante riego).
- `mqttConnect`: Client ID `aquantia-{MAC}`, `setKeepAlive(180)` para GSM / 60 para WiFi.
  Usa **sesión persistente** (`cleanSession=false`): el broker encola mensajes QoS 1 mientras
  el dispositivo está desconectado y los entrega al reconectar. El client ID basado en MAC
  garantiza que el broker identifica la misma sesión entre reconexiones.
  Suscripción a `cmd` con QoS 1.
- `mqttPublishRegister`: payload de registro al arranque con chip info, `sketch_size_kb`,
  `free_sketch_kb`, `device_profile` string.
- `mqttPublishAlert`: publica en `aquantia/<finca_id>/alerts` con `device_mac`, `type`,
  `severity`, `message`.

---

## Orden de inclusión en el .ino

El compilador Arduino resuelve todo en una sola TU; el orden de los `#include` es el contrato
de dependencias:

```
[globals — declarados en .ino]
   ↓
sensor_recovery.h   (necesita pressure_sensor_i2c.h incluido antes)
   ↓
pipeline_core.h     (necesita xdb401_* de sensor_recovery.h)
wind_sensor.h       (necesita windMux declarado en la sección FreeRTOS del .ino)
   ↓
http_client.h       (solo !AQUA_SMART_REMOTE — necesita WiFiClientSecure, globals relay)
gsm_modem.h         (solo AQUA_SMART_REMOTE — necesita modemSIM, gsmTLSClient)
   ↓
display_tft.h       (solo HAS_DISPLAY)
mqtt_helpers.h      (solo USE_MQTT)
   ↓
network_task.h      (necesita todo lo anterior — takeSnapshot + networkTask)
sensor_read.h       (necesita todos los globals de sensores)
```
