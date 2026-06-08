// soil_baud_changer.ino
// One-shot: changes a YIERYI RS485 soil sensor from 4800 → 9600 baud and address to 3.
// Register 0x0100: slave address | Register 0x0101 values: 0=1200, 1=2400, 2=4800, 3=9600, 4=19200
//
// Wiring (ESP32 example):
//   GPIO16 (RX2) ← sensor TX
//   GPIO17 (TX2) → sensor TX
//   GPIO4  (DE)  → DE/RE pin of RS485 module  (omit if using auto-dir module)

#define SENSOR_RX    16
#define SENSOR_TX    17
#define DE_PIN        4   // set to -1 if your module handles direction automatically
#define SLAVE_ADDR   0x01 // factory default

static uint16_t crc16(const byte* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static bool sendFC06(HardwareSerial& ser, uint8_t addr, uint16_t reg, uint16_t value) {
  byte cmd[8];
  cmd[0] = addr;
  cmd[1] = 0x06;
  cmd[2] = reg >> 8;
  cmd[3] = reg & 0xFF;
  cmd[4] = value >> 8;
  cmd[5] = value & 0xFF;
  uint16_t c = crc16(cmd, 6);
  cmd[6] = c & 0xFF;
  cmd[7] = c >> 8;

  if (DE_PIN >= 0) { digitalWrite(DE_PIN, HIGH); delay(5); }
  ser.write(cmd, 8);
  ser.flush();
  delayMicroseconds(500);
  if (DE_PIN >= 0) digitalWrite(DE_PIN, LOW);

  delay(2);
  while (ser.available()) ser.read(); // flush echo

  uint32_t t0 = millis();
  while (!ser.available()) {
    if (millis() - t0 > 500) { Serial.println("TIMEOUT — no response"); return false; }
  }
  delay(20); // collect full frame
  byte resp[8] = {};
  size_t n = 0;
  while (ser.available() && n < sizeof(resp)) resp[n++] = ser.read();

  Serial.print("Response bytes: ");
  for (size_t i = 0; i < n; i++) { Serial.printf("%02X ", resp[i]); }
  Serial.println();

  if (n < 8) { Serial.println("Short response"); return false; }
  uint16_t calc = crc16(resp, 6);
  if ((calc & 0xFF) != resp[6] || (calc >> 8) != resp[7]) {
    Serial.println("CRC mismatch"); return false;
  }
  // Success echo = same frame
  return resp[0] == addr && resp[1] == 0x06 && resp[4] == (value >> 8) && resp[5] == (value & 0xFF);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (DE_PIN >= 0) { pinMode(DE_PIN, OUTPUT); digitalWrite(DE_PIN, LOW); }

  Serial2.begin(4800, SERIAL_8N1, SENSOR_RX, SENSOR_TX);
  delay(500);
  Serial.println("Conectado al sensor a 4800 baud, dir 0x01.");

  Serial.println("Cambiando direccion: 0x01 → 0x03 (reg 0x0100 = 3) ...");
  bool okAddr = sendFC06(Serial2, SLAVE_ADDR, 0x0100, 3);
  if (okAddr) {
    Serial.println("OK — direccion cambiada a 3.");
  } else {
    Serial.println("FALLO al cambiar direccion — abortando.");
    Serial2.end();
    return;
  }
  delay(200);

  Serial.println("Cambiando baud rate: 4800 → 9600 (reg 0x0101 = 3) ...");
  // Tras el cambio de dir, el sensor responde en addr 3
  bool okBaud = sendFC06(Serial2, 0x03, 0x0101, 3);
  if (okBaud) {
    Serial.println("OK — baud rate cambiado a 9600.");
    Serial.println("Verifica reconectando con soil_scanner a 9600 baud, dir 3.");
  } else {
    Serial.println("FALLO — no se recibió confirmación.");
    Serial.println("Posibles causas:");
    Serial.println("  · Sensor no implementa registro 0x0101 (algunos modelos lo ignoran)");
    Serial.println("  · El sensor reinicia sin enviar eco — prueba soil_scanner a 9600 / dir 3 igual");
    Serial.println("  · Conexiones o DE/RE pin incorrectos");
  }

  Serial2.end();
}

void loop() {}
