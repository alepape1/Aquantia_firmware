# Aquantia — Pinout de dispositivos

Cuatro perfiles de hardware, un mismo firmware. El perfil se selecciona en tiempo de compilación con `DEVICE_PROFILE`.

---

## PROFILE_METEO (1) — LilyGo TTGO T-Display

**Placa:** LilyGo TTGO T-Display (ESP32-D0WDQ6, 4 MB flash, pantalla ST7789 240×135 integrada)

### GPIO del firmware

| Función | GPIO | Notas |
|---------|:----:|-------|
| I2C SDA | 21 | Bus de sensores |
| I2C SCL | 22 | Bus de sensores |
| DHT11 datos | 15 | Pull-up 4.7 kΩ a 3.3 V |
| Anemómetro (ADC) | 37 | 0–3.3 V → 0–30 m/s, ADC1_CH1 |
| Veleta dirección (ADC) | 36 | 0–3.3 V → 0–360°, ADC1_CH0 (input-only) |
| Humedad suelo YL-69 (ADC) | 33 | ADC1_CH5, divisor de tensión |
| Caudalímetro (pulsos ISR) | 32 | INPUT_PULLUP, ISR FALLING (BC547 NPN, señal invertida) |
| Relay 1 (electroválvula) | 26 | Activo-LOW, JQC-3FF-S-Z |
| Botón izquierdo (BOOT) | 0 | INPUT_PULLUP, activo LOW |
| Botón derecho | 35 | INPUT, activo LOW — sin pull-up interno; debounce 400 ms en firmware |
| LED onboard | — | Sin LED accesible en LilyGo T-Display (`LED_PIN = -1`) |
| RS485 Serial2 RX (DI) | **13** | Helissense sensor suelo — **NO usar GPIO16 (= TFT_DC)** |
| RS485 Serial2 TX (RO) | 17 | Helissense sensor suelo |
| RS485 DE/RE | 27 | Control half-duplex Helissense |

### Pantalla TFT — SPI (ST7789 240×135)

Configurada mediante `TFT_eSPI` → `User_Setups/Setup25_TTGO_T_Display.h`. No modificar en el código del sketch.

| Señal TFT | GPIO |
|-----------|:----:|
| MOSI (SDA) | 19 |
| SCLK (SCK) | 18 |
| CS | 5 |
| DC (RS) | 16 |
| RST | 23 |
| Backlight (PWM) | 4 |

### Sensores I2C (SDA=21, SCL=22)

| Sensor | Dirección I2C | Función |
|--------|:-------------:|---------|
| Adafruit MCP9808 | 0x19 | Temperatura exterior (±0.0625 °C) |
| SparkFun MicroPressure | 0x18 | Presión barométrica |
| HTU2x (HTU21D / HTU20D) | 0x40 | Temperatura + humedad relativa |
| TSL2584 / APDS-9930 (clon) | 0x39 | Luz ambiente (lux) — autodetección |

> Sensores confirmados en hardware real con I2C scanner: `0x18, 0x19, 0x39, 0x40`

### Diagrama de conexiones externas

```
LilyGo TTGO T-Display
┌────────────────────┐
│  3V3 ──────────────┼──► VCC sensores I2C / DHT11 / divisor YL-69 / adaptador RS485
│  GND ──────────────┼──► GND común
│  GPIO21 (SDA) ─────┼──► SDA → MCP9808, MicroPressure, HTU2x, TSL2584
│  GPIO22 (SCL) ─────┼──► SCL → (mismos sensores)
│  GPIO15 ───────────┼──► DHT11 DATA  (pull-up 4.7kΩ a 3.3V)
│  GPIO37 ───────────┼──► Anemómetro salida analógica 0–3.3V
│  GPIO36 ───────────┼──► Veleta salida analógica 0–3.3V
│  GPIO33 ───────────┼──► YL-69 AO (tras divisor de tensión) — fallback suelo
│  GPIO32 ───────────┼──► Caudalímetro (colector BC547 NPN)
│  GPIO26 ───────────┼──► IN del relay (JQC-3FF-S-Z)
│  GPIO0  ───────────┼──► Botón BOOT (ya integrado en placa)
│  GPIO35 ───────────┼──► Botón derecho externo
│  GPIO13 ───────────┼──► RS485 DI → RX Helissense  ← usar GPIO13, NO GPIO16
│  GPIO17 ───────────┼──► RS485 RO ← TX Helissense
│  GPIO27 ───────────┼──► RS485 DE/RE (control half-duplex)
│  GPIO16 ── TFT_DC ─┼   (interno, NO conectar nada aquí)
└────────────────────┘
```


