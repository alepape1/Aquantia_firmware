/**
 * @file pressure_sensor_i2c.cpp
 * @brief Implementación del driver para sensor de presión I2C
 *
 * Implementado para ESP32 con Arduino framework.
 * Basado en datasheet físico del fabricante chino (familia XGZP6847D).
 */

#include "pressure_sensor_i2c.h"
// Pines I2C almacenados en init() y usados por recover()
static int      _sda_pin    = -1;
static int      _scl_pin    = -1;
static uint32_t _sensor_freq = PRESSURE_SENSOR_I2C_FREQ_HZ;  // 50 kHz para cable largo
static uint32_t _normal_freq = 100000;  // frecuencia del resto del bus; se restaura al soltar

// Reduce la frecuencia del bus al valor del sensor (50 kHz) antes de cada operación
// y la restaura al terminar. Así los demás sensores siguen a 100 kHz.
static inline void _bus_claim()   { Wire.setClock(_sensor_freq); }
static inline void _bus_release() { Wire.setClock(_normal_freq); }
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
 *
 * Estrategia de baja interferencia para cable largo:
 *   1. Espera fija PRESSURE_SENSOR_ACQ_DELAY_MS (60 ms) — cubre el tiempo
 *      de conversión típico sin generar ningún tráfico I2C.
 *   2. Lee el registro de estado una vez. Si Sco=0 → listo.
 *   3. Si aún ocupado, espera 30 ms más y reintenta una última vez.
 *
 * Esto reduce las transacciones I2C de ~40 (bucle 5 ms) a un máximo de 2,
 * lo que minimiza las interferencias en el bus compartido y el ruido de
 * reflexión en el cable.
 */
static bool _wait_for_ready(void) {
    // Espera principal: la conversión típica del XGZP6847D tarda ~35-50 ms.
    delay(PRESSURE_SENSOR_ACQ_DELAY_MS);

    // Hasta 2 comprobaciones con una pausa breve entre ellas.
    for (int attempt = 0; attempt < 2; attempt++) {
        uint8_t status = 0xFF;
        if (!_read_registers(PSEN_REG_STATUS, &status, 1)) {
            return false;   // Error I2C — línea probablemente inestable
        }
        if ((status & PSEN_SCO_BIT) == 0) {
            return true;    // Sco = 0 → conversión completa
        }
        delay(30);          // Conversión aún en progreso — breve espera extra
    }

    return false;           // No listo tras ACQ_DELAY + 2 × 30 ms
}

// ─── API pública ──────────────────────────────────────────────────────────────

bool pressureSensor_init(int sda_pin, int scl_pin, uint32_t freq_hz) {
    _sda_pin     = sda_pin;
    _scl_pin     = scl_pin;
    _sensor_freq = PRESSURE_SENSOR_I2C_FREQ_HZ;  // siempre 50 kHz para cable largo
    _normal_freq = (freq_hz > PRESSURE_SENSOR_I2C_FREQ_HZ) ? freq_hz : 100000;
    // NO se llama Wire.begin() aquí — el bus ya está inicializado por setup().
    // Solo verificamos la presencia del sensor (bajo la frecuencia del bus normal).
    return pressureSensor_isPresent();
}

void pressureSensor_recover(void) {
    // Recuperación de bus I2C atascado (UM10204 §3.1.16).
    // Si un glitch hace que un esclavo pierda la cuenta de su byte, SDA
    // queda en LOW indefinidamente. La solución es generar hasta 9 pulsos
    // SCL en modo GPIO hasta que el esclavo libere SDA, seguido de STOP.
#ifdef ARDUINO
    Wire.end();
    delay(5);

    if (_sda_pin >= 0 && _scl_pin >= 0) {
        // Configurar SCL como salida, SDA como entrada con pull-up
        pinMode(_scl_pin, OUTPUT);
        digitalWrite(_scl_pin, HIGH);
        pinMode(_sda_pin, INPUT_PULLUP);
        delayMicroseconds(10);

        // 9 pulsos SCL — a 50 kHz el semiciclo es 10 µs
        for (int i = 0; i < 9; i++) {
            digitalWrite(_scl_pin, LOW);
            delayMicroseconds(10);
            digitalWrite(_scl_pin, HIGH);
            delayMicroseconds(10);
            if (digitalRead(_sda_pin) == HIGH) break;  // SDA libre → listo
        }

        // Condición STOP: SDA pasa de LOW a HIGH con SCL en HIGH
        pinMode(_sda_pin, OUTPUT);
        digitalWrite(_sda_pin, LOW);
        delayMicroseconds(5);
        digitalWrite(_scl_pin, HIGH);
        delayMicroseconds(5);
        digitalWrite(_sda_pin, HIGH);
        delayMicroseconds(5);
    }

    // Reinicializar bus a frecuencia normal del sistema (no a la del sensor)
    Wire.begin(_sda_pin, _scl_pin);
    Wire.setClock(_normal_freq);
    delay(20);   // Estabilización del bus tras recovery
#endif
}

bool pressureSensor_isPresent(void) {
    _bus_claim();
    Wire.beginTransmission(PRESSURE_SENSOR_I2C_ADDR);
    bool ok = (Wire.endTransmission() == 0);
    _bus_release();
    return ok;
}

bool pressureSensor_read(PressureSensorData_t *out) {
    if (out == nullptr) return false;

    out->valid = false;
    _bus_claim();   // bajar a 50 kHz sólo para este sensor

    // 1. Iniciar adquisición: escribir 0x0A en registro 0x30
    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) {
        _bus_release(); return false;
    }

    // 2. Esperar conversión + verificar Sco=0 (delay incluido en _wait_for_ready)
    if (!_wait_for_ready()) {
        _bus_release(); return false;
    }

    // 3. Leer 3 bytes de presión: registros 0x06, 0x07, 0x08
    uint8_t pressure_raw[3];
    if (!_read_registers(PSEN_REG_PRESS_H, pressure_raw, 3)) {
        _bus_release(); return false;
    }

    // 4. Leer 2 bytes de temperatura: registros 0x09, 0x0A
    uint8_t temp_raw[2];
    if (!_read_registers(PSEN_REG_TEMP_H, temp_raw, 2)) {
        _bus_release(); return false;
    }

    _bus_release();   // restaurar 100 kHz antes de convertir

    out->pressure_kpa  = _convert_pressure(pressure_raw[0], pressure_raw[1], pressure_raw[2]);
    out->temperature_c = _convert_temperature(temp_raw[0], temp_raw[1]);
    out->valid = true;
    return true;
}

bool pressureSensor_readPressure(float *pressure_out) {
    if (pressure_out == nullptr) return false;

    _bus_claim();
    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) { _bus_release(); return false; }
    if (!_wait_for_ready())                                       { _bus_release(); return false; }

    uint8_t raw[3];
    if (!_read_registers(PSEN_REG_PRESS_H, raw, 3))              { _bus_release(); return false; }
    _bus_release();

    *pressure_out = _convert_pressure(raw[0], raw[1], raw[2]);
    return true;
}

bool pressureSensor_readTemperature(float *temp_out) {
    if (temp_out == nullptr) return false;

    _bus_claim();
    if (!_write_register(PSEN_REG_TRIGGER, PSEN_CMD_START_ACQ)) { _bus_release(); return false; }
    if (!_wait_for_ready())                                       { _bus_release(); return false; }

    uint8_t raw[2];
    if (!_read_registers(PSEN_REG_TEMP_H, raw, 2))              { _bus_release(); return false; }
    _bus_release();

    *temp_out = _convert_temperature(raw[0], raw[1]);
    return true;
}
