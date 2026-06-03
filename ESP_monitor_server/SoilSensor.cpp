/*
  SoilSensor.cpp

  Implementation for 7-in-1 RS485 Modbus soil sensor (NPK + Temp + Humidity + EC + pH).

  Modbus RTU frame format (request):
    [ADDR][FC][REG_HI][REG_LO][COUNT_HI][COUNT_LO][CRC_LO][CRC_HI]

  Response frame (7 bytes for single register):
    [ADDR][FC][BYTE_COUNT][DATA_HI][DATA_LO][CRC_LO][CRC_HI]

  Response frame (19 bytes for readAll - 7 registers):
    [ADDR][FC][BYTE_COUNT][D0_HI][D0_LO]...[D6_HI][D6_LO][CRC_LO][CRC_HI]

  Register map (standard for this sensor family):
    0x0000 - Humidity       (x0.1 %)
    0x0001 - Temperature    (x0.1 °C)
    0x0002 - EC             (µS/cm)
    0x0003 - pH             (x0.1)
    0x0004 - Nitrogen       (mg/kg)
    0x0005 - Phosphorus     (mg/kg)
    0x0006 - Potassium      (mg/kg)

  Address change: FC 0x06, register 0x0100 = new slave address (YIERYI family standard).
  Baud rate change: FC 0x06, register 0x0101 = 0-4 (1200/2400/4800/9600/19200) — if supported.
  Note: some variants (confirmed YIERYI NPK) have baud rate fixed in firmware and return
  ILLEGAL DATA ADDRESS (exception 0x02) when writing 0x0101 — bus must stay at 9600.
*/

#include "SoilSensor.h"

// ── Timing constants ─────────────────────────────────────────────────────────
static const uint16_t TX_ENABLE_DELAY_MS    = 10;   // DE/RE settle time before sending
static const uint16_t RESPONSE_TIMEOUT_MS   = 250;  // max wait for first response byte
static const uint16_t INTER_BYTE_TIMEOUT_MS = 20;   // max gap between bytes mid-response

// ── CRC-16 Modbus ────────────────────────────────────────────────────────────
static uint16_t crc16(const byte *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

// Build a standard FC 0x03 read command with CRC
static void buildReadCmd(uint8_t addr, uint16_t startReg, uint16_t count, byte *out)
{
    out[0] = addr;
    out[1] = 0x03;
    out[2] = startReg >> 8;
    out[3] = startReg & 0xFF;
    out[4] = count >> 8;
    out[5] = count & 0xFF;
    uint16_t c = crc16(out, 6);
    out[6] = c & 0xFF;
    out[7] = c >> 8;
}

// ── CRC validation ────────────────────────────────────────────────────────────
static bool validateCRC(const byte *buf, size_t len)
{
    if (len < 4) return false;
    uint16_t calc = crc16(buf, len - 2);
    uint16_t recv = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    return (calc == recv);
}

// ── Constructors ─────────────────────────────────────────────────────────────
SoilSensor::SoilSensor(HardwareSerial &serial)
    : sensorSerial(serial),
      temperature(0), humidity(0), ec(0), ph(0),
      nitrogen(0), phosphorus(0), potassium(0),
      tempFresh(false), humFresh(false), ecFresh(false), phFresh(false),
      nFresh(false), pFresh(false), kFresh(false)
{}

SoilSensor::SoilSensor(HardwareSerial &serial, int dePin)
    : sensorSerial(serial), _dePin(dePin),
      temperature(0), humidity(0), ec(0), ph(0),
      nitrogen(0), phosphorus(0), potassium(0),
      tempFresh(false), humFresh(false), ecFresh(false), phFresh(false),
      nFresh(false), pFresh(false), kFresh(false)
{}

SoilSensor::SoilSensor(HardwareSerial &serial, int rxPin, int txPin, int dePin)
    : sensorSerial(serial), _rxPin(rxPin), _txPin(txPin), _dePin(dePin), _customPins(true),
      temperature(0), humidity(0), ec(0), ph(0),
      nitrogen(0), phosphorus(0), potassium(0),
      tempFresh(false), humFresh(false), ecFresh(false), phFresh(false),
      nFresh(false), pFresh(false), kFresh(false)
{}

// ── begin() ──────────────────────────────────────────────────────────────────
bool SoilSensor::begin(uint32_t baudRate)
{
    if (_dePin >= 0) {
        pinMode(_dePin, OUTPUT);
        digitalWrite(_dePin, LOW);  // start in receive mode
    }

    if (_customPins)
        sensorSerial.begin(baudRate, SERIAL_8N1, _rxPin, _txPin);
    else
        sensorSerial.begin(baudRate);

    delay(500);  // RS485 adapter needs more than 100 ms to stabilise after power-on
    return readAllVariables();
}

// ── sendCommand() ─────────────────────────────────────────────────────────────
bool SoilSensor::sendCommand(const byte *command, size_t length)
{
    while (sensorSerial.available()) sensorSerial.read();
    memset(readBuffer, 0x00, sizeof(readBuffer));

    if (_dePin >= 0) digitalWrite(_dePin, HIGH);  // enable TX
    delay(TX_ENABLE_DELAY_MS);
    sensorSerial.write(command, length);
    sensorSerial.flush();
    delayMicroseconds(200);                        // allow last stop bit to complete before releasing DE
    if (_dePin >= 0) digitalWrite(_dePin, LOW);   // back to RX

    // Flush TX echo — some RS485 adapters reflect TX bytes back into RX during transmission.
    delay(2);
    while (sensorSerial.available()) sensorSerial.read();

    uint32_t startTime = millis();
    while (!sensorSerial.available()) {
        if (millis() - startTime > RESPONSE_TIMEOUT_MS) return false;
    }

    size_t idx = 0;
    startTime = millis();
    while (idx < sizeof(readBuffer)) {
        if (sensorSerial.available()) {
            readBuffer[idx++] = sensorSerial.read();
            startTime = millis();
        } else if (millis() - startTime > INTER_BYTE_TIMEOUT_MS) {
            break;
        }
    }

    return (idx > 0);
}

// ── readAllVariables() ────────────────────────────────────────────────────────
bool SoilSensor::readAllVariables()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0000, 7, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    return processReadingAll();
}

