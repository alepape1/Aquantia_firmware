#pragma once
#include <Arduino.h>

// RS485 half-duplex: pin compartido DE+RE, LOW = recepción, HIGH = transmisión
static uint8_t _hali_de_re = 255;

struct HalisenseData {
    float temperature;   // °C
    float moisture;      // %
    float ec;            // dS/m
    float ph;
    float tds;           // ppm (derivado: ec_raw_uScm * 0.5)
    int   n;             // mg/kg Nitrógeno
    int   p;             // mg/kg Fósforo
    int   k;             // mg/kg Potasio
    bool  ok;
};

// Modbus RTU CRC-16
static uint16_t _hali_crc16(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else              crc >>= 1;
        }
    }
    return crc;
}

// Comando READ_ALL: leer 7 registros desde 0x0000 (dirección 0x01)
// Respuesta: 3 bytes cabecera + 14 bytes datos (7 registros × 2) + 2 bytes CRC = 19 bytes
static const uint8_t _HALI_CMD[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08 };

void halisense_init(uint8_t de_re_pin) {
    _hali_de_re = de_re_pin;
    pinMode(_hali_de_re, OUTPUT);
    digitalWrite(_hali_de_re, LOW);  // modo recepción por defecto
    Serial2.begin(4800, SERIAL_8N1, 16, 17);  // RX=GPIO16, TX=GPIO17
    delay(100);
}

bool halisense_read(HalisenseData &out) {
    out = {};

    // Vaciar buffer de entrada
    while (Serial2.available()) Serial2.read();

    // Transmitir
    digitalWrite(_hali_de_re, HIGH);
    delayMicroseconds(200);
    Serial2.write(_HALI_CMD, sizeof(_HALI_CMD));
    Serial2.flush();
    delayMicroseconds(200);
    digitalWrite(_hali_de_re, LOW);  // modo recepción

    // Esperar respuesta (19 bytes a 4800 baud ≈ 40 ms; timeout 200 ms)
    const uint8_t EXPECTED = 19;
    unsigned long t0 = millis();
    while (Serial2.available() < EXPECTED) {
        if (millis() - t0 > 200) {
            out.ok = false;
            return false;
        }
    }

    uint8_t buf[EXPECTED];
    Serial2.readBytes(buf, EXPECTED);

    // Validar cabecera: dirección 0x01, función 0x03, byte_count 0x0E (14 bytes)
    if (buf[0] != 0x01 || buf[1] != 0x03 || buf[2] != 0x0E) {
        out.ok = false;
        return false;
    }

    // Validar CRC
    uint16_t crc_calc = _hali_crc16(buf, EXPECTED - 2);
    uint16_t crc_recv = (uint16_t)buf[EXPECTED - 1] << 8 | buf[EXPECTED - 2];
    if (crc_calc != crc_recv) {
        out.ok = false;
        return false;
    }

    // Extraer registros (big-endian, offset 3)
    uint16_t reg[7];
    for (uint8_t i = 0; i < 7; i++) {
        reg[i] = (uint16_t)buf[3 + i * 2] << 8 | buf[4 + i * 2];
    }

    // Registro 0: temperatura (÷10 → °C)
    out.temperature = reg[0] / 10.0f;
    // Registro 1: humedad (÷10 → %)
    out.moisture    = reg[1] / 10.0f;
    // Registro 2: CE en µS/cm → dS/m (÷1000) + TDS ppm (× 0.5 sobre µS/cm)
    float ec_raw    = (float)reg[2];
    out.ec          = ec_raw / 1000.0f;
    out.tds         = ec_raw * 0.5f;
    // Registro 3: pH (÷10)
    out.ph          = reg[3] / 10.0f;
    // Registros 4-6: NPK directos (mg/kg)
    out.n           = (int)reg[4];
    out.p           = (int)reg[5];
    out.k           = (int)reg[6];
    out.ok          = true;
    return true;
}
