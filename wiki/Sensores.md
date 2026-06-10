# Sensores soportados

Todos los drivers son nativos (sin librerías de terceros), salvo los indicados.

---

## Sensores por perfil

| Sensor | Perfil | Bus | Driver | Magnitudes publicadas |
|--------|--------|-----|--------|-----------------------|
| **MCP9808** | METEO | I2C 0x18 | nativo | `temperature` (°C) |
| **HTU21 / HTU2x** | METEO | I2C 0x40 | `htu2x_driver.h` | `humidity` (%), `temperature` (°C) |
| **DHT11** | METEO | GPIO | Adafruit DHT | `dht_temperature`, `dht_humidity` (fallback) |
| **BMP280** | METEO / IRRIGATION / AQUALEAK / AQUA_SMART_REMOTE | I2C 0x76/0x77 | Adafruit BMP280 | `pressure` (hPa), `temperature` (°C, fallback) |
| **TSL2584 / APDS-9930** | METEO | I2C — autodetect por ID nibble | `light_sensor.h` | `lux` |
| **MicroPressure** | METEO / AQUALEAK | I2C 0x18 | SparkFun | `pipe_pressure` (bar) |
| **YL-69** | METEO | ADC | nativo | `soil_humidity_raw` (mV, fallback suelo) |
| **Anemómetro ADC** | METEO | ADC | `wind_sensor.h` | `wind_speed` (m/s), media móvil 10 muestras |
| **Veleta ADC** | METEO | ADC | `wind_sensor.h` | `wind_dir` (°), promedio vectorial |
| **Caudalímetro YF-B9** | IRRIGATION / AQUALEAK / AQUA_SMART_REMOTE | GPIO ISR | `pipeline_core.h` | `flow_lpm` (L/min), `flow_total_l`, `flow_session_l` |
| **XDB401 (XGZP6847D)** | IRRIGATION / AQUALEAK / AQUA_SMART_REMOTE | I2C 0x6D/0x7F | `pressure_sensor_i2c.h` | `pipe_pressure` (bar). Sentinel `NAN` si falla |
| **AHT20** | IRRIGATION / AQUA_SMART_REMOTE | I2C 0x38 | `aht20_driver.h` | `temperature` (°C), `humidity` (%) |
| **INA219** | IRRIGATION / AQUA_SMART_REMOTE | I2C 0x40 | `ina219_driver.h` | `ina219_bus_voltage` (V), `ina219_current_ma` (mA), `ina219_power_mw` (mW) |
| **HDC1080** | AQUALEAK | I2C 0x40 | `hdc1080_driver.h` | `temperature` (°C), `humidity` (%), `dew_point` (°C) |
| **BH1750** | AQUALEAK | I2C 0x23 | nativo | `lux` |
| **Helissense RS485** | METEO / IRRIGATION / AQUA_SMART_REMOTE | UART RS485 4800 8N1 | `SoilSensor.h` | `soil_temp` (°C), `soil_humidity` (%), `soil_ec` (µS/cm), `soil_ph`, `soil_n/p/k` (mg/kg) |

---

## Cadena de fallback

| Magnitud | Fuente 1 | Fuente 2 | Fuente 3 |
|----------|----------|----------|----------|
| Temperatura | MCP9808 | BMP280 | Simulado con drift |
| Presión atmosférica | MicroPressure | BMP280 | Simulado |
| Humedad relativa | HTU2x / AHT20 / HDC1080 | — | Simulado |
| Iluminancia | TSL2584 / APDS-9930 | BH1750 | Simulado |
| Suelo | Helissense RS485 | YL-69 ADC | Simulado |

---

## Sensor de suelo Helissense RS485

- **Baud rate:** 4800 (consolidado; no usar 9600).
- **Protocolo:** Modbus RTU, comandos construidos dinámicamente (CRC en runtime).
- **Dirección Modbus:** guardada en NVS (`soil_bus/addr`). Default 0x01 si no hay NVS.
- **Auto-provisioning:** `SoilProvisioner.h` — `soilBusScan()` escanea direcciones 1–8,
  `soilBusProvision()` asigna dirección libre y la guarda en NVS.
  Activable por MQTT con `{"cmd": "provision_soil"}` o desde el Flash Tool.
- **Inicio correcto:** llamar `soilBusLoadAddress()` en boot **antes** de `soilSensor.begin(4800)`.

Ver también: [Uso — SoilProvisioner](Uso#soilprovisioner--auto-provisioning-del-sensor-de-suelo)

---

## Sensor de presión de tubería XDB401 (XGZP6847D)

- Detección automática de dirección I2C: 0x6D o 0x7F.
- Lectura de 5 bytes: presión 24 bit + temperatura 16 bit.
- Sentinel de fallo: `NAN` (nunca `-1.0f`) — permite distinguir presión negativa real de fallo.
- Gestión de fallos centralizada en `readXDB401Safe()`: tras `XDB401_MAX_FAILURES` fallos
  consecutivos, el sensor se suspende `XDB401_RETRY_INTERVAL` ms antes del siguiente intento.

---

## INA219 — Monitor de batería/panel solar

Publica tres campos en telemetría (perfiles IRRIGATION y AQUA_SMART_REMOTE):

| Campo MQTT | Unidad | Descripción |
|------------|--------|-------------|
| `ina219_bus_voltage` | V | Tensión en el bus (panel / batería) |
| `ina219_current_ma` | mA | Corriente consumida (positivo = carga) |
| `ina219_power_mw` | mW | Potencia calculada (V × I) |

---

## pH Helissense — escala correcta

El sensor devuelve pH ×100. El driver divide entre `100.0f` (no `10.0f`).
Si ves valores de pH × 10 en el backend, el firmware es anterior a `fix(firmware): pH scale`.
