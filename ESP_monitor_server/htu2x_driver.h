#pragma once
// =============================================================================
// HTU2x (HTU21D / HTU20D / SHT21) — temperatura y humedad por I2C
// Dirección fija: 0x40. Sin librería externa.
// =============================================================================
#define HTU2X_ADDR        0x40
#define HTU2X_CMD_TEMP    0xF3   // medir temperatura, no-hold master
#define HTU2X_CMD_HUM     0xF5   // medir humedad,     no-hold master
#define HTU2X_CMD_RESET   0xFE
#define HTU2X_CMD_WR_REG  0xE6   // escribir registro de usuario
#define HTU2X_CMD_RD_REG  0xE7   // leer registro de usuario
// Registro de usuario: bit7+bit0=resolución, bit2=calefactor, bit1=OTP_disable
// 0x02 → 12/14 bit, calefactor OFF, OTP desactivado (default tras reset)
// 0x06 → 12/14 bit, calefactor ON,  OTP desactivado
#define HTU2X_REG_DEFAULT 0x02
#define HTU2X_REG_HEATER  0x06

bool htu_begin() {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(HTU2X_CMD_RESET);
  if (Wire.endTransmission() != 0) return false;
  delay(15);  // tiempo de reset (máx 15 ms según datasheet)
  return true;
}

// Lanza una medición y espera. Devuelve NAN si falla o si hay timeout.
static float htu_measure(uint8_t cmd, uint16_t wait_ms) {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return NAN;
  delay(wait_ms);
  uint8_t got = Wire.requestFrom((uint8_t)HTU2X_ADDR, (uint8_t)3, (uint8_t)1);
  if (got < 2) return NAN;  // timeout o NACK — el sensor no respondió
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  if (Wire.available()) Wire.read();  // descarta CRC
  raw &= 0xFFFC;  // limpia los 2 bits de tipo de medición
  return (float)raw;
}

float htu_readTemp() {
  float raw = htu_measure(HTU2X_CMD_TEMP, 50);
  if (isnan(raw)) return NAN;
  return -46.85f + 175.72f * (raw / 65536.0f);
}

float htu_readHumidity() {
  float raw = htu_measure(HTU2X_CMD_HUM, 20);
  if (isnan(raw)) return NAN;
  return constrain(-6.0f + 125.0f * (raw / 65536.0f), 0.0f, 100.0f);
}

// Activa o desactiva el calefactor interno del HTU2x.
static void htu_set_heater(bool on) {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(HTU2X_CMD_WR_REG);
  Wire.write(on ? HTU2X_REG_HEATER : HTU2X_REG_DEFAULT);
  Wire.endTransmission();
}

// Calentamiento de arranque: activa el calefactor ~3 s para evaporar
// condensación y obtener lecturas de humedad más fiables desde el inicio.
void htu_heater_warmup() {
  DLOGLN("HTU2x: calentamiento 3s (evaporar condensacion)...");
  htu_set_heater(true);
  for (int i = 3; i > 0; i--) {
    float t = htu_readTemp();
    float h = htu_readHumidity();
    DLOGF("  HTU2x heater ON — T:%.1f C  H:%.1f %%  (%ds)\n", t, h, i);
    delay(1000);
  }
  htu_set_heater(false);
  delay(1000);  // estabilización tras apagar calefactor
  DLOGLN("HTU2x: calentamiento completado.");
}
