#pragma once
// =============================================================================
// HDC1080 — temperatura y humedad I2C (solo PROFILE_AQUALEAK)
// Dirección 0x40. Se accede directamente via Wire, sin librería externa.
// NOTA: comparte dirección con HTU2x pero protocolo diferente.
// =============================================================================
// Parámetros derivados agroambientales (Magnus, Heat Index, AH) — PROFILE_AQUALEAK
// =============================================================================
// Qwiic Power Switch (PCA9536) — gestiona alimentación del bus de sensores.
// Se enciende una vez en setup() y permanece ON durante toda la ejecución.
// =============================================================================

#if DEVICE_PROFILE == PROFILE_AQUALEAK

#define HDC1080_ADDR 0x40

static bool hdc1080_init() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x02);  // registro de configuración
  Wire.write(0x00);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(15);
  return true;
}

static float hdc1080_readTemp() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x00);  // registro temperatura
  if (Wire.endTransmission() != 0) return NAN;
  delay(35);  // datasheet: 6.5ms; 35ms para cubrir bus ocupado
  Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2, (uint8_t)1);
  if (Wire.available() < 2) return NAN;
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  float t = (raw / 65536.0f) * 165.0f - 40.0f;
  return (t > -40.0f && t < 85.0f) ? t : NAN;
}

static float hdc1080_readHum() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x01);  // registro humedad
  if (Wire.endTransmission() != 0) return NAN;
  delay(35);  // datasheet: 6.5ms; 35ms para cubrir bus ocupado
  Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2, (uint8_t)1);
  if (Wire.available() < 2) return NAN;
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  float h = (raw / 65536.0f) * 100.0f;
  return (h >= 0.0f && h <= 100.0f) ? h : NAN;
}

// Parámetros derivados agrometeorologícos
// Temperatura: HDC1080 primaria, BMP280 secundaria; Tavg = media si ambas disponibles
static float agro_calcDewPoint(float tempC, float hum) {
  const float a = 17.271f, b = 237.7f;
  float g = (a * tempC / (b + tempC)) + logf(hum / 100.0f);
  return (b * g) / (a - g);
}

static float agro_calcHeatIndex(float tempC, float hum) {
  float t = tempC * 9.0f / 5.0f + 32.0f;
  float hi = -42.379f
    + 2.04901523f  * t
    + 10.14333127f * hum
    - 0.22475541f  * t * hum
    - 0.00683783f  * t * t
    - 0.05481717f  * hum * hum
    + 0.00122874f  * t * t * hum
    + 0.00085282f  * t * hum * hum
    - 0.00000199f  * t * t * hum * hum;
  return (hi - 32.0f) * 5.0f / 9.0f;
}

static float agro_calcAbsHumidity(float tempC, float hum) {
  float es = 6.112f * expf((17.67f * tempC) / (tempC + 243.5f));
  return (es * hum * 2.1674f) / (273.15f + tempC);
}

#endif  // PROFILE_AQUALEAK
