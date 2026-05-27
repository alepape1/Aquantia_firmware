# Aquantia — Pinout de dispositivos

Dos perfiles de hardware, un mismo firmware. El perfil se selecciona en tiempo de compilación con `DEVICE_PROFILE`.

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
| LED onboard | 2 | Activo-LOW. Estados: parpadeo rápido=WiFi buscando, doble parpadeo=MQTT pendiente, latido=idle, triple=TX OK, 1s/1s=error |
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
| IRRIGATION | 13         | 14         | —     | DE/RE automático (no conectar) |

> **Conflicto GPIO16:** `TFT_DC = 16` en `Setup25_TTGO_T_Display.h`. No usar para RX.

```
Adaptador RS485 (MAX485 o similar)
┌───────────┐
│  VCC ─────┼──► 3.3V
│  GND ─────┼──► GND
│  DI  ─────┼──► GPIO14  (TX — ESP32 → sensor) [IRRIGATION]
│  RO  ─────┼──► GPIO13  (RX — sensor → ESP32)
│  DE/RE  ──┼──► (solo METEO: GPIO27; IRRIGATION: automático, no conectar)
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
| RS485 Serial2 RX (DI) | 13 | Helissense sensor suelo — RX (ESP32 ← sensor) |
| RS485 Serial2 TX (RO) | 14 | Helissense sensor suelo — TX (ESP32 → sensor) |

> Este perfil no tiene sensores meteorológicos ni pantalla. Solo gestiona los 4 relays por MQTT.

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
│  GPIO32 ─────────────┼──► IN1 relay 1 (zona riego 1)
│  GPIO33 ─────────────┼──► IN2 relay 2 (zona riego 2)
│  GPIO25 ─────────────┼──► IN3 relay 3 (zona riego 3)
│  GPIO26 ─────────────┼──► IN4 relay 4 (zona riego 4)
│  GPIO23 ─────────────┼──► LED estado (integrado)
│  GPIO13 ─────────────┼──► RS485 RO (RX — sensor Helissense → ESP32)
│  GPIO14 ─────────────┼──► RS485 DI (TX — ESP32 → sensor Helissense)
│  VCC / GND ──────────┼──► Alimentación 5 V / GND común
└────────────────────────────┘

*Para módulos MAX485 con DE/RE automático, no conectar ni definir pin de control DE/RE.*

Cada relay:
  COM ──► neutro / positivo de la válvula
  NO  ──► electroválvula 24 VAC / 12 VDC
```

---

## Caudalímetro de tubería (PROFILE_METEO y PROFILE_AGROMETEO)

El firmware implementa la lectura de caudal mediante **interrupciones hardware** para no perder ningún pulso.

### GPIO y circuito

| Perfil | Función | GPIO | Notas |
|--------|---------|:----:|-------|
| METEO (LilyGo TTGO T-Display) | Caudalímetro (pulsos) | **32** | INPUT_PULLUP, ISR FALLING edge |
| AGROMETEO (WEMOS D1 MINI ESP32) | Caudalímetro (pulsos) | **17** | INPUT_PULLUP, ISR FALLING edge — sin función especial, libre en esta placa |

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
| `FLOW_K_FACTOR` | `450` | Pulsos por litro — YF-S201; **ajustar según sensor real** con agua calibrada |

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

## Comportamiento del relay (ambos perfiles)

Los relays **JQC-3FF-S-Z** son **activo-LOW**:

| GPIO | Estado | Relay | Válvula |
|------|--------|:-----:|---------|
| HIGH | Arranque / seguro | OFF | Cerrada |
| LOW | Activado | ON | Abierta |

Durante una actualización OTA todos los relays pasan a OFF (HIGH) automáticamente por seguridad.

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
