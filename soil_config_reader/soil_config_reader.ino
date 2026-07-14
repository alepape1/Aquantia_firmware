// soil_config_reader.ino
// Lee los bloques de registros de configuración de un sensor RS485 YIERYI/similar.
// Conecta al sensor en la velocidad conocida (4800/8N1/addr=0x01) y vuelca:
//   - Registros 0x0100..0x010F  (rango típico de config en YIERYI)
//   - Registros 0x07D0..0x07DF  (rango alternativo documentado en otros clones)
//   - Registros 0x1000..0x100F  (rango usado por algunos modelos RENKE)
// Con los valores leídos podrás identificar cuál registro corresponde al baud rate.
//
// Wiring (mismos pines que soil_baud_changer):
//   GPIO16 (RX2) ← sensor TX
//   GPIO17 (TX2) → sensor TX
//   GPIO4  (DE)  → DE/RE pin del módulo RS485

#define SENSOR_RX   14
#define SENSOR_TX   13
#define DE_PIN       4
#define SLAVE_ADDR  0x01
#define KNOWN_BAUD  4800

static uint16_t crc16(const byte* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// Envía FC03 (read holding registers) y devuelve número de registros leídos (0 = fallo)
static uint8_t readRegs(uint16_t startReg, uint8_t count, uint16_t* out) {
  byte cmd[8];
  cmd[0] = SLAVE_ADDR;
  cmd[1] = 0x03;
  cmd[2] = startReg >> 8;
  cmd[3] = startReg & 0xFF;
  cmd[4] = 0x00;
  cmd[5] = count;
  uint16_t c = crc16(cmd, 6);
  cmd[6] = c & 0xFF;
  cmd[7] = c >> 8;

  if (DE_PIN >= 0) { digitalWrite(DE_PIN, HIGH); delayMicroseconds(200); }
  Serial2.write(cmd, 8);
  Serial2.flush();
  delayMicroseconds(500);
  if (DE_PIN >= 0) digitalWrite(DE_PIN, LOW);

  delay(3);
  while (Serial2.available()) Serial2.read(); // flush echo

  uint32_t t0 = millis();
  while (!Serial2.available()) {
    if (millis() - t0 > 500) return 0;
  }
  delay(20);

  size_t expected = 3 + count * 2 + 2;
  byte resp[64] = {};
  size_t n = 0;
  while (Serial2.available() && n < expected) resp[n++] = Serial2.read();

  if (n < expected) return 0;
  uint16_t calc = crc16(resp, n - 2);
  if ((calc & 0xFF) != resp[n - 2] || (calc >> 8) != resp[n - 1]) return 0;
  if (resp[0] != SLAVE_ADDR || resp[1] != 0x03) return 0;

  for (uint8_t i = 0; i < count; i++)
    out[i] = ((uint16_t)resp[3 + i * 2] << 8) | resp[4 + i * 2];

  return count;
}

static void dumpBlock(uint16_t startReg, uint8_t count) {
  Serial.printf("\n--- Registros 0x%04X..0x%04X ---\n", startReg, startReg + count - 1);
  uint16_t vals[16] = {};
  uint8_t got = readRegs(startReg, count, vals);
  if (got == 0) {
    Serial.println("  Sin respuesta (bloque no soportado)");
    return;
  }
  for (uint8_t i = 0; i < count; i++) {
    Serial.printf("  0x%04X = 0x%04X  (%5u)\n", startReg + i, vals[i], vals[i]);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (DE_PIN >= 0) { pinMode(DE_PIN, OUTPUT); digitalWrite(DE_PIN, LOW); }

  Serial2.begin(KNOWN_BAUD, SERIAL_8N1, SENSOR_RX, SENSOR_TX);
  delay(500);
  Serial.printf("Conectado al sensor a %u baud, addr=0x%02X\n", KNOWN_BAUD, SLAVE_ADDR);

  // Lectura de datos para confirmar que el sensor responde
  dumpBlock(0x0000, 7);

  // Bloques de configuración a explorar
  dumpBlock(0x0100, 8);   // rango típico YIERYI
  dumpBlock(0x07D0, 8);   // rango alternativo clones
  dumpBlock(0x1000, 8);   // rango RENKE/otros

  Serial.println("\n====== FIN ======");
  Serial.println("Busca el registro cuyo valor = 2 (4800 baud) o 1 (2400) — ese es el de baud rate.");
  Serial.println("Reg de addr suele tener valor = 1 (tu slave addr actual).");

  Serial2.end();
}

void loop() {}