### Sensor de suelo RS485 — Helissense (Modbus RTU)

Sensor 7-en-1: humedad, temperatura, CE, pH, TDS, N, P, K. Conectado a Serial2 mediante un adaptador RS485 half-duplex TTL.

| Perfil      | RX (ESP32) | TX (ESP32) | DE/RE | Notas |
|------------|:----------:|:----------:|:-----:|-------|
| METEO      | 13         | 17         | 27    | DE/RE manual (GPIO27) |
| IRRIGATION | 14         | 13         | 27    | DE/RE en GPIO27 |

> **Conflicto GPIO16:** `TFT_DC = 16` en `Setup25_TTGO_T_Display.h`. No usar para RX.

```
Adaptador RS485 (MAX485 o similar)
┌───────────┐
│  VCC ─────┼──► 3.3V
│  GND ─────┼──► GND
│  DI  ─────┼──► GPIO17  (TX — ESP32 → sensor) [METEO]
│         ──┼──► GPIO13  (TX — ESP32 → sensor) [IRRIGATION]
│  RO  ─────┼──► GPIO13  (RX — sensor → ESP32) [METEO]
│         ──┼──► GPIO14  (RX — sensor → ESP32) [IRRIGATION]
│  DE/RE  ──┼──► GPIO27 (ambos perfiles)
│  A/B ─────┼──► Bus RS485 al sensor Helissense
└───────────┘
```

Parámetros de comunicación: **4800 baud, 8N1**, dirección Modbus esclavo `0x01`, registro inicial `0x0000`, 7 registros.

### Calibración del sensor de suelo (YL-69)

Ajustar en `ESP_monitor_server.ino` según el sensor real:

```cpp
#define SOIL_RAW_DRY   3300   // Lectura ADC en tierra seca   (~0%)
#define SOIL_RAW_WET   1000   // Lectura ADC en tierra húmeda (~100%)
```

---

## PROFILE_IRRIGATION (2) — ESP32 4-Relay Board

