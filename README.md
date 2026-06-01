# Aquantia — Firmware ESP32

**Versión activa:** `0.2.0-beta.3` · **Rama:** `feat/helissense-sensor` · **Backend compatible:** `v0.1.0+`

Firmware para la estación meteorológica y sistema de detección de fugas/control de riego Aquantia. Compatible con **tres perfiles de hardware** seleccionables en tiempo de compilación.

Repositorio del servidor y dashboard: [alepape1/app_meteo](https://github.com/alepape1/app_meteo)

---

## Índice

- [Perfiles de dispositivo](#perfiles-de-dispositivo)
- [Herramienta de flash](#herramienta-de-flash)
- [Provisioning — primer arranque](#provisioning--primer-arranque)
- [Librerías necesarias](#librerías-necesarias)
- [secrets.h — modo DEV](#secretsh--modo-dev)
- [Arquitectura del firmware](#arquitectura-del-firmware)
- [Timings de lectura de sensores](#timings-de-lectura-de-sensores)
- [Modo MQTT](#modo-mqtt)
- [Modo HTTP legacy](#modo-http-legacy)
- [Relay y electroválvulas](#relay-y-electroválvulas)
- [Pantalla TFT](#pantalla-tft)
- [Sensor de suelo RS485 Helissense](#sensor-de-suelo-rs485-helissense)
- [Filtros y estabilización](#filtros-y-estabilización)
- [Simulación de sensores](#simulación-de-sensores)
- [Sensor de presión de tubería XDB401](#sensor-de-presión-de-tubería-xdb401)
- [Umbrales de alerta de presión](#umbrales-de-alerta-de-presión)
- [Detector automático de fugas](#detector-automático-de-fugas-leakdetector)
- [Alertas MQTT](#alertas-mqtt)
- [Modo debug](#modo-debug)
- [Ahorro energético](#ahorro-energético)
- [Problemas conocidos](#problemas-conocidos)

---

## Perfiles de dispositivo

El firmware compila un binario distinto para cada dispositivo. El perfil se pasa con `-DDEVICE_PROFILE=N` en tiempo de compilación; no hay que editar el código fuente.

| Perfil | `DEVICE_PROFILE` | Hardware | Relays | Pantalla | Sensores meteo |
|--------|:----------------:|----------|:------:|:--------:|:--------------:|
| **METEO** | 1 | LilyGo TTGO T-Display | 1 × GPIO26 | ST7789 240×135 | Sí (MCP9808, HTU2x, DHT11, BMP280, MicroPressure, TSL2584/APDS, YL-69) |
| **IRRIGATION** | 2 | ESP32 4-Relay Board | 4 × GPIO32/33/25/26 | No | AHT20 (T+H), INA219 (V/I/P), BMP280 (T+P, fallback) |
| **AGROMETEO** | 3 | Wemos D1 Mini ESP32 + CJMCU-14 | Sin relays | No | Sí (BH1750, HDC1080, BMP280, MicroPressure) |

> **Nota:** El perfil AGROMETEO no incluye relays. Publica parámetros agrometeorológicos calculados: `dew_point`, `heat_index` y `abs_humidity`.

Pinout completo de cada perfil: [PINOUT.md](PINOUT.md)

---

## Herramienta de flash

La herramienta principal es **`tools/flasher_gui.py`** — una aplicación de escritorio Python/Tkinter que gestiona todo el ciclo de desarrollo:

```
tools/
├── flasher_gui.py          ← herramienta principal
├── Aquantia Flash Tool.bat ← lanzador Windows (doble clic)
└── Aquantia Flash Tool.vbs ← lanzador silencioso (sin ventana CMD)
```

### Funcionalidades

| Función | Descripción |
|---------|-------------|
| Selector de perfil | METEO / IRRIGATION |
| Selector de versión | Working copy, tags, commits históricos |
| Info de commit | Muestra hash, fecha, autor y mensaje antes de flashear |
| Estado del binario | Indica si el binario en caché es válido o hay que recompilar |
| Modo DEV / PROD | Activa/desactiva `DEV_MODE` en `secrets.h` sin editar el archivo |
| Compilar | `arduino-cli compile` con barra de progreso por hitos |
| Flash USB | `arduino-cli upload` con barra de progreso real (formato `esptool`) |
| Flash OTA | `espota.py` con barra de progreso real (formato `espota`) |
| Buscar OTA | Descubre dispositivos ArduinoOTA via mDNS (`_arduino._tcp`) |
| Factory Provision | Lee MAC → genera token → registra en backend → escribe NVS |
| **Borrar NVS** | Lee tabla de particiones real del chip y borra solo la partición NVS |

### Requisitos

```bash
pip install pyserial          # puertos serie
pip install zeroconf          # opcional — descubrimiento OTA por mDNS
pip install qrcode[pil]       # opcional — QR en factory provision
pip install bcrypt            # factory provision (hash de token)
```

Arduino IDE 2.x instalado (proporciona `arduino-cli`, `esptool` y `espota.py`).

### Borrar NVS

El botón **"🗑 Borrar NVS"** en la sección Factory Provision:

1. Lee la tabla de particiones real del chip desde `0x8000`
2. Localiza la partición NVS (type=data, subtype=nvs)
3. Muestra en el log el nombre, offset y tamaño exactos detectados
4. Pide confirmación antes de borrar
5. Ejecuta `esptool erase_region <offset> <size>`

Funciona con todos los ESP32 (S2, S3, C3, C6) y tablas de particiones personalizadas.

Tras el borrado el dispositivo arranca en **modo SoftAP** y espera que el usuario configure el WiFi.

---

## Provisioning — primer arranque

El firmware incluye un sistema de provisioning **SoftAP + NVS** que elimina la necesidad de hardcodear credenciales WiFi.

### Flujo en modo PROD

```
1. Arranque sin credenciales en NVS
   └─► TFT muestra pantalla "AQUANTIA SETUP" con el SSID y la contraseña
   └─► Serial imprime: [PROV] Conecta tu movil a: Aquantia-XXXXXX

2. Usuario conecta su móvil a la red WiFi "Aquantia-XXXXXX" (pass: aquantia1)
   └─► Abre http://192.168.4.1 en el navegador

3. Portal SoftAP muestra redes disponibles
   └─► Usuario elige su red y escribe la contraseña

4. ESP32 guarda las credenciales en NVS (partición 0x9000)
   └─► Reinicia y conecta a la red configurada

5. El dispositivo funciona con normalidad — ya no vuelve al portal
```

### SSID del punto de acceso

El SSID se forma con los últimos 6 caracteres hexadecimales de la MAC del chip, leídos de eFuse (sin necesidad de WiFi inicializado):

```
Serial = AQ-FCB467F37748
SSID   = Aquantia-F37748
```

### Modo DEV (desarrollo)

Cuando `DEV_MODE` está activo en `secrets.h`, el dispositivo **ignora el portal SoftAP** y usa directamente las credenciales de `secrets.h`. Útil para desarrollo en banco sin tener que configurar el portal cada vez.

El botón **DEV / PROD** de la Flash Tool gestiona este flag sin editar el archivo manualmente.

---

## Librerías necesarias

Instalar desde el gestor de librerías de Arduino IDE 2.x:

| Librería | Autor | Perfil | Uso |
|----------|-------|:------:|-----|
| `TFT_eSPI` | Bodmer | METEO | Pantalla ST7789 |
| *(sin librería externa)* | — | METEO | Sensor suelo Helissense — driver RS485 Modbus RTU en `SoilSensor.h/.cpp` |
| `Adafruit MCP9808 Library` | Adafruit | METEO | Temperatura exterior |
| `Adafruit BMP280 Library` | Adafruit | METEO, AGROMETEO | Temperatura + presión atmosférica |
| `SparkFun MicroPressure Library` | SparkFun | METEO, AGROMETEO | Barómetro principal |
| `DHTesp` | beegee-tokyo | METEO | DHT11 |
| `BH1750` | claws | AGROMETEO | Iluminancia |
| `SparkFun Qwiic Power Switch Library` | SparkFun | AGROMETEO | PCA9536 — alimenta el bus I2C |
| `ArduinoJson` | Benoit Blanchon | Todos | Payloads JSON |
| `PubSubClient` | knolleary | Todos (MQTT) | Cliente MQTT |

HTU2x, HDC1080, TSL2584/APDS-9930, AHT20 e INA219 se implementan directamente sobre I2C sin librería externa.
El driver del sensor de presión de tubería XDB401 (familia XGZP6847D) está en `pressure_sensor_i2c.h/.cpp`.

### Configuración TFT_eSPI (PROFILE_METEO)

Editar `User_Setup_Select.h` de la librería:

```cpp
//#include <User_Setup.h>                         // comentar
#include <User_Setups/Setup25_TTGO_T_Display.h>  // activar T-Display
```

---

## secrets.h — modo DEV

Crear `ESP_monitor_server/secrets.h` (excluido del repo vía `.gitignore`):

```cpp
// secrets.h — NO subir al repositorio

// ── Modo DEV — activo: usa estas credenciales directamente (sin portal SoftAP)
//              inactivo: el dispositivo usa NVS / portal SoftAP en primer arranque
#define DEV_MODE

// ── Perfil de hardware (solo si no es METEO, que es el valor por defecto) ─────
// #define DEVICE_PROFILE PROFILE_IRRIGATION   // ← descomentar para placa riego

// ── WiFi (solo DEV_MODE) ──────────────────────────────────────────────────────
#define WIFI_SSID     "TU_RED_WIFI"
#define WIFI_PASSWORD "TU_PASSWORD"

// ── Servidor HTTP legacy (solo modo HTTP, sin USE_MQTT) ───────────────────────
#define SERVER_IP     "meteo.aquantialab.com"
#define SERVER_PORT   443

// ── MQTT ─────────────────────────────────────────────────────────────────────
#define USE_MQTT
#define MQTT_SERVER  "meteo.aquantialab.com"
#define MQTT_PORT    8883
#define FINCA_ID     "finca-dev"

// En DEV_MODE el broker acepta el usuario interno "backend":
#define MQTT_USER    "backend"
#define MQTT_PASS    "aquantia_159"   // valor de MQTT_PASSWORD en el .env del servidor
```

En modo **PROD** (sin `DEV_MODE`), las credenciales WiFi y el token MQTT vienen de la NVS escrita por la Flash Tool durante el factory provision. El `secrets.h` solo necesita las constantes de compilación (`USE_MQTT`, `MQTT_SERVER`, `MQTT_PORT`).

### Diferencia DEV / PROD en la autenticación MQTT

Este es el punto más importante. En función del modo, el firmware usa **credenciales distintas** para conectar al broker:

| | `DEV_MODE` activo | `DEV_MODE` inactivo (PROD) |
|--|--|--|
| **Usuario MQTT** | `MQTT_USER` de `secrets.h` → `"backend"` | MAC del dispositivo → `"FC:B4:67:F3:77:48"` |
| **Contraseña MQTT** | `MQTT_PASS` de `secrets.h` | Token generado en fábrica, leído de NVS |
| **WiFi** | `WIFI_SSID` / `WIFI_PASSWORD` de `secrets.h` | SSID + contraseña guardados en NVS |
| **Provisioning** | Saltado | Portal SoftAP en primer arranque |

El motivo es que en PROD el broker identifica cada dispositivo **por su MAC** y verifica el token con un hash bcrypt almacenado en la base de datos. En DEV, en cambio, no existe ese token todavía (el dispositivo no ha pasado por fábrica), así que se usa el usuario interno `backend` que el broker acepta directamente comparando la contraseña con la variable de entorno `MQTT_PASSWORD`.

> **Nota de implementación:** en el firmware, la sobreescritura `mqtt_user = MAC` está dentro de un bloque `#ifndef DEV_MODE`. Sin ese guard, el usuario de `secrets.h` quedaría siempre machacado por la MAC tras la conexión WiFi, haciendo inefectivo el `MQTT_USER` en DEV.

---

## Arquitectura del firmware

### FreeRTOS dual-core

```
Core 1 — loop()
 ├─ Cada 100ms   → Leer ADC anemómetro/veleta (METEO)
 │                Acumular vector de viento para promedio vectorial
 ├─ Cada 200ms   → [solo pipeline_mode=real] Leer XDB401 + caudalímetro
 │                Actualizar display y LeakDetector sin enviar a backend
 ├─ Cada 20s*    → Leer I2C: sensores meteo según perfil
 │                Calcular parámetros agrometeorológicos (AGROMETEO)
 │                Construir TelemetrySnapshot → xQueueOverwrite (sin bloqueo)
 │                Actualizar pantalla TFT (METEO)
 └─ Siempre      → Gestionar botones y timeout de pantalla (METEO)
                   ledTick() — máquina de estados LED no-bloqueante

Core 0 — networkTask()  [prioridad 2, watchdog 30s, trigger_panic]
 ├─ Cada ~10ms   → ArduinoOTA.handle()  ← nunca bloqueado
 │                esp_task_wdt_reset()
 ├─ Al arrancar  → [HTTP] POST /api/device_info
 │                 [MQTT] mqttConnect() + mqttPublishRegister()
 │                 mqttPublishAlert("device_reboot", …)
 ├─ Continuo     → [MQTT] mqttClient.loop()  ← recibe comandos relay
 ├─ Cada 2s      → [HTTP] GET /api/relay/command → actuar relays + ack
 ├─ Edge-trig.   → mqttPublishAlert() — alertas de sensor/pipeline/heap
 └─ Cada 20s*    → xQueuePeek (sin bloqueo) → envío telemetría
                   [HTTP] CSV → POST /send_message
                   [MQTT] JSON → publish aquantia/<finca_id>/telemetry

* telemetryIntervalMs ajustable en runtime vía MQTT / HTTP /api/pipeline/config
```

Sincronización entre cores:
- **`telemetryQueue`** (FreeRTOS Queue, tamaño 1): Core 1 publica el snapshot con `xQueueOverwrite`; Core 0 lo consume con `xQueuePeek`. Nunca bloquea ningún core.
- **`dataMutex`** (semáforo binario): protege exclusivamente las escrituras de config desde Core 0 (`pipelineScenario`, `pipelineMode`, `relayActive[]`, `irrigationType`). Lecturas en Core 1 son eventual-consistent (máx. 1 ciclo).
- **`windMux`** (`portMUX_TYPE`): sección crítica de bare-metal para acumuladores vectoriales del viento. Seguro frente a preempción entre loop y sección crítica de snapshot.
- **`char[16]`** para `pipelineMode` y `pipelineScenario**: elimina carreras de heap entre cores que ocurrían con `String`.

### Potencia de transmisión WiFi

El firmware fija la potencia TX a **18.5 dBm** (`WIFI_POWER_18_5dBm`) tanto en el arranque inicial como en cada reconexión. Valor empíricamente elegido para mejorar la estabilidad en instalaciones con cobertura marginal sin saturar el receptor del AP.

### Reconexión WiFi robustecida

`networkTask` implementa backoff exponencial y recuperación de stack colgado:

| Condición | Acción |
|-----------|--------|
| WiFi desconectado | Intento de reconexión con backoff de 500 ms → máx. 30 s |
| Cada 10 fallos consecutivos | `WiFi.disconnect(true)` + reset completo del stack WiFi |
| 60 fallos consecutivos (~5 min) | `esp_restart()` — reinicio total del dispositivo |
| Reconexión exitosa | Backoff reseteado a 500 ms tras 10 s de conexión estable |

### Seguridad OTA

Al arrancar una actualización OTA:
- `networkTask` deja de enviar datos
- Todos los relays pasan a OFF (LOW para relay activo-HIGH)
- Core 1 (sensores/pantalla) sigue funcionando

---

## Timings de lectura de sensores

Todos los intervalos son constantes de compilación definidas al inicio de `ESP_monitor_server.ino`. `telemetryIntervalMs` es el único ajustable en tiempo de ejecución vía MQTT/HTTP (valor por defecto 20 s).

### Tabla de ciclos

| Intervalo | Constante | Core | Qué ocurre | Perfiles |
|----------:|-----------|:----:|-----------|:--------:|
| **100 ms** | `WIND_MS` | 1 | Lee ADC anemómetro (GPIO 36) → `windSpeed`; ADC veleta → `windDirection`; acumula vector | METEO |
| **200 ms** | `PIPELINE_FAST_MS` | 1 | Lee XDB401 (presión + temperatura agua) y caudalímetro → actualiza display y LeakDetector. **Solo activo cuando `pipeline_mode = real`** | METEO, AGROMETEO |
| **1 s** | `SCREEN_MS` | 1 | Refresca pantalla TFT (doble buffer, sin parpadeo) | METEO |
| **2 s** | `RELAY_MS` | 0 | `GET /api/relay/command` — solo modo HTTP legacy | Todos |
| **10 s** | `DEBUG_INTERVAL_MS` | 1 | Imprime reporte de estado por Serial — **solo con `DEBUG_MODE` activo** | Todos |
| **20 s** \* | `telemetryIntervalMs` | 1 → 0 | Lee todos los sensores I2C → construye `TelemetrySnapshot` → Core 0 envía MQTT o HTTP | Todos |
| **20 s** | `PIPELINE_SYNC_MS` | 0 | Sincroniza config pipeline desde el servidor | Todos |
| **15 s** | `XDB401_RETRY_INTERVAL` | 1 | Reintenta `xdb401_begin()` tras 8 fallos consecutivos de lectura | Todos |
| **10 min** | `DISPLAY_TIMEOUT_MS` | 1 | Apaga la pantalla TFT si no hay actividad de botones (ajustable vía MQTT `display_timeout_s`) | METEO |
| **3 s / 20 s** | `SOIL_FAST_MS` / `SOIL_SLOW_MS` | 1 | Muestreo adaptativo del sensor RS485: 3 s durante riego activo y en ventana post-riego (2 min); 20 s en reposo | METEO |

\* `telemetryIntervalMs` puede modificarse en tiempo de ejecución mediante MQTT (`telemetry_interval_s`) o HTTP `/api/pipeline/config`. El valor por defecto (y el usado para sincronizar las lecturas I2C) es **20 s**.

### Sensores leídos en el ciclo de 20 s (por perfil)

| Sensor | Magnitudes | METEO | IRRIGATION | AGROMETEO |
|--------|-----------|:-----:|:----------:|:---------:|
| **MCP9808** | Temperatura exterior | ✓ | — | — |
| **HTU2x** | Temperatura + humedad | ✓ | — | — |
| **DHT11** | Temperatura + humedad (secundario) | ✓ | — | — |
| **AHT20** | Temperatura + humedad ambiente | — | ✓ | — |
| **HDC1080** | Temperatura + humedad | — | — | ✓ |
| **MicroPressure** | Presión atmosférica (principal) | ✓ | — | ✓ |
| **BMP280** | Temperatura + presión atmosférica (fallback) | ✓ | ✓ | ✓ |
| **TSL2584 / APDS-9930** | Iluminancia | ✓ | — | — |
| **BH1750** | Iluminancia | — | — | ✓ |
| **INA219** | Voltaje bus (V), corriente (mA), potencia (mW) | — | ✓ | — |
| **YL-69** (ADC) | Humedad suelo (fallback si Helissense no responde) | ✓ | — | — |
| **Helissense RS485** | Humedad + temperatura + CE + pH + TDS + NPK | ✓ | ✓ | — |
| **XDB401** (pipeline) | Presión tubería + temperatura agua | ✓ | ✓ | ✓ |
| **Caudalímetro YF-B9** (ISR) | Caudal L/min + litros totales | ✓ | — | ✓ |

> El XDB401 y el caudalímetro se leen **también cada 200 ms** (ciclo rápido) cuando `pipeline_mode = real`. En el ciclo de 20 s se sincronizan sus valores al snapshot de telemetría.

### Fallback y simulación

Si un sensor no responde al arrancar (o acumula 5 errores consecutivos), el firmware sustituye su lectura por un valor simulado que deriva suavemente. El sensor vuelve a intentar reinicializarse en el siguiente ciclo de 20 s (o cada 30 s en el caso del XDB401). El campo `*_source` del payload indica si el valor es real o simulado (`"SIM"`).

---

## Modo MQTT

Modo de comunicación principal. Activar definiendo `USE_MQTT` en `secrets.h`.

### Topics

| Topic | Dirección | Cuándo | Contenido |
|-------|-----------|--------|-----------|
| `aquantia/<finca_id>/register` | ESP → broker | Al arrancar (1 vez) | JSON con MAC, IP, chip info, relay_count, firmware_version, device_profile |
| `aquantia/<finca_id>/telemetry` | ESP → broker | Cada 20s | JSON con todos los campos de sensores |
| `aquantia/<finca_id>/alerts` | ESP → broker | Edge-triggered | JSON alerta de sensor/pipeline/heap |
| `aquantia/<finca_id>/cmd` | broker → ESP | Comando | Ver tabla de comandos más abajo |

### Comandos MQTT (topic `cmd`)

Todos los campos son opcionales — se puede enviar solo el subconjunto que se quiere cambiar. Si se incluye `mac`, solo el dispositivo con esa MAC ejecutará el comando.

| Campo | Tipo | Valores | Efecto |
|-------|------|---------|--------|
| `mac` | string | `"FC:B4:67:F3:77:48"` | Filtro por MAC — otros dispositivos ignoran el mensaje |
| `relay` + `state` | int + bool | `{"relay":0,"state":true}` | Activa/desactiva el relay N (índice 0-based). En transición OFF→ON resetea el contador de sesión de caudal |
| `pipeline_mode` | string | `"real"` / `"sim"` | Cambia modo pipeline |
| `pipeline_scenario` | string | `"normal"` / `"leak"` / `"burst"` / `"obstruction"` | Fuerza escenario (solo modo sim) |
| `irrigation_type` | string | `"sprinkler"` / `"drip"` / `"drip_tape"` / `"micro_sprinkler"` | Cambia perfil de riego y reinicia baseline del LeakDetector |
| `telemetry_interval_s` | int | 5–3600 | Intervalo de publicación de telemetría (segundos) |
| `config_sync_interval_s` | int | 5–3600 | Intervalo de sincronización de config desde el servidor (segundos) |
| `display_timeout_s` | int | 0–3600 | Timeout de apagado de pantalla TFT (solo METEO; 0 = nunca apagar) |

### Payload telemetría (JSON)

Campos presentes en todos los perfiles salvo indicación:

```json
{
  "temperature":              22.5,
  "pressure":                 101.3,
  "temperature_barometer":    21.8,
  "humidity":                 65.2,
  "temperature_source":       "MCP9808",
  "pressure_source":          "MicroPressure",
  "bmp280_ok":                true,
  "bmp280_temperature":       21.5,
  "bmp280_pressure":          101.2,
  "pressure_micro":           101.3,
  "pressure_bmp280":          101.2,
  "windSpeed":                3.5,
  "windDirection":            180.0,
  "windSpeedFiltered":        3.3,
  "windDirectionFiltered":    178.0,
  "light":                    350.0,
  "dht_temperature":          21.6,
  "dht_humidity":             63.0,
  "rssi":                     -65,
  "free_heap":                245000,
  "uptime_s":                 12345,
  "relay_active":             0,
  "relay_count":              1,
  "soil_moisture":            50.0,
  "halisense_ok":             true,
  "soil_irrig_mode":          false,
  "soil_temperature":         19.5,
  "soil_ec":                  0.35,
  "soil_ph":                  6.8,
  "soil_tds":                 175.0,
  "soil_n":                   42,
  "soil_p":                   18,
  "soil_k":                   31,
  "pipeline_pressure":        3.50,
  "pipeline_flow":            5.00,
  "flow_total_l":             12.3,
  "flow_session_l":           3.5,
  "flow_irrig_l":             11.8,
  "flow_leak_l":              0.5,
  "pipeline_scenario":        "normal",
  "pipeline_mode":            "real",
  "pipeline_source":          "real",
  "pipeline_pressure_ok":     true,
  "pipeline_flow_ok":         true,
  "irrigation_type":          "sprinkler",
  "leak_detect_trained":      true,
  "leak_baseline_pressure":   2.80,
  "leak_baseline_flow":       4.95,
  "leak_warmup_progress":     20,
  "xdb401_ok":                true,
  "xdb401_temperature":       18.3,
  "mac_address":              "FC:B4:67:F3:77:48",
  "ip_address":               "192.168.1.9",
  "firmware_version":         "0.2.0-beta.3",
  "ts":                       1746360000
}
```

Campos exclusivos **PROFILE_IRRIGATION** (solo cuando `DEVICE_PROFILE = 2`):

```json
{
  "aht20_ok":           true,
  "ina219_ok":          true,
  "ina219_bus_voltage": 12.34,
  "ina219_current_ma":  150.0,
  "ina219_power_mw":    1850.0
}
```

> - `aht20_ok`: `true` cuando el sensor AHT20 respondió correctamente. Si falla, la temperatura/humedad provienen del BMP280 o del simulador.
> - `ina219_ok`: `true` cuando el INA219 respondió. Si falla, los tres campos se omiten del payload.
> - `ina219_bus_voltage`: voltaje de bus medido (V), resolución 4 mV.
> - `ina219_current_ma`: corriente (mA), resolución 0.1 mA (shunt 0.1 Ω).
> - `ina219_power_mw`: potencia (mW), resolución 2 mW.

Campos exclusivos **PROFILE_AGROMETEO** (solo cuando `DEVICE_PROFILE = 3`):

```json
{
  "dew_point":     12.4,
  "heat_index":    24.1,
  "abs_humidity":   9.85
}
```

> - `halisense_ok`: `true` cuando el sensor RS485 respondió correctamente en el último ciclo
> - `soil_irrig_mode`: `true` si el relay estaba activo o se estaba en ventana post-riego durante la lectura
> - `soil_temperature … soil_k`: solo presentes cuando `halisense_ok = true`; si es `false`, se omiten del payload
> - `temperature_source`: `"MCP9808"` | `"BMP280"` | `"HDC1080"` | `"SIM"`
> - `pressure_source`: `"XDB401"` | `"MicroPressure"` | `"BMP280"` | `"SIM"`
> - `pipeline_source`: `"real"` (presión+caudal reales) | `"real_flow"` (caudal real, presión sim) | `"sim"` | `"fallback"`
> - `flow_total_l`: litros acumulados desde el último arranque (resolución 100 mL)
> - `flow_session_l`: litros desde la última apertura de válvula (se resetea en transición OFF→ON del relay)
> - `flow_irrig_l`: litros acumulados mientras el relay estuvo activo (ciclos de riego)
> - `flow_leak_l`: litros acumulados mientras el relay estaba cerrado (posibles fugas de fondo)
> - `xdb401_temperature`: temperatura del fluido medida por el sensor de presión — solo cuando `xdb401_ok = true`
> - `ts`: timestamp NTP epoch Unix; se omite si el reloj aún no está sincronizado

### Autenticación MQTT — flujo completo

El broker usa el plugin **mosquitto-go-auth**, que delega la validación a un webhook HTTP al backend Flask en cada intento de conexión:

```
ESP32                   Mosquitto               Backend Flask
  │                         │                        │
  │── CONNECT (user/pass) ──►│                        │
  │                         │── POST /api/mqtt/auth ─►│
  │                         │   {"username":…,        │
  │                         │    "password":…}        │
  │                         │                        │ (valida)
  │                         │◄── 200 OK ─────────────│
  │◄── CONNACK (rc=0) ──────│                        │
```

El backend acepta **dos tipos de credenciales**:

| Tipo | `username` | `password` | Cuándo se usa |
|------|-----------|-----------|---------------|
| Usuario interno | `"backend"` | Valor de `MQTT_PASSWORD` en `.env` | DEV_MODE, backend Flask propio |
| Dispositivo | MAC del chip (`"FC:B4:67:F3:77:48"`) | Token generado en fábrica (bcrypt en DB) | PROD, dispositivos reales |

Si el broker no puede alcanzar el backend, deniega todas las conexiones.

### TLS — arquitectura por perfil

El firmware implementa TLS de forma diferente según el perfil de hardware:

| Perfil | Librería TLS | Fuente de confianza | Cómo funciona |
|--------|-------------|---------------------|---------------|
| WiFi (`DEFAULT` / `IRRIGATION`) | `WiFiClientSecure` (mbedTLS) | `mqtt_cert.h` — bundle PEM (R13 + ISRG Root X1) | `setCACert()` antes de conectar |
| SIM (`AQUA_SMART_REMOTE`) | `SSLClient` (BearSSL en ESP32) | `trust_anchors.h` — struct BearSSL | Handshake TLS en el MCU sobre TCP plano |

**Por qué dos métodos:** El firmware SIM7000G R1529 no soporta `AT+CSSLCFG="authmode"` ni `AT+CSSLCFG="cacert"`, por lo que el stack SSL hardware del modem no puede verificar CA. BearSSL corre completamente en el ESP32 sobre un socket TCP normal del SIM7000G.

**Raíz de confianza:** Let's Encrypt ISRG Root X1 — fingerprint SHA-256 `96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6`. Expira 2035 — normalmente no necesita renovación manual.

Para regenerar los archivos de certificado si cambia la CA raíz, ver [`tools/gen_trust_anchor.py`](tools/gen_trust_anchor.py) y la [wiki de TLS](wiki/TLS.md).

---

## Alertas MQTT

El firmware publica en `aquantia/<finca_id>/alerts` solo al **cambio de estado** (edge-triggered). El backend inserta una fila en la tabla `alerts`.

### Payload alerta

```json
{ "device_mac": "FC:B4:67:F3:77:48", "type": "leak", "severity": "warning", "message": "Fuga detectada: caudal con valvula cerrada" }
```

### Tipos de alerta

| `type` | `severity` | Cuándo |
|--------|-----------|--------|
| `device_reboot` | info | Al reconectar tras reinicio — el campo `message` incluye el motivo de reset (`encendido`, `reinicio SW`, `panic/crash`, `WDT tarea`, `brownout`, etc.) y, si fue WDT, la tarea/fase registrada por `wdt_heartbeat()` |
| `mqtt_reconnect` | info | Reconexión al broker (no primer arranque) |
| `leak` | warning | LeakDetector detecta fuga |
| `burst` | critical | LeakDetector detecta rotura |
| `obstruction` | warning | LeakDetector detecta obstrucción |
| `pipeline_ok` | info | Pipeline se recupera a estado normal |
| `sensor_failure` | warning | Sensor deja de responder (XDB401, MCP9808, BMP280, HTU2x, HDC1080, BH1750, MicroPressure, AHT20, INA219) |
| `sensor_ok` | info | Sensor se recupera |
| `low_heap` | warning | Heap libre < 30 KB |

### Cooldown de alertas de sensor

Las alertas `sensor_failure` / `sensor_ok` se disparan **solo al cambio de estado** (edge-triggered). Además, si el sensor permanece en fallo, el firmware re-emite la alerta cada **12 horas** (`SENSOR_ALERT_COOLDOWN = 43 200 000 ms`) para que el operador reciba un recordatorio sin inundar el broker.

### Diagnóstico WDT — `wdt_heartbeat()`

Antes de cada operación bloqueante relevante, el firmware llama a `wdt_heartbeat(taskName, phase)`, que escribe el nombre de la tarea y la fase en **RTC RAM** (persiste a través de reinicios). Si el WDT dispara durante esa operación, el campo `message` de la alerta `device_reboot` incluirá `"WDT tarea: <task>:<phase>"`, facilitando el diagnóstico remoto sin necesidad de monitor serie.

Las fases registradas son:

| Tarea | Fases |
|-------|-------|
| `NetworkTask` | *(idle)*, `wifi_reconnect`, `scenario_sync`, `mqtt_connect`, `mqtt_publish` |
| `loopTask` | *(idle)*, `soil_rs485` |

---

## Modo HTTP legacy

Modo de compatibilidad. Activo cuando `USE_MQTT` **no** está definido.

El ESP envía un CSV de 16 campos cada 20s y consulta el estado del relay cada 2s.

### Formato CSV (16 campos)

```
temperature, pressure, temperature_barometer, humidity,
windSpeed, windDirection, windSpeedFiltered, windDirectionFiltered,
light, dht_temperature, dht_humidity,
rssi, free_heap, uptime_s, relay_active, soil_moisture
```

---

## Relay y electroválvulas

Los relays son **activo-HIGH** (lote Aquantia, JQC-3FF-S-Z):

| GPIO | Estado | Relay | Válvula |
|------|--------|:-----:|---------|
| LOW | Arranque / seguro | OFF | Cerrada |
| HIGH | Activado | ON | Abierta |

El campo `relay_active` es un bitmask: bit 0 = relay 1, bit 1 = relay 2, etc.

**Control MQTT:** Flask publica `{"relay": N, "state": bool}` → `mqttCallback()` actúa el relay en <100ms.

**Control HTTP:** polling `GET /api/relay/command` cada 2s → latencia ~2s.

---

## Pantalla TFT

Solo `PROFILE_METEO`. Resolución **240×135 px** (ST7789). Doble buffer con `TFT_eSprite` → sin parpadeo.

Los botones GPIO0/BOOT y GPIO35 ciclan entre las 4 vistas con debounce de 400 ms. Si la pantalla está apagada, el primer toque la reactiva sin cambiar de vista.

### Pantallas

| Pantalla | `displayView` | Cuándo se muestra |
|----------|:-------------:|------------------|
| **Boot** | — | Primeros segundos del arranque |
| **Setup AP** | — | Sin credenciales WiFi — muestra SSID y contraseña del AP |
| **Meteo** | 0 | Funcionamiento normal — sensores meteorológicos en tiempo real |
| **Pipeline** | 1 | Presión y caudal de tubería |
| **Info** | 2 | Versión de firmware, IP, MAC, uptime |
| **Suelo** | 3 | Datos del sensor Helissense (humedad, temperatura, pH, NPK) |

Todas las vistas muestran 4 puntos de navegación en la cabecera (posiciones 110/119/128/137 px). El punto de la vista activa se muestra en blanco; los demás en gris.

### Vista Meteo (vista 0)

```
┌──────────────────────────────────────┐
│ METEOSTATION   ☀350lx  WiFi  ●       │  ← ● verde=OK / rojo=error
├──────────┬──────────┬────────────────┤
│ T.EXT    │ T.INT    │ HUMEDAD        │
│ 22.5 C   │ 21.8 C   │ 65.2 %        │
│ [OK]     │ [OK]     │ [OK]          │
├──────────┼──────────┼────────────────┤
│ PRESION  │ VIENTO   │ DIRECC.        │
│ 101.3KPa │ 3.5 m/s  │ 180 deg S     │
│ [OK]     │          │               │
└──────────────────────────────────────┘
```

### Vista Pipeline (vista 1)

Dos tarjetas anchas (presión / caudal) más una franja inferior con modo, escenario y temperatura del agua:

```
┌──────────────────────────────────────┐
│ PIPELINE          •   WiFi  ●         │  ← punto 2 activo
├─────────────────────┬────────────────┤
│ PRESION             │ CAUDAL         │
│ 3.50 bar            │ 5.00 L/min     │
│ [OK]                │ [OK]           │
├─────────────────────┴────────────────┤
│ MODO: REAL   Escenario: normal        │
│              Taq: 18.3 °C            │  ← temp. agua XDB401
└──────────────────────────────────────┘
```

Cuando el sensor XDB401 no está conectado o `pipeline_mode = sim`, los valores se muestran en naranja con badge `[SIM]` y la línea `Taq` desaparece.

### Vista Suelo (vista 3)

Muestra los datos del sensor Helissense RS485. Si el sensor no responde (`halisense_ok = false`), las tarjetas aparecen en rojo con el badge `HALI!`.

```
┌──────────────────────────────────────┐
│ SUELO      HALI  WiFi  ●              │  ← HALI verde=OK / rojo=sin respuesta
├──────────┬──────────┬────────────────┤
│ HUM.SUELO│ T.SUELO  │ pH             │
│ 45.2 %   │ 19.5 °C  │ 6.8           │
├──────────┴──────────┴────────────────┤
│ N: 42 mg/kg   P: 18 mg/kg   K: 31 mg/kg │
└──────────────────────────────────────┘
```

Los botones (GPIO0/BOOT y GPIO35) reactivan la pantalla y reinician el timer de apagado (10 min por defecto, configurable vía MQTT `display_timeout_s`).

---

## Sensor de suelo RS485 Helissense

Solo `PROFILE_METEO`. Sensor Modbus RTU conectado por RS485 half-duplex a `Serial2`.

### Hardware y pines

| Señal | GPIO | Notas |
|-------|:----:|-------|
| Serial2 RX (DI del sensor) | **13** | NO usar GPIO16 — es TFT_DC |
| Serial2 TX (RO del sensor) | 17 | |
| DE/RE (control half-duplex) | 27 | |

> **Advertencia de hardware:** GPIO16 está asignado a TFT_DC por `Setup25_TTGO_T_Display.h`. Si el RX de RS485 se conecta a GPIO16, TFT_eSPI inyecta transiciones falsas en UART2 que corrompen las lecturas del sensor y provocan que los colores de la pantalla cambien aleatoriamente. Conectar siempre el DI del adaptador RS485 a **GPIO13**.

### Protocolo

Modbus RTU a **4800 baud, 8N1**. El firmware lee 7 registros desde la dirección 0x0000 del esclavo 0x01 con validación CRC-16.

| Registro | Magnitud | Resolución |
|:--------:|----------|:----------:|
| 0 | Temperatura del suelo (°C) | ÷10 |
| 1 | Humedad del suelo (%) | ÷10 |
| 2 | CE — conductividad eléctrica (µS/cm → dS/m ÷1000, TDS ppm ×0.5) | — |
| 3 | pH | ÷10 |
| 4 | Nitrógeno N (mg/kg) | directo |
| 5 | Fósforo P (mg/kg) | directo |
| 6 | Potasio K (mg/kg) | directo |

### Modo de operación y fallback

- Si Helissense responde (`halisense_ok = true`): `soilMoisture` se toma de `soil_moisture` del sensor RS485.
- Si Helissense **no responde** (`halisense_ok = false`): `soilMoisture` se toma del sensor analógico YL-69 (ADC GPIO33) como fallback.
- El firmware marca `soil_irrig_mode = true` cuando algún relay está activo o se está en ventana post-riego; este flag indica que la lectura puede estar sesgada por el agua de riego reciente.

### Muestreo adaptativo

El sensor no se lee a intervalo fijo. El firmware ajusta la frecuencia según el estado del riego para capturar la evolución de la humedad en tiempo real:

| Estado | Intervalo | Constante |
|--------|----------:|-----------|
| Reposo (válvula cerrada, fuera de ventana) | 20 s | `SOIL_SLOW_MS` |
| Riego activo (relay ON) | 3 s | `SOIL_FAST_MS` |
| Ventana post-riego (2 min tras cerrar) | 3 s | `SOIL_FAST_MS` |

Al detectar el flanco OFF del relay se inicia la ventana post-riego (`soilPostIrrigEndMs`). Si el riego se reactiva antes de expirar, la ventana se cancela y el intervalo sigue siendo 3 s. Pasados 2 min (`SOIL_POST_IRRIG_MS = 120 000 ms`) sin nuevo riego, el intervalo vuelve a 20 s.

---

## Filtros y estabilización

### Anemómetro — media móvil circular (10 muestras, 100ms)

Suaviza picos ADC. La velocidad filtrada se publica como `windSpeedFiltered`.

### Dirección del viento — promedio vectorial

Evita el error de la media aritmética cerca del norte (350° + 10° → 0°, no 180°):

```cpp
// Cada 100ms (Core 1)
windSumX += cos(deg * PI / 180);
windSumY += sin(deg * PI / 180);

// Cada 20s (Core 0)
float dir = atan2(windSumY, windSumX) * 180 / PI;
if (dir < 0) dir += 360;
```

### YL-69 humedad suelo — media móvil circular (10 muestras)

Buffer precalentado en `setup()` para evitar transitorios al arrancar. Calibración:

```cpp
#define SOIL_RAW_DRY   3300   // ADC tierra seca   (~0%)
#define SOIL_RAW_WET   1000   // ADC tierra húmeda (~100%)
```

---

## Simulación de sensores

Si un sensor no responde al arrancar, el firmware continúa con valores simulados que derivan suavemente:

| Sensor | Rango | Drift/lectura |
|--------|-------|:-------------:|
| MCP9808 | −10 a 45 °C | ±0.05 °C |
| HTU2x temperatura | −10 a 45 °C | ±0.05 °C |
| HTU2x humedad | 20–95 % | ±0.20 % |
| DHT11 temperatura | −10 a 45 °C | ±0.05 °C |
| DHT11 humedad | 20–95 % | ±0.20 % |
| HDC1080 temperatura | −10 a 45 °C | ±0.05 °C |
| HDC1080 humedad | 20–95 % | ±0.20 % |
| BH1750 iluminancia | 0–2000 lux | ±5 lux |
| Barómetro | 95–110 kPa | ±0.02 kPa |
| Luz | 0–2000 lux | ±5 lux |
| Viento | 0–15 m/s | ±0.30 m/s |
| Dirección | 0–360° | ±2.5° |
| Humedad suelo | 0–100 % | ±0.5 % |
| Pipeline presión | 0–`PIPELINE_STATIC_P` bar | ruido sinusoidal |
| Pipeline caudal | 0–`PIPELINE_NOMINAL_Q` L/min | ruido sinusoidal |

La pantalla TFT muestra `[OK]` o `[SIM]` por cada sensor.

---

## Sensor de presión de tubería XDB401

Compatible con la familia **XGZP6847D** y sensores chinos equivalentes con el mismo protocolo I2C.

| Parámetro | Valor |
|-----------|-------|
| Dirección I2C | `0x6D` (principal) / `0x7F` (algunos lotes — autodetectado) |
| Alimentación | 3.3 V |
| Pull-ups | Internos — no añadir externos |
| Frecuencia I2C | 100 kHz (400 kHz puede fallar en algunos ejemplares) |
| Salida | Presión (24 bits, ±bit 23) + temperatura (16 bits, ±bit 15) |

### Configurar el fondo de escala

Definir en `secrets.h` o pasar como flag de compilación:

```cpp
// Ajustar según el modelo físico recibido
#define XDB401_FULLSCALE_KPA  400.0f   // 0–4 bar
// #define XDB401_FULLSCALE_KPA  1000.0f  // 0–10 bar (defecto)
// #define XDB401_FULLSCALE_KPA  4000.0f  // 0–40 bar
```

### Conexión (cable 4 hilos)

| Pin | Color | Función |
|-----|-------|---------|
| 1 | Marrón | VCC (+3.3 V) |
| 2 | Azul | GND |
| 3 | Blanco | SDA |
| 4 | Negro | SCL |

### Comportamiento en firmware

- Si el sensor **responde en I2C** al arrancar (`xdb401_ok = true`): `pipeline_mode` se cambia automáticamente a `"real"` y la presión se obtiene del sensor físico.
- La **temperatura del agua** (`xdb401_temperature`) se publica en telemetría y se muestra en la pantalla TFT como `Taq`.
- Si el sensor **no está conectado**: `pipeline_mode = "sim"` y la presión/caudal usan el simulador determinista.
- El backend puede forzar `pipeline_mode = "sim"` en cualquier momento vía MQTT `pipeline_config` o HTTP `/api/pipeline/config`.

---

## Umbrales de alerta de presión

Definidos en el firmware como constantes de compilación (no requieren reflashear si se ajustan en `secrets.h`):

```cpp
#define PRESSURE_MIN_NORMAL    1.5f  // bar — por debajo: fuga grande o corte de suministro
#define PRESSURE_MAX_NORMAL    7.0f  // bar — por encima: sobrepresión, riesgo instalación
#define PRESSURE_DROP_ALERT    1.0f  // bar — caída en <2 s: fuga activa → corte automático
#define PRESSURE_HYDRO_TARGET  3.5f  // bar — presión de trabajo del hidropresor
#define PRESSURE_HYDRO_MARGIN  0.5f  // bar — desviación máxima aceptable del hidro
```

> **Ajuste para Lanzarote:** la red pública puede bajar de 2 bar en verano/horas punta. El umbral `PRESSURE_MIN_NORMAL = 1.5 bar` evita falsas alarmas en esos periodos.

La lógica de actuación (corte de relay, alerta MQTT) se implementa en el backend a partir del campo `pipeline_pressure` del payload de telemetría.

---

## Modo debug

Definir `-DDEBUG_MODE=1` al compilar (casilla "🐛 Debug" en la Flash Tool). Cada 10 s imprime por Serial un reporte de estado con solo los datos variables y advertencias activas. En producción la macro es un no-op — cero overhead en runtime.

---

## Ahorro energético

| Medida | Configuración | Ahorro estimado |
|--------|---------------|-----------------|
| CPU a 160 MHz | `setCpuFrequencyMhz(160)` | ~20% vs 240 MHz |
| Modem Sleep | `WiFi.setSleep(true)` tras conectar | ~15–20 mA entre transmisiones |
| Pantalla con timeout | Apagado automático tras 60s sin actividad | Variable según brillo |

---

## Detector automático de fugas (LeakDetector)

Clase `LeakDetector` en `LeakDetector.h`. Recibe muestras de presión y caudal cada **200 ms** y clasifica el estado de la tubería en tiempo real.

### Algoritmo

1. **Warm-up** (20 muestras con válvula abierta): aprende el baseline de presión y caudal mediante EMA (`α = 0.05`). `leak_detect_trained = false` hasta completar el entrenamiento.
2. **Detección activa** una vez entrenado:
   - **Válvula cerrada**: caudal > `leak_idle_threshold_lpm` durante **3 muestras consecutivas** → `"leak"`
   - **Válvula abierta**: caída de presión ≥ `burst_pressure_drop_pct` durante **2 muestras consecutivas** → `"burst"`; exceso de caudal ≥ `leak_on_deviation_pct` → `"leak"`; caída de caudal ≥ `obstruction_flow_drop_pct` durante **2 muestras consecutivas** → `"obstruction"`
3. **Normal** en el resto de casos: actualiza EMA con la muestra nueva.

Los contadores de confirmación (`BURST_CONFIRM = 2`, `OBSTR_CONFIRM = 2`, `IDLE_CONFIRM = 3`) evitan falsos positivos por picos de ruido.

### Perfiles de riego predefinidos

| Perfil | `irrigation_type` | Presión nominal | Caudal nominal |
|--------|:-----------------:|:---------------:|:--------------:|
| Aspersión (defecto) | `sprinkler` | 2.80 bar | 5.00 L/min |
| Goteo | `drip` | 1.50 bar | 2.00 L/min |
| Cinta de goteo | `drip_tape` | 0.80 bar | 0.80 L/min |
| Microaspersión | `micro_sprinkler` | 2.20 bar | 3.50 L/min |

El tipo se configura vía MQTT (`irrigation_type`) o HTTP `/api/pipeline/config`. El detector reinicia su baseline al cambiar de perfil.

### Campos de telemetría del detector

| Campo | Tipo | Descripción |
|-------|------|-------------|
| `pipeline_scenario` | string | `normal` / `leak` / `burst` / `obstruction` |
| `leak_detect_trained` | bool | `true` cuando el baseline EMA está entrenado |
| `leak_baseline_pressure` | float | Baseline EMA de presión (bar) |
| `leak_baseline_flow` | float | Baseline EMA de caudal (L/min) |
| `leak_warmup_progress` | int | Muestras de warm-up completadas (0–20) |
| `pipeline_pressure_ok` | bool | Sensor de presión real activo |
| `pipeline_flow_ok` | bool | Caudalímetro real activo |
| `pipeline_source` | string | `real` / `real_flow` / `sim` / `fallback` |
| `flow_total_l` | float | Litros acumulados desde arranque (resolución ±100 mL) |
| `flow_session_l` | float | Litros desde la última apertura de válvula (resetea en OFF→ON) |
| `flow_irrig_l` | float | Litros acumulados con relay activo (riego) |
| `flow_leak_l` | float | Litros acumulados con relay cerrado (posible fuga de fondo) |

> **Nota sobre contadores de pulsos:** los cuatro contadores de caudal se acumulan internamente como `uint32_t` (pulsos) para evitar pérdida de precisión float en largos periodos. El backend puede pedir su reset enviando `reset_flow_counters: true` en la respuesta de `/api/pipeline/config`; el firmware los borra y responde con un ACK a `POST /api/flow/reset-ack?mac=<MAC>`.

> **Cambio de perfil:** al cambiar `irrigation_type` (vía MQTT o HTTP), el LeakDetector reinicia su baseline EMA automáticamente, descartando el entrenamiento previo e iniciando un nuevo warm-up de 20 muestras.

---

## Problemas conocidos

| Problema | Estado |
|----------|--------|
| DHT11 lecturas inestables ocasionalmente | Conocido — valorar reemplazar por DHT22 o SHT31 |
| Temperatura agua XDB401 puede reflejar T ambiente si la tubería está seca | Esperado — documentar en dashboard |
| **GPIO16 (TFT_DC) no debe usarse como RX de Serial2** | **Resuelto** — Serial2 RX movido a GPIO13. Requiere reconectar el hilo DI del adaptador RS485 de GPIO16 a GPIO13 en el hardware. |
| GPIO35 sin pull-up interno puede provocar ciclo de vistas espontáneo | **Resuelto** — debounce de 400 ms en la detección de flanco de botón |

---

## Versionado y flujo de trabajo

### Estructura de ramas

```
main              ← producción (solo merges de release/* o hotfix/*)
develop           ← integración continua
feature/*         ← nueva funcionalidad (sale de develop, merge a develop)
release/vX.Y.Z    ← congelado para pruebas (sale de develop)
hotfix/*          ← parche urgente sobre main
```

**Regla principal:** nunca se trabaja directamente en `main`. Todo cambio pasa por `develop` y, cuando llega a producción, lo hace a través de una rama `release/`.

### Versionado semántico (SemVer)

El firmware sigue `MAJOR.MINOR.PATCH[-prerelease]`:

| Incremento | Cuándo |
|------------|--------|
| `PATCH` | Corrección de bug sin cambio de API/protocolo |
| `MINOR` | Nueva funcionalidad compatible con backend anterior |
| `MAJOR` | Cambio de protocolo MQTT o HTTP incompatible |

Ejemplos de ciclo: `v0.1.0-beta.1` → `v0.1.0-rc.1` → `v0.1.0` → `v0.1.1` → `v0.2.0`

### Cómo actualizar la versión del firmware

La versión se define en una sola línea al inicio de `ESP_monitor_server.ino`:

```cpp
#define FIRMWARE_VERSION "0.1.0-beta.2"
```

Este valor se envía automáticamente al backend en dos momentos:
- **Arranque**: mensaje MQTT `aquantia/<finca_id>/register` → campo `firmware_version`
- **Registro HTTP**: POST `/api/device_info` → campo `firmware_version`

El backend lo almacena en `device_info.firmware_version` y lo muestra como badge en el dashboard.

### Proceso de release

```bash
# 1. Crear rama de release desde develop
git checkout develop && git pull
git checkout -b release/v0.2.0

# 2. Actualizar FIRMWARE_VERSION en el .ino
#    Editar: #define FIRMWARE_VERSION "0.2.0-rc.1"

# 3. Actualizar CHANGELOG.md con los cambios

# 4. Commit de cierre de release
git add ESP_monitor_server/ESP_monitor_server.ino CHANGELOG.md
git commit -m "chore: bump firmware to v0.2.0-rc.1"

# 5. Etiquetar
git tag -a v0.2.0-rc.1 -m "Release candidate v0.2.0-rc.1"
git push origin release/v0.2.0 --tags

# 6. Tras validación, merge a main y develop
git checkout main && git merge --no-ff release/v0.2.0
git tag -a v0.2.0 -m "Release v0.2.0"
git push origin main --tags
git checkout develop && git merge --no-ff release/v0.2.0
git push origin develop
```

### Compatibilidad firmware ↔ backend

Ambos repositorios se versionan de forma independiente pero coordinada.
El backend mantiene en `app_settings` la clave `min_firmware_version` con la versión mínima aceptada. Cuando se introduce un cambio de protocolo incompatible, se incrementa `MAJOR` en ambos repos y se actualiza ese valor.

| Firmware | Backend app_meteo | Estado |
|----------|-------------------|--------|
| `v0.1.x` | `v0.1.x` | Compatible |
| `v0.2.0` | `v0.1.x` | Puede no funcionar — revisar CHANGELOG |

Ver también: [CHANGELOG.md](CHANGELOG.md) y el [README del backend](../app_meteo/app_meteo/README.md).
