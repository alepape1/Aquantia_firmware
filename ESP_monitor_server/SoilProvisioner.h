#pragma once
// =============================================================================
// SoilProvisioner — Modbus RS485 address auto-assignment for soil sensors
//
// Problem: YIERYI/RS485 NPK sensors always ship from factory at address 0x01.
// When adding a second sensor to an existing bus, addresses collide.
//
// Solution flow (triggered via MQTT cmd "provision_soil"):
//   1. soilBusScan()      — probe addresses 1..maxAddr, build occupied bitmask
//   2. soilBusProvision() — reprogram factory sensor (0x01) to first free address,
//                           persist assignment to NVS namespace "soil_bus"
//
// Boot integration — add before soilSensor.begin() in setup():
//   uint8_t saved = soilBusLoadAddress();
//   if (saved > 0) soilSensor.setSlaveAddress(saved);
//
// Physical requirement: when provisioning, connect ONLY the new factory sensor
// plus any already-programmed sensors. The new sensor must be the only one at 0x01.
//
// Note on baud rate: all sensors on the same RS485 bus must run at the same baud.
// If your new sensor is at 9600 baud and existing sensors are at 4800, you must
// also write register 0x0101 (value 2 = 4800 bps) before calling this provisioner.
// =============================================================================

#include "SoilSensor.h"
#include <Preferences.h>

static constexpr const char* _SOIL_BUS_NS  = "soil_bus";
static constexpr const char* _SOIL_BUS_KEY = "addr";

// Scans addresses 1..maxAddr (max 15). Returns bitmask (bit N set = address N responded).
// Does NOT modify sensor._slaveAddr — safe to call at any time.
inline uint16_t soilBusScan(SoilSensor& sensor, uint8_t maxAddr = 8)
{
    uint16_t found = 0;
    if (maxAddr > 15) maxAddr = 15;
    for (uint8_t a = 1; a <= maxAddr; a++) {
        if (sensor.probe(a)) {
            found |= (1u << a);
            Serial.printf("[SOIL-BUS] Sensor en 0x%02X\n", a);
        }
        delay(60);  // inter-query gap for bus stability
    }
    return found;
}

// Provisions the factory-default sensor at defaultAddr → first free address >= 2.
// On success: updates sensor._slaveAddr and saves new address to NVS.
// Returns the new address, or 0 on failure.
inline uint8_t soilBusProvision(SoilSensor& sensor,
                                 uint8_t defaultAddr = 1,
                                 uint8_t maxAddr = 8)
{
    Serial.println("[SOIL-BUS] Iniciando provisioning de nuevo sensor...");

    uint16_t occupied = soilBusScan(sensor, maxAddr);

    if (!(occupied & (1u << defaultAddr))) {
        Serial.printf("[SOIL-BUS] No hay sensor respondiendo en dir. 0x%02X\n", defaultAddr);
        return 0;
    }

    uint8_t newAddr = 0;
    for (uint8_t a = 2; a <= maxAddr && a < 16; a++) {
        if (!(occupied & (1u << a))) {
            newAddr = a;
            break;
        }
    }
    if (newAddr == 0) {
        Serial.printf("[SOIL-BUS] Sin direcciones libres en rango 2..%d\n", maxAddr);
        return 0;
    }

    Serial.printf("[SOIL-BUS] Reprogramando 0x%02X → 0x%02X...\n", defaultAddr, newAddr);
    if (!sensor.changeAddress(defaultAddr, newAddr)) {
        Serial.println("[SOIL-BUS] ERROR: changeAddress no recibió confirmación del sensor");
        Serial.println("[SOIL-BUS] Tip: algunos sensores no esperan eco — verificar con probe()");
        // Some sensors restart silently after address change without echoing the response.
        // Try a probe at the new address before giving up.
        delay(400);
        if (!sensor.probe(newAddr)) {
            Serial.printf("[SOIL-BUS] ERROR: sensor no responde en 0x%02X\n", newAddr);
            return 0;
        }
        Serial.println("[SOIL-BUS] Sensor responde en nueva dirección (sin eco)");
    } else {
        delay(300);  // let sensor restart internally and apply new address
        if (!sensor.probe(newAddr)) {
            Serial.printf("[SOIL-BUS] ERROR: sensor no responde en 0x%02X tras el cambio\n", newAddr);
            return 0;
        }
    }

    sensor.setSlaveAddress(newAddr);

    Preferences prefs;
    prefs.begin(_SOIL_BUS_NS, false);
    prefs.putUChar(_SOIL_BUS_KEY, newAddr);
    prefs.end();

    Serial.printf("[SOIL-BUS] OK — sensor activo en 0x%02X (guardado en NVS)\n", newAddr);
    return newAddr;
}

// Load saved slave address from NVS (returns 0 if none saved → keep default 0x01)
inline uint8_t soilBusLoadAddress()
{
    Preferences prefs;
    prefs.begin(_SOIL_BUS_NS, true);
    uint8_t addr = prefs.getUChar(_SOIL_BUS_KEY, 0);
    prefs.end();
    return addr;
}
