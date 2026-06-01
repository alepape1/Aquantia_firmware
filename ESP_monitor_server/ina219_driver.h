#pragma once
// =============================================================================
// INA219 — voltaje de bus, corriente y potencia I2C (solo PROFILE_IRRIGATION)
// Dirección configurable via A0/A1: 0x40 (A0=GND, A1=GND) por defecto.
// Driver directo via Wire.  Asume resistencia shunt de 0.1 Ω (breakout estándar).
//
// Configuración: 32 V bus, ±2 A, ADC 12-bit, promedio 1 muestra.
//   Reg Config  (0x00): 0x399F
//   Reg Cal     (0x05): 0x1000  → current_lsb = 0.1 mA/bit, power_lsb = 2 mW/bit
// =============================================================================

#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE

#define INA219_ADDR      0x40   // A0=GND, A1=GND
#define INA219_REG_CFG   0x00
#define INA219_REG_SHUNT 0x01
#define INA219_REG_BUS   0x02
#define INA219_REG_PWR   0x03
#define INA219_REG_CURR  0x04
#define INA219_REG_CAL   0x05

static bool _ina219_writeReg(uint8_t reg, uint16_t val) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(val >> 8));
  Wire.write((uint8_t)(val & 0xFF));
  return Wire.endTransmission() == 0;
}

static bool _ina219_readReg(uint8_t reg, int16_t& out) {
  Wire.beginTransmission(INA219_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((uint8_t)INA219_ADDR, (uint8_t)2, (uint8_t)1);
  if (Wire.available() < 2) return false;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  out = (int16_t)((hi << 8) | lo);
  return true;
}

static bool ina219_begin() {
  // Reset + config 32V/2A/12-bit
  if (!_ina219_writeReg(INA219_REG_CFG, 0x399F)) return false;
  delay(1);
  // Registrar calibración para activar corriente y potencia
  if (!_ina219_writeReg(INA219_REG_CAL, 0x1000)) return false;
  delay(1);
  // Verificar que el config se guardó (descarta 0xFFFF de bus flotante)
  int16_t cfg = 0;
  if (!_ina219_readReg(INA219_REG_CFG, cfg)) return false;
  return (uint16_t)cfg != 0xFFFF;
}

// Voltaje del bus en voltios (precisión 4 mV)
static float ina219_readBusVoltage() {
  int16_t raw = 0;
  if (!_ina219_readReg(INA219_REG_BUS, raw)) return NAN;
  // bits[15:3] = voltaje, bit 1 = CNVR, bit 0 = OVF
  if (raw & 0x01) return NAN;  // overflow
  return (float)((raw >> 3) & 0x1FFF) * 0.004f;
}

// Corriente en mA (current_lsb = 0.1 mA/bit con Cal = 0x1000 y R_shunt = 0.1 Ω)
static float ina219_readCurrent_mA() {
  int16_t raw = 0;
  if (!_ina219_readReg(INA219_REG_CURR, raw)) return NAN;
  return (float)raw * 0.1f;  // 0.1 mA/bit
}

// Potencia en mW (power_lsb = 2 mW/bit)
static float ina219_readPower_mW() {
  int16_t raw = 0;
  if (!_ina219_readReg(INA219_REG_PWR, raw)) return NAN;
  return (float)(uint16_t)raw * 2.0f;  // unsigned, 2 mW/bit
}

#endif  // PROFILE_IRRIGATION || PROFILE_AQUA_SMART_REMOTE