bool SoilSensor::processReadingAll()
{
    if (readBuffer[0] != _slaveAddr || readBuffer[1] != 0x03 || readBuffer[2] != 0x0E) return false;
    if (!validateCRC(readBuffer, 19)) return false;

    auto readWord = [&](size_t offset) -> uint16_t {
        return ((uint16_t)readBuffer[offset] << 8) | readBuffer[offset + 1];
    };

    humidity    = readWord(3)  * 0.1f;   // reg 0x0000
    temperature = readWord(5)  * 0.1f;   // reg 0x0001
    ec          = (float)readWord(7);    // reg 0x0002  (µS/cm, no scaling)
    ph          = readWord(9)  * 0.1f;   // reg 0x0003
    nitrogen    = (float)readWord(11);   // reg 0x0004
    phosphorus  = (float)readWord(13);   // reg 0x0005
    potassium   = (float)readWord(15);   // reg 0x0006

    tempFresh = humFresh = ecFresh = phFresh = nFresh = pFresh = kFresh = true;
    return true;
}

// ── Individual reads ──────────────────────────────────────────────────────────
static bool parseSingleRegister(const byte *buf, float &out, float scale, uint8_t addr)
{
    if (buf[0] != addr || buf[1] != 0x03 || buf[2] != 0x02) return false;
    if (!validateCRC(buf, 7)) return false;
    uint16_t raw = ((uint16_t)buf[3] << 8) | buf[4];
    out = raw * scale;
    return true;
}

bool SoilSensor::readTemperature()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0001, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, temperature, 0.1f, _slaveAddr);
    if (ok) tempFresh = true;
    return ok;
}

bool SoilSensor::readHumidity()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0000, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, humidity, 0.1f, _slaveAddr);
    if (ok) humFresh = true;
    return ok;
}

bool SoilSensor::readEC()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0002, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, ec, 1.0f, _slaveAddr);
    if (ok) ecFresh = true;
    return ok;
}

bool SoilSensor::readPH()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0003, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, ph, 0.1f, _slaveAddr);
    if (ok) phFresh = true;
    return ok;
}

bool SoilSensor::readNitrogen()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0004, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, nitrogen, 1.0f, _slaveAddr);
    if (ok) nFresh = true;
    return ok;
}

bool SoilSensor::readPhosphorus()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0005, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, phosphorus, 1.0f, _slaveAddr);
    if (ok) pFresh = true;
    return ok;
}

bool SoilSensor::readPotassium()
{
    byte cmd[8];
    buildReadCmd(_slaveAddr, 0x0006, 1, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    bool ok = parseSingleRegister(readBuffer, potassium, 1.0f, _slaveAddr);
    if (ok) kFresh = true;
    return ok;
}

// ── probe() ───────────────────────────────────────────────────────────────────
bool SoilSensor::probe(uint8_t addr)
{
    byte cmd[8];
    buildReadCmd(addr, 0x0000, 7, cmd);
    if (!sendCommand(cmd, sizeof(cmd))) return false;
    if (readBuffer[0] != addr || readBuffer[1] != 0x03 || readBuffer[2] != 0x0E) return false;
    return validateCRC(readBuffer, 19);
}

// ── changeAddress() ───────────────────────────────────────────────────────────
bool SoilSensor::changeAddress(uint8_t currentAddr, uint8_t newAddr)
{
    if (newAddr < 1 || newAddr > 247) return false;
    // FC 0x06: Write Single Register → register 0x0100 (slave address, YIERYI family)
    byte cmd[8];
    cmd[0] = currentAddr;
    cmd[1] = 0x06;
    cmd[2] = 0x01;
    cmd[3] = 0x00;
    cmd[4] = 0x00;
    cmd[5] = newAddr;
    uint16_t c = crc16(cmd, 6);
    cmd[6] = c & 0xFF;
    cmd[7] = c >> 8;

    if (!sendCommand(cmd, sizeof(cmd))) return false;
    // Successful write echoes the full 8-byte command
    return (readBuffer[0] == currentAddr && readBuffer[1] == 0x06
            && readBuffer[5] == newAddr
            && validateCRC(readBuffer, 8));
}
