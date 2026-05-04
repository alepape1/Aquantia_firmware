/**
 * @file pressure_sensor_i2c.cpp
 * @brief Implementación del driver para sensor de presión I2C
 *
 * Implementado para ESP32 con Arduino framework.
 * Basado en datasheet físico del fabricante chino (familia XGZP6847D).
 */

#include "pressure_sensor_i2c.h"

// ─── Helpers I2C internos ─────────────────────────────────────────────────────

/**
 * Escribe un byte en un registro del sensor.
 * Retorna true si ACK recibido.
 */
static bool _write_register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(PRESSURE_SENSOR_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    return (Wire.endTransmission() == 0);
}

/**
 * Lee N bytes a partir de un registro usando stop-start (NO repeated-start).
 * Muchos sensores chinos de esta familia no implementan repeated-start
 * correctamente — un stop completo entre escritura y lectura es más compatible.
 * Retorna true si se recibieron exactamente n_bytes.
 */
static bool _read_registers(uint8_t reg, uint8_t *buf, uint8_t n_bytes) {
    // Fase 1: escribir la dirección del registro con STOP completo
    Wire.beginTransmission(PRESSURE_SENSOR_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) {   // STOP — compatible con sensores sin repeated-start
        return false;
    }

    // Fase 2: leer N bytes en nueva transacción (START fresco)
    uint8_t received = Wire.requestFrom((uint8_t)PRESSURE_SENSOR_I2C_ADDR, n_bytes);
    if (received != n_bytes) {
        return false;
    }

    for (uint8_t i = 0; i < n_bytes; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

// ─── Conversiones matemáticas ─────────────────────────────────────────────────

/**
 * Convierte los 3 bytes crudos de presión a kPa (unidades de PRESSURE_SENSOR_FULLSCALE).
 *
 * Datasheet:
 *   m = REG06<<16 | REG07<<8 | REG08
 *   Si m >= 2^23: pressure = (m - 2^24) / 2^23 * Fullscale
 *   Si m <  2^23: pressure = m / 2^23 * Fullscale
 */
static float _convert_pressure(uint8_t high, uint8_t mid, uint8_t low) {
    uint32_t m = ((uint32_t)high << 16) | ((uint32_t)mid << 8) | (uint32_t)low;

    float pressure;
    if (m >= 8388608UL) {   // 2^23 = 8388608
        // Valor negativo (bit de signo = 1)
        pressure = ((float)m - 16777216.0f) / 8388608.0f * PRESSURE_SENSOR_FULLSCALE;
    } else {
        // Valor positivo (bit de signo = 0)
        pressure = (float)m / 8388608.0f * PRESSURE_SENSOR_FULLSCALE;
    }
    return pressure;
}

/**
 * Convierte los 2 bytes crudos de temperatura a °C.
 *
 * Datasheet:
 *   n = REG09<<8 | REG0A
 *   Si n >= 2^15: temp = (n - 2^16) / 256
 *   Si n <  2^15: temp = n / 256
 */
static float _convert_temperature(uint8_t high, uint8_t low) {
    uint16_t n = ((uint16_t)high << 8) | (uint16_t)low;

    float temp;
    if (n >= 32768U) {   // 2^15 = 32768
        // Temperatura negativa
        temp = ((float)n - 65536.0f) / 256.0f;
    } else {
        // Temperatura positiva
        temp = (float)n / 256.0f;
    }
    return temp;
}

// ─── Lógica de adquisición ────────────────────────────────────────────────────

/**
 * Espera a que el sensor complete la adquisición (Sco bit = 0).
 * Retorna true si completó antes del timeout.
 */
static bool _wait_for_ready(void) {
    uint8_t status;
    uint32_t start = millis();

    while ((millis() - start) < PRESSURE_SENSOR_TIMEOUT_MS) {
        if (!_read_registers(PSEN_REG_STATUS, &status, 1)) {
            return false;   // Error I2C
        }
        if ((status & PSEN_SCO_BIT) == 0) {
            return true;   // Sco = 0 → adquisición completa
        }
        delay(5);   // Pequeño yield para no saturar el bus
    }

    return false;   // Timeout
}

// ─── API pública ──────────────────────────────────────────────────────────────

bool pressureSensor_init(int sda_pin, int scl_pin, uint32_t freq_hz) {
    Wire.begin(sda_pin, scl_pin);
    Wire.setClock(freq_hz);
    delay(10);   // Tiempo de estabilización tras power-on

    return pressureSensor_isPresent();
}

bool pressureSensor_isPresent(void) {
    Wire.beginTransmission(PRESSURE_SENSOR_I2C_ADDR);
    return (Wire.endTransmission() == 0);
}

bool pressureSensor_read(PressureSensorData_t *out) {
    if (out == nullptr) return false;

    out->valid = false;

    // 1. Iniciar adquisición: escribir 0x30 en registro 0x0A
    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) {
        return false;
    }

    // 2. Polling hasta que Sco bit = 0 (o timeout)
    if (!_wait_for_ready()) {
        return false;
    }

    // 3. Delay adicional de seguridad indicado en datasheet (~50ms)
    delay(PRESSURE_SENSOR_ACQ_DELAY_MS);

    // 4. Leer 3 bytes de presión: registros 0x06, 0x07, 0x08
    uint8_t pressure_raw[3];
    if (!_read_registers(PSEN_REG_PRESS_H, pressure_raw, 3)) {
        return false;
    }

    // 5. Leer 2 bytes de temperatura: registros 0x09, 0x0A
    uint8_t temp_raw[2];
    if (!_read_registers(PSEN_REG_TEMP_H, temp_raw, 2)) {
        return false;
    }

    // 6. Convertir
    out->pressure_kpa  = _convert_pressure(pressure_raw[0], pressure_raw[1], pressure_raw[2]);
    out->temperature_c = _convert_temperature(temp_raw[0], temp_raw[1]);
    out->valid = true;

    return true;
}

bool pressureSensor_readPressure(float *pressure_out) {
    if (pressure_out == nullptr) return false;

    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) {
        return false;
    }

    if (!_wait_for_ready()) {
        return false;
    }

    delay(PRESSURE_SENSOR_ACQ_DELAY_MS);

    uint8_t raw[3];
    if (!_read_registers(PSEN_REG_PRESS_H, raw, 3)) {
        return false;
    }

    *pressure_out = _convert_pressure(raw[0], raw[1], raw[2]);
    return true;
}

bool pressureSensor_readTemperature(float *temp_out) {
    if (temp_out == nullptr) return false;

    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) {
        return false;
    }

    if (!_wait_for_ready()) {
        return false;
    }

    delay(PRESSURE_SENSOR_ACQ_DELAY_MS);

    uint8_t raw[2];
    if (!_read_registers(PSEN_REG_TEMP_H, raw, 2)) {
        return false;
    }

    *temp_out = _convert_temperature(raw[0], raw[1]);
    return true;
}
