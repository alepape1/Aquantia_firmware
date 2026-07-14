// soil_scanner.ino — Diagnóstico de sensor RS485 Modbus
// Prueba todas las velocidades y vuelca bytes crudos al monitor serie.
// Pines configurados para PROFILE_METEO: RX=13, TX=17, DE/RE=27
// Abrir Monitor Serie a 115200 baud.

#define RS485_RX   14
#define RS485_TX   13
#define RS485_DERE 27

// Baud rates a probar (el YIERYI puede salir de fábrica a cualquiera de estos)
const uint32_t BAUDS[] = { 1200, 2400, 4800, 9600, 19200 };
const uint8_t  NBAUDS  = sizeof(BAUDS) / sizeof(BAUDS[0]);

// Modos de paridad a probar
struct UartMode { uint32_t config; const char* name; };
const UartMode MODES[] = {
    { SERIAL_8N1, "8N1" },
    { SERIAL_8E1, "8E1" },
    { SERIAL_8O1, "8O1" },
};
const uint8_t NMODES = sizeof(MODES) / sizeof(MODES[0]);

// Comandos Modbus: leer 7 registros desde 0x0000 — dirección 0x01
// (mismo que el driver halisense/SoilSensor existente)
const uint8_t CMD_ADDR01[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08 };
// Leer 8 registros — por si el YIERYI tiene el 8° (salinidad)
const uint8_t CMD_8REG[8]   = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x08, 0x44, 0x0C };
// Dirección 0x02 — por si alguien ya cambió la dirección de fábrica
const uint8_t CMD_ADDR02[8] = { 0x02, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x3B };

struct Probe { const uint8_t* cmd; const char* label; };
const Probe PROBES[] = {
    { CMD_ADDR01, "addr=0x01 regs=7" },
    { CMD_8REG,   "addr=0x01 regs=8" },
    { CMD_ADDR02, "addr=0x02 regs=7" },
};
const uint8_t NPROBES = sizeof(PROBES) / sizeof(PROBES[0]);

static void sendCmd(const uint8_t* cmd, size_t len)
{
    while (Serial2.available()) Serial2.read();  // flush RX
    digitalWrite(RS485_DERE, HIGH);
    delayMicroseconds(200);
    Serial2.write(cmd, len);
    Serial2.flush();
    delayMicroseconds(500);
    digitalWrite(RS485_DERE, LOW);
    delay(3);
    while (Serial2.available()) Serial2.read();  // flush echo
}

static uint8_t collectResponse(uint8_t* buf, size_t maxLen, uint32_t timeoutMs)
{
    uint8_t  idx       = 0;
    uint32_t t0        = millis();
    uint32_t lastByte  = millis();

    while (idx < maxLen) {
        if (Serial2.available()) {
            buf[idx++] = Serial2.read();
            lastByte = millis();
        } else {
            if (idx > 0 && millis() - lastByte > 30) break;  // inter-byte gap
            if (idx == 0 && millis() - t0 > timeoutMs) break; // first-byte timeout
        }
    }
    return idx;
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n====== SOIL SENSOR RS485 SCANNER ======");
    Serial.printf("RX=GPIO%d  TX=GPIO%d  DE/RE=GPIO%d\n", RS485_RX, RS485_TX, RS485_DERE);

    pinMode(RS485_DERE, OUTPUT);
    digitalWrite(RS485_DERE, LOW);

    for (uint8_t bi = 0; bi < NBAUDS; bi++) {
        for (uint8_t mi = 0; mi < NMODES; mi++) {
            Serial2.begin(BAUDS[bi], MODES[mi].config, RS485_RX, RS485_TX);
            delay(200);  // let UART settle

            for (uint8_t pi = 0; pi < NPROBES; pi++) {
                sendCmd(PROBES[pi].cmd, 8);

                uint8_t buf[32] = {};
                uint8_t n = collectResponse(buf, sizeof(buf), 400);

                if (n > 0) {
                    Serial.printf("\n>>> RESPUESTA ENCONTRADA <<<\n");
                    Serial.printf("    Baud: %lu  Paridad: %s  Comando: %s\n",
                                  BAUDS[bi], MODES[mi].name, PROBES[pi].label);
                    Serial.printf("    Bytes recibidos (%d): ", n);
                    for (uint8_t k = 0; k < n; k++) Serial.printf("%02X ", buf[k]);
                    Serial.println();
                } else {
                    Serial.printf("  [%lu/%s/%s] sin respuesta\n",
                                  BAUDS[bi], MODES[mi].name, PROBES[pi].label);
                }

                delay(50);
            }

            Serial2.end();
            delay(50);
        }
    }

    Serial.println("\n====== SCAN COMPLETO ======");
    Serial.println("Si no hay ninguna linea '>>> RESPUESTA <<<':");
    Serial.println("  1. Verifica que 12V llega al sensor con multimetro");
    Serial.println("  2. Prueba swap fisico amarillo<->verde en A y B");
    Serial.println("  3. El sensor puede estar defectuoso");
}

void loop() {}