**Placa:** ESP32-WROOM-32E (ESP32-D0WD-V3)
**Referencia:** [ESP32 Relay x4 — ESPHome devices](https://devices.esphome.io/devices/ESP32-Relay-x4/)

### GPIO del firmware

| Función | GPIO | Notas |
|---------|:----:|-------|
| Relay 1 (zona 1) | 32 | Activo-LOW, JQC-3FF-S-Z |
| Relay 2 (zona 2) | 33 | Activo-LOW |
| Relay 3 (zona 3) | 25 | Activo-LOW |
| Relay 4 (zona 4) | 26 | Activo-LOW |
| LED estado | 23 | Activo-LOW. Mismos estados que METEO + encendido fijo cuando relay activo |
| I2C SDA | 21 | Sin sensores en v actual |
| I2C SCL | 22 | Sin sensores en v actual |
| Caudalímetro (pulsos ISR) | 34 | ADC1_CH6, solo entrada — sin pull-up interno; requiere pull-up externo, ISR FALLING |
| RS485 Serial2 RX (RO) | 14 | Helissense sensor suelo — RX (ESP32 ← sensor) |
| RS485 Serial2 TX (DI) | 13 | Helissense sensor suelo — TX (ESP32 → sensor) |
| RS485 DE/RE | 27 | Control half-duplex Helissense |

> Este perfil no tiene pantalla. Gestiona los 4 relays por MQTT y lee temperatura/humedad ambiente (AHT20), voltaje/corriente/potencia (INA219) y presión atmosférica (BMP280) por I2C.

### Sensores I2C (SDA=21, SCL=22)

| Sensor | Dirección I2C | Función |
|--------|:-------------:|---------|
| AHT20 | 0x38 | Temperatura + humedad ambiente — driver en `aht20_driver.h`, sin librería externa |
| INA219 | 0x40 | Voltaje de bus, corriente y potencia — driver en `ina219_driver.h`, sin librería externa |
| BMP280 | 0x76 / 0x77 | Temperatura + presión atmosférica (fallback si AHT20 no responde) |

> **INA219:** configurado para 32 V bus, ±2 A, ADC 12-bit. Resistencia shunt 0.1 Ω (breakout estándar).
> - `current_lsb` = 0.1 mA/bit, `power_lsb` = 2 mW/bit
> - Dirección configurable via A0/A1: defecto 0x40 (A0=GND, A1=GND)

---

### Bitmask de relays

El campo `relay_active` en la telemetría y en los comandos es un bitmask de 4 bits:

| Valor | Estado |
|-------|--------|
| `0`  | Todos OFF |
| `1`  | Relay 1 ON (bit 0) |
| `2`  | Relay 2 ON (bit 1) |
| `4`  | Relay 3 ON (bit 2) |
| `8`  | Relay 4 ON (bit 3) |
| `15` | Los 4 relays ON |


### Diagrama de conexiones

```
ESP32 4-Relay Board
┌────────────────────────────┐
│  GPIO32 ─────────────────┼──► IN1 relay 1 (zona riego 1)
│  GPIO33 ─────────────────┼──► IN2 relay 2 (zona riego 2)
│  GPIO25 ─────────────────┼──► IN3 relay 3 (zona riego 3)
│  GPIO26 ─────────────────┼──► IN4 relay 4 (zona riego 4)
│  GPIO23 ─────────────────┼──► LED estado (integrado)
│  GPIO14 ─────────────────┼──► RS485 RO → RX Helissense (sensor → ESP32)
│  GPIO13 ─────────────────┼──► RS485 DI ← TX Helissense (ESP32 → sensor)
│  GPIO27 ─────────────────┼──► RS485 DE/RE (control half-duplex)
│  GPIO21 (SDA) ───────────┼──► SDA → AHT20, INA219, BMP280
│  GPIO22 (SCL) ───────────┼──► SCL → AHT20, INA219, BMP280
│  3V3 ────────────────────┼──► VCC sensores I2C / adaptador RS485
│  VCC / GND ──────────────┼──► Alimentación 5 V / GND común
└────────────────────────────┘

Cada relay:
  COM ──► neutro / positivo de la válvula
  NO  ──► electroválvula 24 VAC / 12 VDC
```

---

## PROFILE_AQUALEAK (3) — Wemos D1 Mini ESP32

**Placa:** Wemos D1 Mini ESP32 + módulo CJMCU-14 (BH1750 + HDC1080 + BMP280 + MicroPressure)
**Conectividad:** WiFi. 1 relay de corte. Caudalímetro YF-B9 (K_FACTOR = 288).

### GPIO del firmware

| Función | GPIO | Notas |
|---------|:----:|-------|
| I2C SDA | 21 | Bus sensores: BH1750, HDC1080, BMP280, MicroPressure, Qwiic Switch |
| I2C SCL | 22 | Bus sensores |
| Caudalímetro (pulsos ISR) | 17 | INPUT_PULLUP, ISR FALLING — GPIO libre, sin función especial |
| Relay 1 (corte electroválvula) | 26 | Activo-HIGH, JQC-3FF-S-Z |
| LED onboard | 2 | Activo-HIGH |

### Sensores I2C (SDA=21, SCL=22)

| Sensor | Dirección I2C | Función |
|--------|:-------------:|---------|
| BH1750 | 0x23 | Iluminancia (lux) |
| HDC1080 | 0x40 | Temperatura + humedad |
| BMP280 | 0x76/0x77 | Temperatura + presión atmosférica (fallback) |
| SparkFun MicroPressure | 0x18 | Presión atmosférica principal |
| Qwiic Power Switch (PCA9536) | 0x41 | Alimenta el bus I2C secundario antes de la lectura |

> Este perfil publica el campo adicional `dew_point` (punto de rocío calculado) en la telemetría.

### Diagrama de conexiones

```
Wemos D1 Mini ESP32
┌────────────────────┐
│  3V3 ──────────────┼──► VCC sensores I2C / Qwiic Switch
│  GND ──────────────┼──► GND común
│  GPIO21 (SDA) ─────┼──► SDA → BH1750, HDC1080, BMP280, MicroPressure
│  GPIO22 (SCL) ─────┼──► SCL → (mismos sensores)
│  GPIO17 ───────────┼──► Caudalímetro (colector BC547 NPN)
│  GPIO26 ───────────┼──► IN del relay (JQC-3FF-S-Z)
└────────────────────┘
```

---

## Caudalímetro de tubería (todos los perfiles)

El firmware implementa la lectura de caudal mediante **interrupciones hardware** para no perder ningún pulso.

### GPIO y circuito

| Perfil | GPIO | K_FACTOR | Sensor | Notas |
|--------|:----:|:--------:|--------|-------|
| METEO | 32 | 660 | YF-B4 | INPUT_PULLUP, ISR FALLING (BC547 NPN invierte señal) |
| IRRIGATION | 34 | 660 | YF-B4 | Input-only, sin pull-up interno — pull-up externo obligatorio, ISR FALLING |
| AQUALEAK | 17 | 288 | YF-B9 | INPUT_PULLUP, ISR FALLING (medido: ~9 L / 2678 pulsos) |
| AQUA_SMART_REMOTE | 34 | 660 | YF-B4 | Input-only, sin pull-up interno — pull-up externo obligatorio, ISR FALLING |

### Transistor de acondicionamiento BC547 NPN

El sensor de caudal genera pulsos a tensión mayor de 3.3 V. El BC547 la recorta e invierte para que el ESP32 la pueda leer de forma segura:

```
Sensor (pulso HIGH)
      │
     [R_base ~10kΩ]
      │
   Base BC547
   Colector ──────────┬── 3.3V (pull-up interno ESP32)
                      └── GPIO 32
   Emisor ────────────── GND
```

- Sensor pulsa HIGH → BC547 conduce → GPIO 32 baja a LOW → **FALLING edge = 1 pulso**
- Sensor en reposo LOW → BC547 corta → GPIO 32 sube a HIGH (pull-up)

La señal llega **invertida**: el firmware cuenta flancos `FALLING`.

### Parámetros de calibración (en `ESP_monitor_server.ino`)

| Constante | Valor por defecto | Descripción |
|-----------|:-----------------:|-------------|
| `FLOW_K_FACTOR` | `660` (METEO/IRRIGATION/AQUA_SMART_REMOTE) · `288` (AQUALEAK) | Pulsos por litro — **ajustar según sensor real** con agua calibrada |

Fórmula de cálculo:

$$\text{L/min} = \frac{\text{pulsos} \times 60}{t_{seg} \times K_{factor}}$$

### Modo de operación

El caudal real solo se usa cuando el firmware está en `pipelineMode = "real"` (activable por MQTT o HTTP desde el backend). En ese modo:
- **Caudal**: medido por ISR (GPIO 32) → `pipeline_flow_ok = true`
- **Presión**: estimada por el simulador (sin sensor de presión de tubería instalado aún) → `pipeline_pressure_ok = false`
- **Fuente**: `pipeline_source = "real"`

Cuando el modo es `"sim"` (defecto de fábrica):
- `pipeline_source = "sim"`, `pipeline_flow_ok = false`, `pipeline_pressure_ok = false`

Cuando el modo es `"real"` pero el caudalímetro no responde:
- `pipeline_source = "fallback"`, `pipeline_flow_ok = false`, `pipeline_pressure_ok = false`

Estos tres campos se almacenan en la base de datos del backend (`pipeline_source`, `pipeline_pressure_ok`, `pipeline_flow_ok`) y están disponibles en `/api/historico` y endpoint de pipeline.

> Perfil afectado: **solo PROFILE_METEO** (LilyGo TTGO T-Display). `FLOW_PIN` y `FLOW_K_FACTOR` están dentro del guard `#if DEVICE_PROFILE == PROFILE_METEO`.

---

## Comportamiento del relay (todos los perfiles)

Los relays **JQC-3FF-S-Z** son **activo-HIGH** (lote Aquantia):

| GPIO | Estado | Relay | Válvula |
|------|--------|:-----:|---------|
| LOW | Arranque / seguro | OFF | Cerrada |
| HIGH | Activado | ON | Abierta |

Durante una actualización OTA todos los relays pasan a OFF (HIGH) automáticamente por seguridad.

---

## PROFILE_AQUA_SMART_REMOTE (4) — LilyGO T-SIM7000G

**Placa:** LilyGO T-SIM7000G (ESP32, modem SIM7000G integrado, conectividad LTE-M/NB-IoT/GPRS)
**SIM:** Onomondo IoT SIM — APN `onomondo`, sin usuario ni contraseña
**Conectividad:** Celular (TinyGSM) — sin WiFi. Sensor suite idéntico a PROFILE_IRRIGATION.
**Pantalla:** La placa dispone de pantalla, a implementar en iteración futura.

### Identidad del dispositivo

El ESP32 tiene una MAC WiFi grabada en eFuse que **no cambia aunque WiFi no esté activo**. Esta MAC es el identificador principal en la API (campo `mac_address`, `device_mac`, `X-Device-MAC`). Para leerla sin inicializar WiFi el firmware usa:

```c
// provisioning.h — getDeviceMacAddress()
uint8_t mac[6];
esp_read_mac(mac, ESP_MAC_WIFI_STA);  // eFuse, sin WiFi
// → "FC:B4:67:F3:77:48"  — mismo formato que WiFi.macAddress()
```

El campo `device_serial` (formato `AQ-FCB467F37748`) es distinto del `mac_address` y se usa como client ID en MQTT y en el QR de etiqueta, no como clave de lookup en la API.

| Campo | Valor ejemplo | Usado en |
|-------|--------------|----------|
| `mac_address` | `FC:B4:67:F3:77:48` | API REST, MQTT telemetry, alerts |
| `device_serial` | `AQ-FCB467F37748` | MQTT client ID, QR etiqueta, logs |
| MQTT client ID | `aquantia-FCB467F37748` | Broker (mismos bytes que mac, sin colons) |

### Arranque y conectividad cellular

Secuencia de boot en `setup()`:
1. `sim7000g_powerOn()` — DTR LOW → pulso PWRKEY 1 s → Serial1@115200 → `modemSIM.init()` → `waitForNetwork(60 s)` → `gprsConnect("onomondo")`
2. Tiempo de red: `modemSIM.getNetworkTime()` → `settimeofday()` (en lugar de NTP)
3. MQTT TLS: `gsmTLSClient.setCACert(MQTT_CA_CERT_PEM)` → `mqttClient.setClient(gsmTLSClient)`
4. `networkTask` en Core 0 gestiona reconexión GPRS y MQTT

RSSI reportado como CSQ (0–31) via `modemSIM.getSignalQuality()` en campo `rssi` de telemetría.

### GPIO del firmware

| Función | GPIO | Notas |
|---------|:----:|-------|
| I2C SDA | 21 | Bus sensores: AHT20, BMP280, INA219 |
| I2C SCL | 22 | Bus sensores |
| Caudalímetro (ISR) | 34 | ADC1_CH6, solo entrada — sin pull-up interno; requiere pull-up externo |
| Relay 1 | 32 | Libre, sin conflicto con modem |
| Relay 2 | 33 | Libre |
| Relay 3 | 16 | Libre |
| Relay 4 | 17 | Libre |
| RS485 TX (DI→sensor) | 19 | GPIO libre — SD card intacta |
| RS485 RX (RO←sensor) | 18 | GPIO libre — SD card intacta |
| RS485 DE/RE | 23 | Control half-duplex Helissense |
| MODEM TX (AT→modem) | 27 | Serial1 |
| MODEM RX (AT←modem) | 26 | Serial1 |
| MODEM DTR | 25 | LOW = modem despierto |
| MODEM PWRKEY | 4 | Pulso 1000 ms para encender |
| LED onboard | 12 | Activo-LOW (BOARD_LED_PIN) |
| BAT ADC | 35 | Tensión batería — input-only |
| SOLAR ADC | 36 | Tensión panel solar — input-only |

### Pines SD card (intactos, disponibles)

| Señal SD | GPIO |
|----------|:----:|
| MISO | 2 |
| CS | 13 |
| SCK | 14 |
| MOSI | 15 |

### Sensores I2C (SDA=21, SCL=22)

| Sensor | Dirección | Función |
|--------|:---------:|---------|
| AHT20 | 0x38 | Temperatura + humedad ambiente |
| INA219 | 0x40 | Voltaje bus / corriente / potencia |
| BMP280 | 0x76/0x77 | Temperatura + presión atmosférica (fallback) |

### Sensor de suelo RS485 — Helissense (Modbus RTU)

| Parámetro | Valor |
|-----------|-------|
| Serial | Serial2 |
| RX (ESP32←sensor) | GPIO 18 |
| TX (ESP32→sensor) | GPIO 19 |
| DE/RE | GPIO 23 |
| Baudrate | 4800 |

### Compilación

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "build.extra_flags=-DDEVICE_PROFILE=4" \
  ESP_monitor_server/ESP_monitor_server.ino
```

---

## Anemómetro y veleta (PROFILE_METEO)

### Anemómetro — velocidad del viento

Salida analógica 0–3.3 V proporcional a 0–30 m/s:

| Parámetro | Valor |
|-----------|-------|
| Pin | GPIO37 (ADC1_CH1) |
| Resolución ADC | 12 bit (0–4095) |
| Referencia | 3.41 V |
| Fórmula | `speed = (raw / ADC_RANGE) * ADC_VOLTAGE_REF / 3.3 * 30.0` |
| Filtro | Media móvil circular de 10 muestras, muestreo cada 100 ms |

### Veleta — dirección del viento

Salida analógica 0–3.3 V proporcional a 0–360°:

| Parámetro | Valor |
|-----------|-------|
| Pin | GPIO36 (ADC1_CH0, input-only) |
| Resolución ADC | 12 bit (0–4095) |
| Filtro | Promedio vectorial (`atan2` de suma de vectores unitarios) |

> GPIO36 es input-only en ESP32 — no conectar como salida.

---

## DHT11 — sensor secundario (PROFILE_METEO)

Temperatura y humedad de respaldo, independiente del bus I2C.

| Parámetro | Valor |
|-----------|-------|
| Pin | GPIO15 |
| Pull-up | 4.7 kΩ entre DATA y 3.3 V |
| Librería | `DHTesp` (beegee-tokyo) |

> Sensor **primario** de temperatura y humedad: HTU2x por I2C 0x40.
> DHT11 aparece como campos adicionales `dht_temperature` / `dht_humidity` en la telemetría.

---

## Alimentación

- Todos los sensores I2C y el DHT11 operan a **3.3 V**.
- El relay JQC-3FF-S-Z acepta bobina a 5 V (alimentado desde la placa, no desde el GPIO).
- Las electroválvulas conectadas a los NO del relay operan habitualmente a **24 VAC** o **12 VDC** con fuente externa.
- Las líneas I2C trabajan a 3.3 V — **no conectar a 5 V**.
