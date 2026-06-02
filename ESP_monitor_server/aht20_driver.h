#pragma once
// =============================================================================
// AHT20 — temperatura y humedad I2C (solo PROFILE_IRRIGATION)
// Dirección 0x38. Driver directo via Wire, sin librería externa.
// =============================================================================

#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE

#define AHT20_ADDR      0x38
#define AHT20_CMD_INIT  0xBE
#define AHT20_CMD_TRIG  0xAC
#define AHT20_CMD_RESET 0xBA

static bool aht20_begin() {
  // Reset suave
  Wire.beginTransmission(AHT20_ADDR);
  Wire.write(AHT20_CMD_RESET);
  Wire.endTransmission();
  delay(20);

  // Inicializar calibración
  Wire.beginTransmission(AHT20_ADDR);
  Wire.write(AHT20_CMD_INIT);
  Wire.write(0x08);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(10);

  // Verificar flag calibrado (bit 3 del status)
  Wire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)1, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t status = Wire.read();
  return (status & 0x08) != 0;  // bit CAL = 1 → calibrado
}

// Lee temperatura y humedad en una sola transacción (80 ms bloqueo).
// Retorna false si el sensor no responde o los datos están fuera de rango.
static bool aht20_read(float& outTemp, float& outHum) {
  // Disparar medición
  Wire.beginTransmission(AHT20_ADDR);
  Wire.write(AHT20_CMD_TRIG);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(80);  // conversión típica ~75 ms según datasheet

  Wire.requestFrom((uint8_t)AHT20_ADDR, (uint8_t)6, (uint8_t)1);
  if (Wire.available() < 6) return false;

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

  // bit 7 del byte de status = busy; si sigue ocupado la medición no está lista
  if (buf[0] & 0x80) return false;

  uint32_t rawHum  = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);
  uint32_t rawTemp = (((uint32_t)buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

  outHum  = (rawHum  / 1048576.0f) * 100.0f;
  outTemp = (rawTemp / 1048576.0f) * 200.0f - 50.0f;

  bool ok = (outTemp > -40.0f && outTemp < 85.0f &&
             outHum  >=   0.0f && outHum  <= 100.0f);
  if (!ok) { outTemp = NAN; outHum = NAN; }
  return ok;
}

static float aht20_readTemp() {
  float t = NAN, h = NAN;
  aht20_read(t, h);
  return t;
}

static float aht20_readHum() {
  float t = NAN, h = NAN;
  aht20_read(t, h);
  return h;
}

#endif  // PROFILE_IRRIGATION || PROFILE_AQUA_SMART_REMOTE
