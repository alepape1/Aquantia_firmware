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
*/

#include "SoilSensor.h"

// ── Modbus command bytes ─────────────────────────────────────────────────────
// Format: {ADDR, FC, REG_HI, REG_LO, COUNT_HI, COUNT_LO, CRC_LO, CRC_HI}

const byte SoilSensor::READ_ALL_REG[8]  = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};
const byte SoilSensor::READ_HUM_REG[8]  = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A};
const byte SoilSensor::READ_TEMP_REG[8] = {0x01, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD5, 0xCA};
const byte SoilSensor::READ_EC_REG[8]   = {0x01, 0x03, 0x00, 0x02, 0x00, 0x01, 0x25, 0xCA};
const byte SoilSensor::READ_PH_REG[8]   = {0x01, 0x03, 0x00, 0x03, 0x00, 0x01, 0x74, 0x0A};
const byte SoilSensor::READ_N_REG[8]    = {0x01, 0x03, 0x00, 0x04, 0x00, 0x01, 0xC5, 0xCB};
const byte SoilSensor::READ_P_REG[8]    = {0x01, 0x03, 0x00, 0x05, 0x00, 0x01, 0x94, 0x0B};
const byte SoilSensor::READ_K_REG[8]    = {0x01, 0x03, 0x00, 0x06, 0x00, 0x01, 0x64, 0x0B};

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

    uint32_t startTime = millis();
    while (!sensorSerial.available()) {
        if (millis() - startTime > RESPONSE_TIMEOUT_MS) {
#ifdef DEBUG_MODE
            Serial.printf("[SOIL] timeout esperando respuesta (%lu ms)\n",
                          (unsigned long)RESPONSE_TIMEOUT_MS);
#endif
            return false;
        }
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

#ifdef DEBUG_MODE
    Serial.printf("[SOIL] recibidos %u bytes:", (unsigned)idx);
    for (size_t i = 0; i < idx; i++) Serial.printf(" %02X", readBuffer[i]);
    Serial.println();
#endif

    return (idx > 0);
}

// ── CRC validation ────────────────────────────────────────────────────────────
static bool validateCRC(const byte *buf, size_t len)
{
    if (len < 4) return false;
    uint16_t calc = crc16(buf, len - 2);
    uint16_t recv = (uint16_t)buf[len - 2] | ((uint16_t)buf[len - 1] << 8);
    return (calc == recv);
}

// ── readAllVariables() ────────────────────────────────────────────────────────
bool SoilSensor::readAllVariables()
{
    if (!sendCommand(READ_ALL_REG, sizeof(READ_ALL_REG))) return false;
    return processReadingAll();
}

bool SoilSensor::processReadingAll()
{
    if (readBuffer[0] != 0x01 || readBuffer[1] != 0x03 || readBuffer[2] != 0x0E) {
#ifdef DEBUG_MODE
        Serial.printf("[SOIL] cabecera inesperada: %02X %02X %02X (esperado 01 03 0E)\n",
                      readBuffer[0], readBuffer[1], readBuffer[2]);
#endif
        return false;
    }
    if (!validateCRC(readBuffer, 19)) {
#ifdef DEBUG_MODE
        Serial.println("[SOIL] CRC invalido");
#endif
        return false;
    }

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
static bool parseSingleRegister(const byte *buf, float &out, float scale = 1.0f)
{
    if (buf[0] != 0x01 || buf[1] != 0x03 || buf[2] != 0x02) return false;
    if (!validateCRC(buf, 7)) return false;
    uint16_t raw = ((uint16_t)buf[3] << 8) | buf[4];
    out = raw * scale;
    return true;
}

bool SoilSensor::readTemperature()
{
    if (!sendCommand(READ_TEMP_REG, sizeof(READ_TEMP_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, temperature, 0.1f);
    if (ok) tempFresh = true;
    return ok;
}

bool SoilSensor::readHumidity()
{
    if (!sendCommand(READ_HUM_REG, sizeof(READ_HUM_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, humidity, 0.1f);
    if (ok) humFresh = true;
    return ok;
}

bool SoilSensor::readEC()
{
    if (!sendCommand(READ_EC_REG, sizeof(READ_EC_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, ec, 1.0f);
    if (ok) ecFresh = true;
    return ok;
}

bool SoilSensor::readPH()
{
    if (!sendCommand(READ_PH_REG, sizeof(READ_PH_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, ph, 0.1f);
    if (ok) phFresh = true;
    return ok;
}

bool SoilSensor::readNitrogen()
{
    if (!sendCommand(READ_N_REG, sizeof(READ_N_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, nitrogen, 1.0f);
    if (ok) nFresh = true;
    return ok;
}

bool SoilSensor::readPhosphorus()
{
    if (!sendCommand(READ_P_REG, sizeof(READ_P_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, phosphorus, 1.0f);
    if (ok) pFresh = true;
    return ok;
}

bool SoilSensor::readPotassium()
{
    if (!sendCommand(READ_K_REG, sizeof(READ_K_REG))) return false;
    bool ok = parseSingleRegister(readBuffer, potassium, 1.0f);
    if (ok) kFresh = true;
    return ok;
}
