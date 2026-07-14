#pragma once
// =============================================================================
// ENS160 — Sensor digital de calidad de aire IAQ (TVOC, eCO2, AQI UBA)
// I2C: 0x52 (ADDR pin = GND, default módulo AliExpress) o 0x53 (ADDR = VCC)
// Comb board ENS160+AHT21: ENS160@0x52, AHT21@0x38 (compatible con aht20_driver.h).
// Driver directo via Wire — sin librería externa.
// Salidas:
//   AQI   — UBA Air Quality Index 1 (excelente) a 5 (insalubre)
//   TVOC  — compuestos orgánicos volátiles totales (ppb, 0-65000)
//   eCO2  — CO2 equivalente (ppm, 400-65000)
// Requiere ~1 min de calentamiento inicial para lecturas estables (warm-up=1).
// =============================================================================

#define ENS160_ADDR_LOW   0x52   // ADDR pin = GND (predeterminado)
#define ENS160_ADDR_HIGH  0x53   // ADDR pin = VCC

// Registros
#define ENS160_REG_PART_ID  0x00   // 2 bytes LE → 0x0160 para ENS160
#define ENS160_REG_OPMODE   0x10   // modo de operación (R/W)
#define ENS160_REG_STATUS   0x20   // estado del dispositivo
#define ENS160_REG_AQI      0x21   // UBA AQI (1 byte, bits[2:0])
#define ENS160_REG_TVOC     0x22   // TVOC en ppb (2 bytes LE)
#define ENS160_REG_ECO2     0x24   // eCO2 en ppm eq. (2 bytes LE)
#define ENS160_REG_TEMP_IN  0x13   // temperatura de compensación (2 bytes LE)
#define ENS160_REG_RH_IN    0x15   // humedad de compensación (2 bytes LE)

// Valores de OPMODE
#define ENS160_OPMODE_RESET    0xF0   // reset software
#define ENS160_OPMODE_IDLE     0x01
#define ENS160_OPMODE_STANDARD 0x02   // operación estándar (1 Hz)

// Validity bits[3:2] de STATUS
#define ENS160_VALIDITY_NORMAL     0   // operación normal
#define ENS160_VALIDITY_WARMUP     1   // calentamiento (primeras ~3 mediciones)
#define ENS160_VALIDITY_INIT_START 2   // arranque inicial (<1 min tras power-on)
#define ENS160_VALIDITY_INVALID    3   // salida inválida

static uint8_t _ens160_addr = 0;   // dirección I2C activa (0 = no detectado)

static bool _ens160_write(uint8_t reg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(_ens160_addr);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

static bool _ens160_read(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(_ens160_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  Wire.requestFrom((uint8_t)_ens160_addr, len);
  if (Wire.available() < (int)len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

// Detecta ENS160 en 0x52 o 0x53 verificando PART_ID. Devuelve la dirección o 0.
static uint8_t ens160_detect() {
  const uint8_t addrs[] = {ENS160_ADDR_LOW, ENS160_ADDR_HIGH};
  for (uint8_t a : addrs) {
    _ens160_addr = a;
    uint8_t buf[2] = {};
    if (_ens160_read(ENS160_REG_PART_ID, buf, 2)) {
      uint16_t pid = ((uint16_t)buf[1] << 8) | buf[0];
      if (pid == 0x0160) {
        DLOGF("[ENS160] Detectado en 0x%02X (PART_ID=0x%04X)\n", a, pid);
        return a;
      }
    }
  }
  _ens160_addr = 0;
  return 0;
}

// Inicializa el ENS160: detecta dirección, reset software, activa modo estándar.
// Devuelve true si el sensor responde y se configura correctamente.
static bool ens160_begin() {
  if (!ens160_detect()) return false;

  uint8_t rst = ENS160_OPMODE_RESET;
  _ens160_write(ENS160_REG_OPMODE, &rst, 1);
  delay(20);

  uint8_t std_mode = ENS160_OPMODE_STANDARD;
  if (!_ens160_write(ENS160_REG_OPMODE, &std_mode, 1)) {
    _ens160_addr = 0;
    return false;
  }
  delay(50);
  return true;
}

// Escribe temperatura (°C) y humedad relativa (%) para compensación interna.
// Mejora la precisión del algoritmo de fusión interno del ENS160.
// Llamar antes de ens160_read() con valores actualizados de T y HR.
static void ens160_set_compensation(float tempC, float rh) {
  if (!_ens160_addr || isnan(tempC) || isnan(rh)) return;
  uint16_t t_raw  = (uint16_t)((tempC + 273.15f) * 64.0f);
  uint16_t rh_raw = (uint16_t)(rh * 512.0f);
  uint8_t tbuf[2] = { (uint8_t)(t_raw & 0xFF),  (uint8_t)(t_raw >> 8)  };
  uint8_t rbuf[2] = { (uint8_t)(rh_raw & 0xFF), (uint8_t)(rh_raw >> 8) };
  _ens160_write(ENS160_REG_TEMP_IN, tbuf, 2);
  _ens160_write(ENS160_REG_RH_IN,   rbuf, 2);
}

// Lee AQI (1-5), TVOC (ppb) y eCO2 (ppm eq.).
// Acepta validity=0 (normal) y validity=1 (warm-up — primeras lecturas tras arranque).
// Devuelve false si hay error I2C o validity=3 (inválido).
static bool ens160_read(uint8_t& outAqi, uint16_t& outTvoc, uint16_t& outEco2) {
  if (!_ens160_addr) return false;

  uint8_t status = 0;
  if (!_ens160_read(ENS160_REG_STATUS, &status, 1)) return false;

  uint8_t validity = (status >> 2) & 0x03;
  if (validity == ENS160_VALIDITY_INVALID) return false;

  uint8_t buf[2];

  // AQI — bits[2:0]
  if (!_ens160_read(ENS160_REG_AQI, buf, 1)) return false;
  outAqi = buf[0] & 0x07;

  // TVOC — 2 bytes LE
  if (!_ens160_read(ENS160_REG_TVOC, buf, 2)) return false;
  outTvoc = ((uint16_t)buf[1] << 8) | buf[0];

  // eCO2 — 2 bytes LE
  if (!_ens160_read(ENS160_REG_ECO2, buf, 2)) return false;
  outEco2 = ((uint16_t)buf[1] << 8) | buf[0];

  return true;
}
