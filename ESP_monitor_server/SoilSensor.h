#ifndef SOIL_SENSOR_H
#define SOIL_SENSOR_H

#include <Arduino.h>

class SoilSensor
{
public:
    // Constructor: Serial port only (DE/RE no gestionado)
    SoilSensor(HardwareSerial &serial = Serial1);

    // Constructor: Serial port + DE/RE pin
    SoilSensor(HardwareSerial &serial, int dePin);

    // Constructor: Serial port + custom RX/TX pins (ESP32) + DE/RE pin
    SoilSensor(HardwareSerial &serial, int rxPin, int txPin, int dePin);

    // Initialize the sensor with specified baud rate
    bool begin(uint32_t baudRate = 4800);

    // Read all variables at once (most efficient)
    bool readAllVariables();

    // Individual reading functions
    bool readTemperature();
    bool readHumidity();
    bool readEC();
    bool readPH();
    bool readNitrogen();
    bool readPhosphorus();
    bool readPotassium();

    // Get the latest read values
    float getTemperature() const { return temperature; }
    float getHumidity()    const { return humidity; }
    float getEC()          const { return ec; }
    float getPH()          const { return ph; }
    float getNitrogen()    const { return nitrogen; }
    float getPhosphorus()  const { return phosphorus; }
    float getPotassium()   const { return potassium; }

    // Check if readings are fresh (updated since last check)
    bool isTemperatureFresh() { bool t = tempFresh; tempFresh = false; return t; }
    bool isHumidityFresh()    { bool t = humFresh;  humFresh  = false; return t; }
    bool isECFresh()          { bool t = ecFresh;   ecFresh   = false; return t; }
    bool isPHFresh()          { bool t = phFresh;   phFresh   = false; return t; }
    bool isNitrogenFresh()    { bool t = nFresh;    nFresh    = false; return t; }
    bool isPhosphorusFresh()  { bool t = pFresh;    pFresh    = false; return t; }
    bool isPotassiumFresh()   { bool t = kFresh;    kFresh    = false; return t; }

private:
    HardwareSerial &sensorSerial;
    int _rxPin      = -1;
    int _txPin      = -1;
    int _dePin      = -1;
    bool _customPins = false;

    static const byte READ_ALL_REG[8];
    static const byte READ_TEMP_REG[8];
    static const byte READ_HUM_REG[8];
    static const byte READ_EC_REG[8];
    static const byte READ_PH_REG[8];
    static const byte READ_N_REG[8];
    static const byte READ_P_REG[8];
    static const byte READ_K_REG[8];

    byte readBuffer[20] = {0x00};

    float temperature;
    float humidity;
    float ec;
    float ph;
    float nitrogen;
    float phosphorus;
    float potassium;

    bool tempFresh;
    bool humFresh;
    bool ecFresh;
    bool phFresh;
    bool nFresh;
    bool pFresh;
    bool kFresh;

    bool sendCommand(const byte *command, size_t length);
    bool processReadingAll();
};

#endif // SOIL_SENSOR_H
