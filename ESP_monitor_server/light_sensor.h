#pragma once
// =============================================================================
// Sensor de luz ambiente — autodetección TSL2584 / APDS-9930 (I2C directo)
// Ambos chips usan dirección 0x39 y protocolo CMD=0x80|reg.
// TSL2584:  datos en 0x0C-0x0F, ID en 0x0A (bits[7:4]=0xA)
// APDS-9930: datos en 0x14-0x17, ID en 0x12 (=0x39)
// =============================================================================
#define LIGHT_ADDR     0x39
#define LIGHT_CMD      0x80   // bit CMD obligatorio en el byte de registro

// Registros comunes (misma dirección en ambos chips)
#define LIGHT_CTRL     0x00   // ENABLE / POWER
#define LIGHT_TIMING   0x01   // TIMING (TSL) / ATIME (APDS)

// Registros exclusivos TSL2584
#define TSL_ID_REG     0x0A   // Part number en bits[7:4]; TSL2584 = 0xA
#define TSL_D0L        0x0C   // CH0 low  (visible + IR)
#define TSL_D1L        0x0E   // CH1 low  (IR)

// Registros exclusivos APDS-9930
#define APDS_ID_REG    0x12   // Chip ID; APDS-9930 = 0x39
#define APDS_CDATAL    0x14   // Clear channel low  (visible + IR)
#define APDS_IRDATAL   0x16   // IR channel low

static bool light_is_apds = false;  // se establece en tsl_begin()

static bool light_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LIGHT_ADDR);
  Wire.write(LIGHT_CMD | reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Lectura stop-start
static uint8_t light_read8(uint8_t reg) {
  Wire.beginTransmission(LIGHT_ADDR);
  Wire.write(LIGHT_CMD | reg);
  if (Wire.endTransmission() != 0) return 0xFF;
  Wire.requestFrom((uint8_t)LIGHT_ADDR, (uint8_t)1, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static uint16_t light_read16(uint8_t regL) {
  uint16_t lo = light_read8(regL);
  uint16_t hi = light_read8(regL + 1);
  return (hi << 8) | lo;
}

// Enciende el sensor, detecta el tipo y configura la integración.
// Devuelve true si el sensor responde en el bus.
bool tsl_begin() {
  if (!light_write(LIGHT_CTRL, 0x03)) return false;  // Power ON + ALS enable
  delay(10);

  // Autodetección: leer ambos registros de ID y mostrarlos
  uint8_t apds_id = light_read8(APDS_ID_REG);  // 0x12 → 0x39 o 0x30 para APDS-9930
  uint8_t tsl_id  = light_read8(TSL_ID_REG);   // 0x0A → 0xAx para TSL2584
  DLOGF("[LUZ] ID@0x12=0x%02X  ID@0x0A=0x%02X\n", apds_id, tsl_id);

  // APDS-9930: ID puede ser 0x39 (rev1) o 0x30 (rev0); nibble alto = 0x3
  if ((apds_id & 0xF0) == 0x30) {
    light_is_apds = true;
    // ATIME=0xDB → 37 ciclos × 2.73 ms ≈ 101 ms por conversión
    light_write(LIGHT_TIMING, 0xDB);
    DLOGF("[LUZ] APDS-9930 detectado (ID=0x%02X)\n", apds_id);
  } else if ((tsl_id & 0xF0) == 0xA0) {
    light_is_apds = false;
    light_write(LIGHT_TIMING, 0x02);  // 402 ms, ganancia 1×
    DLOGF("[LUZ] TSL2584 detectado (ID=0x%02X)\n", tsl_id);
  } else {
    // ID desconocido — probar APDS-9930 como fallback (más probable en 0x39)
    light_is_apds = true;
    light_write(LIGHT_TIMING, 0xDB);
    DLOGF("[LUZ] ID desconocido — usando registros APDS-9930 (fallback)\n");
  }

  delay(450);  // Esperar primera integración completa (cubre ambos casos)
  return true;
}

// Devuelve lux estimado a partir del canal visible+IR y el canal IR.
// Usa la fórmula TSL258x (válida también como aproximación para APDS-9930).
float tsl_readLux() {
  uint16_t ch0, ch1;
  if (light_is_apds) {
    ch0 = light_read16(APDS_CDATAL);   // clear (visible + IR)
    ch1 = light_read16(APDS_IRDATAL);  // IR
  } else {
    ch0 = light_read16(TSL_D0L);
    ch1 = light_read16(TSL_D1L);
  }

  if (ch0 == 0) return 0.0f;

  float ratio = (float)ch1 / (float)ch0;
  float lux;

  if      (ratio <= 0.52f) lux = 0.0315f * ch0 - 0.0593f * ch0 * pow(ratio, 1.4f);
  else if (ratio <= 0.65f) lux = 0.0229f * ch0 - 0.0291f * ch1;
  else if (ratio <= 0.80f) lux = 0.0157f * ch0 - 0.0180f * ch1;
  else if (ratio <= 1.30f) lux = 0.00338f * ch0 - 0.00260f * ch1;
  else                     lux = 0.0f;

  return max(0.0f, lux);
}
