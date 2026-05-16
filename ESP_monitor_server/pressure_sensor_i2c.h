/**
 * @file pressure_sensor_i2c.h
 * @brief Driver para sensor de presión I2C (protocolo datasheet chino)
 *
 * Protocolo verificado desde datasheet físico escaneado.
 *
 * HARDWARE:
 *   - Alimentación: 3.3V
 *   - Pull-ups: 4.7kΩ en SDA y SCL (el sensor los lleva internamente,
 *     pero el datasheet muestra pull-ups externos también)
 *   - Cables: Marrón=VCC(+), Azul=GND, Blanco=SDA, Negro=SCL
 *
 * REGISTROS:
 *   - 0x30 → registro de control/estado (read/write):
 *              WRITE 0x0A → inicia adquisición
 *              READ  bit3 (Sco) = 1 ocupado, 0 = listo
 *   - 0x06, 0x07, 0x08 → datos de presión (24-bit, big-endian)
 *   - 0x09, 0x0A → datos de temperatura (16-bit, big-endian)
 *
 * CONVERSIÓN PRESIÓN (24-bit con signo, complemento a 2):
 *   m = REG06<<16 | REG07<<8 | REG08
 *   Si m >= 2^23 (bit de signo=1): pressure = (m - 2^24) / 2^23 * Fullscale
 *   Si m <  2^23 (bit de signo=0): pressure = m / 2^23 * Fullscale
 *
 * CONVERSIÓN TEMPERATURA (16-bit con signo, complemento a 2):
 *   n = REG09<<8 | REG0A
 *   Si n >= 2^15 (bit de signo=1): temp = (n - 2^16) / 256
 *   Si n <  2^15 (bit de signo=0): temp = n / 256
 *
 * DIRECCIÓN I2C (Aquantia — hardware gateway):
 *   0x7F — lote alternativo confirmado en hardware instalado.
 *   Si un sensor responde en 0x6D (lote estándar), cambiar PRESSURE_SENSOR_I2C_ADDR.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef ARDUINO
  #include <Wire.h>
#endif

// ─── Configuración ────────────────────────────────────────────────────────────

/** Dirección I2C del sensor (7-bit). Hardware Aquantia: 0x7F (lote alternativo confirmado). */
#define PRESSURE_SENSOR_I2C_ADDR   0x7F

/**
 * Presión fondo de escala en kPa (rango del sensor instalado en Aquantia).
 *   0-4 bar  (riego doméstico) → 400 kPa
 *   0-10 bar (0-1 MPa)        → 1000 kPa  ← sensor instalado en Aquantia
 *   0-40 bar                  → 4000 kPa
 */
#define PRESSURE_SENSOR_FULLSCALE  1000.0f  // kPa — sensor 0-1 MPa (0-10 bar) Aquantia

/**
 * Frecuencia I2C para las transacciones del sensor de presión (Hz).
 * 50 kHz en vez de 100 kHz: con un cable de ~1 m la capacidad parásita
 * (~100-150 pF) estrecha el margen de subida a 100 kHz. A 50 kHz el
 * semiciclo dobla (20 µs) y la señal sube con holgura incluso con connectors
 * adicionales en el recorrido.
 */
#define PRESSURE_SENSOR_I2C_FREQ_HZ  50000U

/**
 * Tiempo de adquisición fijo antes de leer el registro de estado (ms).
 * El XGZP6847D completa la conversión en ~35-50 ms según datasheet.
 * 60 ms da margen extra sin bloquear el bus innecesariamente.
 */
#define PRESSURE_SENSOR_ACQ_DELAY_MS 60

/**
 * Timeout total de la lectura si el sensor no reporta conversión OK (ms).
 * ACQ_DELAY + hasta 2 checks de 30 ms = 60+60 = 120 ms nominales; 300 ms
 * cubre fallos transitorios de señal en cable largo sin bloquear el loop.
 */
#define PRESSURE_SENSOR_TIMEOUT_MS   300

// ─── Registros (no tocar) ─────────────────────────────────────────────────────

#define PSEN_REG_TRIGGER    0x30   // Registro control/estado (read/write): WRITE 0x0A inicia adq.
#define PSEN_REG_STATUS     0x30   // Mismo registro: READ bit3 (Sco) = 1 → ocupado
#define PSEN_REG_PRESS_H    0x06   // Byte alto presión
#define PSEN_REG_PRESS_M    0x07   // Byte medio presión
#define PSEN_REG_PRESS_L    0x08   // Byte bajo presión
#define PSEN_REG_TEMP_H     0x09   // Byte alto temperatura
#define PSEN_REG_TEMP_L     0x0A   // Byte bajo temperatura (mismo que TRIGGER, post-lectura)

#define PSEN_CMD_START_ACQ  0x0A   // Valor a escribir en PSEN_REG_TRIGGER para iniciar adquisición

#define PSEN_SCO_BIT        (1 << 3)  // Bit 3 del registro de estado

// ─── Estructuras ──────────────────────────────────────────────────────────────

typedef struct {
    float pressure_kpa;   ///< Presión en kPa (escala PRESSURE_SENSOR_FULLSCALE)
    float temperature_c;  ///< Temperatura en °C
    bool  valid;          ///< true si la lectura fue exitosa
} PressureSensorData_t;

// ─── API pública ──────────────────────────────────────────────────────────────

/**
 * @brief Inicializa el bus I2C y verifica comunicación con el sensor.
 * @param sda_pin  Pin SDA
 * @param scl_pin  Pin SCL
 * @param freq_hz  Frecuencia I2C en Hz (recomendado: 100000)
 * @return true si el sensor responde, false si no se detecta
 */
bool pressureSensor_init(int sda_pin, int scl_pin, uint32_t freq_hz);

/**
 * @brief Lee presión y temperatura del sensor.
 *
 * Flujo interno:
 *   1. Escribe 0x0A en reg 0x30 → inicia adquisición
 *   2. Polling de reg 0x30 hasta Sco=0 (o timeout de PRESSURE_SENSOR_TIMEOUT_MS)
 *   3. Delay adicional de PRESSURE_SENSOR_ACQ_DELAY_MS ms
 *   4. Lee 3 bytes de presión desde reg 0x06
 *   5. Lee 2 bytes de temperatura desde reg 0x09
 *   6. Aplica conversión con signo (complemento a 2)
 *
 * @param out  Puntero a estructura donde se escriben los resultados
 * @return true si la lectura fue correcta
 */
bool pressureSensor_read(PressureSensorData_t *out);

/**
 * @brief Lee solo la presión en kPa.
 * @param pressure_out  Puntero a float donde se escribe la presión
 * @return true si OK
 */
bool pressureSensor_readPressure(float *pressure_out);

/**
 * @brief Lee solo la temperatura en °C.
 * @param temp_out  Puntero a float donde se escribe la temperatura
 * @return true si OK
 */
bool pressureSensor_readTemperature(float *temp_out);

/**
 * @brief Comprueba si el sensor está accesible en el bus I2C.
 * @return true si ACK recibido
 */
bool pressureSensor_isPresent(void);

/**
 * @brief Recuperación de bus I2C atascado (UM10204 §3.1.16).
 *
 * Genera 9 pulsos SCL en modo GPIO para liberar cualquier dispositivo
 * que tenga SDA bloqueado en LOW tras un glitch de señal. A continuación
 * emite una condición STOP y reinicializa Wire a PRESSURE_SENSOR_I2C_FREQ_HZ.
 *
 * Llamar desde xdb401_begin() cuando isPresent() falla, antes del reintento.
 * Solo disponible en Arduino/ESP32.
 */
void pressureSensor_recover(void);
