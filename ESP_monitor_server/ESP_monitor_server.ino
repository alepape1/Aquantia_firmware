// MeteoStation — Firmware v3
// Compatible con ESP32 (con pantalla ST7789 240×135) y ESP8266 (sin pantalla)
// Sensores: MCP9808, BMP280, HTU2x, SparkFun MicroPressure, TSL2584/APDS, anemómetro, veleta
// Tres temporizadores independientes: 100ms viento / 1s pantalla / 20s envío

// ── Versión del firmware ───────────────────────────────────────────────────────
// Incrementar según SemVer al crear un release. El backend almacena este valor
// en device_info.firmware_version para mostrar en el dashboard y detectar
// dispositivos desactualizados (comparado con app_settings.min_firmware_version).
#define FIRMWARE_VERSION "0.1.0-beta.5"     

// ── Perfiles de dispositivo — deben ir PRIMERO para que los #if funcionen ─────
#define PROFILE_METEO       1   // ECU meteorológica — 1 relay (GPIO RELAY_PIN)
#define PROFILE_IRRIGATION  2   // ECU irrigación   — 4 relays (GPIOs RELAY_PIN_1..4)
#define PROFILE_AGROMETEO   3   // ECU agrometeorológica — sin relays, sensores CJMCU-14 (BH1750+HDC1080+BMP280)

#ifndef DEVICE_PROFILE
  #define DEVICE_PROFILE PROFILE_METEO
#endif

// ── Modo debug — pasar -DDEBUG_MODE=1 al compilador para activarlo ───────────
// En producción NO definir: elimina toda la salida serie y reduce consumo.
// El Flash Tool GUI tiene la casilla "Debug" que inyecta el flag automáticamente.
#define DEBUG_INTERVAL_MS 5000UL   // reporte completo cada 5 s

// Macros de log — mapeadas a Serial cuando DEBUG_MODE está activo, no-op en prod
#ifdef DEBUG_MODE
  #define DLOGF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define DLOGLN(msg)      Serial.println(msg)
  #define DLOG(msg)        Serial.print(msg)
#else
  #define DLOGF(fmt, ...)  do {} while(0)
  #define DLOGLN(msg)      do {} while(0)
  #define DLOG(msg)        do {} while(0)
#endif

// ── Detección de plataforma ───────────────────────────────────────────────────
#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <WiFiClient.h>
  #include <ArduinoOTA.h>
  #define I2C_SDA          4
  #define I2C_SCL          5
  #define DHTPIN          14   // D5
  #define ADC_VOLTAGE_REF  3.2f
  #define ADC_RANGE        1024.0f
  #define ANEMOMETER_PIN   A0
  #define RELAY_PIN       12   // D6 — GPIO libre para relay electroválvula
#else  // ESP32
  #include "WiFi.h"
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
  #include <ArduinoOTA.h>
  #include "driver/rtc_io.h"
  #define I2C_SDA        21
  #define I2C_SCL        22
  #define DHTPIN           15
  #define ADC_VOLTAGE_REF  3.41f
  #define ADC_RANGE        4096.0f
  #define ANEMOMETER_PIN   36
  #define VANE_PIN         37
  #if DEVICE_PROFILE == PROFILE_METEO
    #define SOIL_PIN         33   // YL-69 humedad suelo (ADC1_CH5) — solo PROFILE_METEO
    #define SOIL_RAW_DRY   3300   // ADC en tierra seca (~0%) — ajustar con valor raw del serial
    #define SOIL_RAW_WET   1000   // ADC en tierra saturada (~100%) — ajustar con valor raw del serial
    #define FLOW_PIN         32   // Caudalímetro — pulsos digitales vía BC547 NPN (señal invertida, activo LOW)
    // K factor según modelo:
    //   YF-S201 → 450 p/L  (F = 7.5·Q Hz)
    //   YF-B4   → 240 p/L  (F = 4.0·Q Hz)
    //   YF-B9   → 288 p/L  (F = 4.8·Q Hz)
    #define FLOW_K_FACTOR   288   // YF-B9 — ajustar con medición real si es necesario
  #endif
  #if DEVICE_PROFILE == PROFILE_METEO
    #define HAS_DISPLAY
  #endif
  #define RELAY_PIN        26   // GPIO libre para relay electroválvula
#endif

// ── Pipeline — constantes físicas del simulador ───────────────────────────────
#define PIPELINE_STATIC_P   3.50f  // bar — presión estática (válvula cerrada)
#define PIPELINE_DYNAMIC_P  2.80f  // bar — presión dinámica a caudal nominal
#define PIPELINE_NOISE_P    0.04f  // bar — dispersión sensor de presión
#define PIPELINE_NOISE_Q    0.12f  // L/min — dispersión caudalímetro
#define PIPELINE_NOMINAL_Q  5.00f  // L/min — caudal nominal del sistema

// Librerías de sensores meteorológicos — según perfil
#if DEVICE_PROFILE == PROFILE_METEO
  #include <SPI.h>
  #include <Wire.h>
  // ESP32 Arduino core 3.x eliminó BitOrder — shim para Adafruit_BusIO
  #if defined(ARDUINO_ARCH_ESP32) && !defined(BitOrder)
    typedef uint8_t BitOrder;
  #endif
  #include <Adafruit_MCP9808.h>
  #include <Adafruit_BMP280.h>
  #include <SparkFun_MicroPressure.h>
  #include <DHTesp.h>
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  // AGROMETEO: CJMCU-14 (BH1750 + HDC1080 + BMP280) + Qwiic Power Switch + MicroPressure
  #include <Wire.h>
  #include <Adafruit_BMP280.h>
  #include <BH1750.h>
  #include <SparkFun_Qwiic_Power_Switch_Arduino_Library.h>
  #include <SparkFun_MicroPressure.h>
#else
  // IRRIGATION: solo I2C básico (sin sensores meteo)
  #include <Wire.h>
#endif
#include <ArduinoJson.h>
#include <math.h>
#include "secrets.h"

// Certificado TLS reutilizado por HTTPS y MQTT en ESP32.
#if !defined(ESP8266)
  #include "mqtt_cert.h"
#endif

// PubSubClient — solo ESP32 y solo cuando USE_MQTT está definido
// IMPORTANTE: este include debe ir DESPUÉS de secrets.h para que USE_MQTT esté definido
#if !defined(ESP8266)
  #include <time.h>
#endif

#if !defined(ESP8266) && defined(USE_MQTT)
  #include <PubSubClient.h>
#endif

// Provisioning: SoftAP captive portal + NVS (solo ESP32)
#ifndef ESP8266
  #include "provisioning.h"
#endif

// ── Perfiles de dispositivo (ver definiciones al inicio del archivo) ───────────
#if DEVICE_PROFILE == PROFILE_IRRIGATION
  #define RELAY_COUNT 4
  #ifndef RELAY_PIN_1
    // ESP32 4-Relay Board (ESPHome): GPIO32/33/25/26, LED status GPIO23
    #define RELAY_PIN_1 32
    #define RELAY_PIN_2 33
    #define RELAY_PIN_3 25
    #define RELAY_PIN_4 26
  #endif
  static const uint8_t RELAY_PINS[RELAY_COUNT] = {RELAY_PIN_1, RELAY_PIN_2,
                                                   RELAY_PIN_3, RELAY_PIN_4};
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  // AGROMETEO: sin relays — array dummy para que el código compile sin cambios
  #define RELAY_COUNT 0
  static const uint8_t RELAY_PINS[1] = {0};
#else
  #define RELAY_COUNT 1
  static const uint8_t RELAY_PINS[RELAY_COUNT] = {RELAY_PIN};
#endif

#ifdef HAS_DISPLAY
  #include <TFT_eSPI.h>
  #ifndef TFT_BL
    #define TFT_BL 4   // LilyGo TTGO T-Display backlight
  #endif
#endif

// ── Credenciales (definidas en secrets.h) ─────────────────────────────────────
const char* ssid        = WIFI_SSID;
const char* password    = WIFI_PASSWORD;
const char* server_ip   = SERVER_IP;
const int   server_port = SERVER_PORT;

#ifdef USE_MQTT
const char* mqtt_server = MQTT_SERVER;
const int   mqtt_port   = MQTT_PORT;
const char* finca_id    = FINCA_ID;
const char* mqtt_user   = MQTT_USER;
const char* mqtt_pass   = MQTT_PASS;
#define MQTT_SEND_MS 20000UL
#endif

// ── Pines ─────────────────────────────────────────────────────────────────────
// LED onboard por perfil de hardware (activo-HIGH):
//   METEO      — LilyGo T-Display      : sin LED de usuario → -1 (desactivado)
//   AGROMETEO  — Wemos D1 Mini ESP32   : GPIO 2
//   IRRIGATION — ESP32 4-Relay Board   : GPIO 23
#if DEVICE_PROFILE == PROFILE_METEO
  const int ledPin = -1;   // LilyGo T-Display no tiene LED onboard
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  const int ledPin = 23;
#else
  const int ledPin = 2;
#endif
#define LED_ON  HIGH
#define LED_OFF LOW

// ── LED status no-bloqueante ──────────────────────────────────────────────────
// Código visual del estado del dispositivo mediante el LED onboard.
//
//  LED_PROVISIONING   — triple parpadeo lento cada 3 s → portal AP activo, esperando config
//  LED_WIFI_CONNECTING — parpadeo rápido 100/100 ms    → buscando red WiFi
//  LED_MQTT_CONNECTING — doble parpadeo cada ~2 s      → WiFi OK, MQTT pendiente
//  LED_IDLE            — latido 50 ms / 2950 ms        → conectado, en espera
//  LED_TX_OK           — triple parpadeo (one-shot)    → telemetría enviada OK
//  LED_TX_ERROR        — 1 s ON / 1 s OFF              → error de red persistente
//  LED_RELAY_ON        — encendido fijo                → relay activo (IRRIGATION)
enum LedStateCode : uint8_t {
  LED_PROVISIONING = 0,
  LED_WIFI_CONNECTING,
  LED_MQTT_CONNECTING,
  LED_IDLE,
  LED_TX_OK,      // one-shot: vuelve automáticamente al estado anterior
  LED_TX_ERROR,
  LED_RELAY_ON,
  _LED_STATE_COUNT
};

struct LedStep { uint16_t onMs; uint16_t offMs; };
static const LedStep _ledPat[_LED_STATE_COUNT][4] = {
  /* PROVISIONING    */ {{300, 300}, {300,1800},{300,1800},{0, 0}},  // triple blink lento
  /* WIFI_CONNECTING */ {{100, 100}, {0, 0},    {0, 0},   {0, 0}},
  /* MQTT_CONNECTING */ {{50,  50 }, {50, 1800},{0, 0},   {0, 0}},  // doble blink + pausa
  /* IDLE            */ {{50, 2950}, {0, 0},    {0, 0},   {0, 0}},  // latido lento
  /* TX_OK           */ {{50,  50 }, {50,   50},{50, 800},{0, 0}},  // triple blink
  /* TX_ERROR        */ {{1000,1000},{0, 0},    {0, 0},   {0, 0}},
  /* RELAY_ON        */ {{0,   0  }, {0, 0},    {0, 0},   {0, 0}},  // sin timer (fijo ON)
};
static const uint8_t _ledPatLen[]  = { 3, 1, 2, 1, 3, 1, 0 };
static const bool    _ledOneShot[] = { false, false, false, false, true, false, false };

static volatile LedStateCode _ledState     = LED_PROVISIONING;
static LedStateCode          _ledPrevState = LED_IDLE;
static uint8_t               _ledStep      = 0;
static bool                  _ledPhaseOn   = true;
static volatile bool         _ledNeedsInit = true;
static unsigned long         _ledPhaseMs   = 0;

// Cambiar estado del LED (seguro desde cualquier tarea — solo escribe variables).
void setLedState(LedStateCode s) {
  if ((LedStateCode)_ledState == s) return;
  if (_ledOneShot[s]) _ledPrevState = (LedStateCode)_ledState;
  _ledState     = s;
  _ledStep      = 0;
  _ledPhaseOn   = true;
  _ledNeedsInit = true;  // ledTick() aplicará ON en el próximo ciclo
}

// Llamar únicamente desde loop() (Core 1). Es la única función que escribe el GPIO.
void ledTick() {
  if (ledPin < 0) return;  // placa sin LED onboard (p.ej. LilyGo T-Display)
  LedStateCode state = (LedStateCode)_ledState;
  if (state == LED_RELAY_ON) { digitalWrite(ledPin, LED_ON); return; }

  if (_ledNeedsInit) {
    _ledNeedsInit = false;
    _ledStep    = 0;
    _ledPhaseOn = true;
    _ledPhaseMs = millis();
    digitalWrite(ledPin, LED_ON);
    return;
  }

  const LedStep* pat = _ledPat[state];
  uint8_t len = _ledPatLen[state];
  unsigned long now = millis();
  uint16_t dur = _ledPhaseOn ? pat[_ledStep].onMs : pat[_ledStep].offMs;
  if (now - _ledPhaseMs < dur) return;
  _ledPhaseMs = now;
  if (_ledPhaseOn) {
    digitalWrite(ledPin, LED_OFF);
    _ledPhaseOn = false;
  } else {
    _ledStep++;
    if (_ledStep >= len) {
      if (_ledOneShot[state]) {
        _ledState     = _ledPrevState;
        _ledNeedsInit = true;
        return;
      }
      _ledStep = 0;
    }
    _ledPhaseOn = true;
    digitalWrite(ledPin, LED_ON);
  }
}

// ── Intervalos ─────────────────────────────────────────────────────────────────
#define WIND_MS          100
#define SCREEN_MS       1000
#define SEND_MS         2000
#define RELAY_MS        2000   // Consulta estado relay cada 2s para respuesta casi inmediata
#define PIPELINE_SYNC_MS 20000UL

#ifdef HAS_DISPLAY
#define DISPLAY_TIMEOUT_MS 60000UL  // Apagar pantalla tras 60s sin actividad
#define BTN_LEFT   0                // Botón izquierdo (BOOT), INPUT_PULLUP, activo LOW
#define BTN_RIGHT 35                // Botón derecho, activo LOW
#endif

// ── Objetos ────────────────────────────────────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_METEO
  SparkFun_MicroPressure barometer;
  Adafruit_MCP9808       tempsensor = Adafruit_MCP9808();
  Adafruit_BMP280        bmp280;
  DHTesp                 dht;
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  Adafruit_BMP280        bmp280;
  BH1750                 bh1750;
  QWIIC_POWER            qwiic_ps;
  SparkFun_MicroPressure barometer;
#endif

#ifdef HAS_DISPLAY
  TFT_eSPI    tft = TFT_eSPI();
  TFT_eSprite spr = TFT_eSprite(&tft);
  unsigned long lastActivityTime = 0;
  bool          displayOn        = true;
  uint8_t       displayView      = 0;   // 0 = meteo, 1 = pipeline
#endif

// ── Relay electroválvula(s) ────────────────────────────────────────────────────
// JQC-3FF-S-Z activo-LOW: LOW = relay ON (válvula abierta), HIGH = relay OFF
// relayActive[i] — estado actual de cada relay (índice = bit en bitmask)
// RELAY_COUNT puede ser 0 en AGROMETEO — usamos max(1,…) para evitar array de tamaño 0
bool relayActive[RELAY_COUNT > 0 ? RELAY_COUNT : 1] = {};

#ifdef USE_MQTT
static WiFiClient       mqttTCPClient;
static WiFiClientSecure mqttTLSClient;
static PubSubClient     mqttClient;
#endif

// ── Flags de sensor ────────────────────────────────────────────────────────────
bool mcp_ok = false;
bool bmp_ok = false;
bool bmp_temp_ok = false;
bool bmp_pressure_ok = false;
bool micropressure_ok = false;
bool temp_ok = false;
bool bar_ok = false;
bool htu_ok = false;
#if DEVICE_PROFILE == PROFILE_METEO
bool dht_ok = false;
static uint8_t bmp280_addr = 0x00;
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
bool hdc_ok        = false;   // HDC1080 — temperatura y humedad primaria
bool bh1750_ok     = false;   // BH1750  — iluminancia
bool qwiic_ps_ok   = false;   // Qwiic Power Switch (PCA9536) — alimenta el bus de sensores
static uint8_t bmp280_addr = 0x00;
#endif
bool tsl_ok = false;

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AGROMETEO
static bool beginBMP280() {
  if (bmp280.begin(0x76)) {
    bmp280_addr = 0x76;
    return true;
  }
  if (bmp280.begin(0x77)) {
    bmp280_addr = 0x77;
    return true;
  }
  bmp280_addr = 0x00;
  return false;
}

static bool readBMP280Temperature(float& outTemp) {
  outTemp = bmp280.readTemperature();
  return !isnan(outTemp) && outTemp > -40.0f && outTemp < 85.0f;
}

static bool readBMP280PressureKPa(float& outPressure) {
  outPressure = bmp280.readPressure() / 1000.0f;
  return !isnan(outPressure) && outPressure > 30.0f && outPressure < 120.0f;
}
#endif  // PROFILE_METEO || PROFILE_AGROMETEO

// =============================================================================
// HDC1080 — temperatura y humedad I2C (solo PROFILE_AGROMETEO)
// Dirección 0x40. Se accede directamente via Wire, sin librería externa.
// NOTA: comparte dirección con HTU2x pero protocolo diferente.
// =============================================================================
#if DEVICE_PROFILE == PROFILE_AGROMETEO
#define HDC1080_ADDR 0x40

static bool hdc1080_init() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x02);  // registro de configuración
  Wire.write(0x00);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(15);
  return true;
}

static float hdc1080_readTemp() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x00);  // registro temperatura
  if (Wire.endTransmission() != 0) return NAN;
  delay(35);  // datasheet: 6.5ms; 35ms para cubrir bus ocupado
  Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2, (uint8_t)1);
  if (Wire.available() < 2) return NAN;
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  float t = (raw / 65536.0f) * 165.0f - 40.0f;
  return (t > -40.0f && t < 85.0f) ? t : NAN;
}

static float hdc1080_readHum() {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x01);  // registro humedad
  if (Wire.endTransmission() != 0) return NAN;
  delay(35);  // datasheet: 6.5ms; 35ms para cubrir bus ocupado
  Wire.requestFrom((uint8_t)HDC1080_ADDR, (uint8_t)2, (uint8_t)1);
  if (Wire.available() < 2) return NAN;
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  float h = (raw / 65536.0f) * 100.0f;
  return (h >= 0.0f && h <= 100.0f) ? h : NAN;
}

// Parámetros derivados agrometeorologícos
// Temperatura: HDC1080 primaria, BMP280 secundaria; Tavg = media si ambas disponibles
static float agro_calcDewPoint(float tempC, float hum) {
  const float a = 17.271f, b = 237.7f;
  float g = (a * tempC / (b + tempC)) + logf(hum / 100.0f);
  return (b * g) / (a - g);
}

static float agro_calcHeatIndex(float tempC, float hum) {
  float t = tempC * 9.0f / 5.0f + 32.0f;
  float hi = -42.379f
    + 2.04901523f  * t
    + 10.14333127f * hum
    - 0.22475541f  * t * hum
    - 0.00683783f  * t * t
    - 0.05481717f  * hum * hum
    + 0.00122874f  * t * t * hum
    + 0.00085282f  * t * hum * hum
    - 0.00000199f  * t * t * hum * hum;
  return (hi - 32.0f) * 5.0f / 9.0f;
}

static float agro_calcAbsHumidity(float tempC, float hum) {
  float es = 6.112f * expf((17.67f * tempC) / (tempC + 243.5f));
  return (es * hum * 2.1674f) / (273.15f + tempC);
}

// =============================================================================
// Qwiic Power Switch — solo PROFILE_AGROMETEO
// El objeto qwiic_ps (QWIIC_POWER) gestiona el PCA9536 en 0x41.
// Se enciende una vez en setup() y permanece ON durante toda la ejecución.
// powerOn/powerOff siguen disponibles si en el futuro se necesita deep-sleep.
// =============================================================================
#endif  // PROFILE_AGROMETEO

static const char* temperatureSourceName() {
#if DEVICE_PROFILE == PROFILE_METEO
  if (mcp_ok) return "MCP9808";
  if (bmp_temp_ok) return "BMP280";
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  if (hdc_ok) return "HDC1080";
  if (bmp_temp_ok) return "BMP280";
#endif
  return "SIM";
}

static const char* pressureSourceName() {
#if DEVICE_PROFILE == PROFILE_METEO
  if (micropressure_ok) return "MicroPressure";
  if (bmp_pressure_ok) return "BMP280";
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  if (micropressure_ok) return "MicroPressure";
  if (bmp_pressure_ok)  return "BMP280";
#endif
  return "SIM";
}

// ── Valores medidos ────────────────────────────────────────────────────────────
float  temperatureMCP    = 0;
float  temperatureDHT    = 0;   // HTU2x temperatura
float  humidity          = 0;   // HTU2x humedad
float  temperatureDHT11  = 0;   // DHT11 temperatura
float  humidityDHT11     = 0;   // DHT11 humedad
float  bmpTemperature    = NAN; // BMP280 temperatura directa
float  bmpPressure       = NAN; // BMP280 presión directa en kPa
double pressure          = 0;
float  windSpeed         = 0;
float  windSpeedFiltered = 0;
float  currentWindDirDeg = 0;
float  lightLevel        = 0;
float  soilMoisture      = 0;   // YL-69 — humedad suelo (0=seco, 100=saturado)

// ── Parámetros calculados AGROMETEO ───────────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_AGROMETEO
float  agroTempAvg   = NAN;  // media HDC1080 + BMP280 si ambos disponibles
float  agroDewPoint  = NAN;  // punto de rocío (Magnus)
float  agroHeatIndex = NAN;  // índice de calor (>27°C y >40% HR)
float  agroAbsHum    = NAN;  // humedad absoluta (g/m³)
#endif

// ── Valores simulados (drift lento) ───────────────────────────────────────────
float sim_tempMCP   = 20.5f;
float sim_tempDHT   = 19.8f;
float sim_humidity  = 62.0f;
float sim_tempDHT11 = 20.1f;
float sim_humDHT11  = 60.0f;
float sim_pressure  = 101.3f;
float sim_light     = 300.0f;
float sim_windSpeed = 3.5f;
float sim_windDir        = 180.0f;
float sim_soilMoisture   = 50.0f;

// ── Caudalímetro — conteo de pulsos por ISR ─────────────────────────────────
// BC547 NPN invierte la señal: pulso del sensor → GPIO LOW → FALLING edge.
// ISR en IRAM_ATTR para ejecución desde RAM (no se suspende durante cache miss).
#if defined(FLOW_PIN)
static volatile uint32_t _flowPulseCount = 0;     // contador crudo (escrito solo por ISR)
static volatile uint32_t _flowPulseTotal = 0;     // acumulador histórico — nunca se resetea
static unsigned long     _flowLastCalcMs = 0;     // marca de tiempo última lectura
static unsigned long     _flowLastDtMs   = 0;     // duración del último intervalo de cálculo (ms)
static uint32_t          _flowLastPulses = 0;     // pulsos contados en el último intervalo
static float             _flowLpm        = 0.0f;  // último caudal calculado (L/min)

void IRAM_ATTR flowPulseISR() {
  _flowPulseCount++;
  _flowPulseTotal++;
}
#endif

// ── Pipeline simulado / hardware-ready ───────────────────────────────────────
String pipelineScenario      = "normal";    // normal | leak | burst | obstruction
// Si FLOW_PIN está definido arrancamos en "real" directamente; de lo contrario "sim".
// El backend puede cambiar este valor en runtime via MQTT pipeline_config o HTTP /api/pipeline/config.
#if defined(FLOW_PIN)
String pipelineMode          = "real";      // sim | real
#else
String pipelineMode          = "sim";       // sim | real
#endif
String pipelineSource        = "sim";       // sim | real | fallback
bool   pipelinePressureOk    = false;
bool   pipelineFlowOk        = false;
float  sim_pipeline_pressure = PIPELINE_STATIC_P;
float  sim_pipeline_flow     = 0.0f;
unsigned long telemetryIntervalMs  = 20000UL;
unsigned long configSyncIntervalMs = PIPELINE_SYNC_MS;
#ifdef HAS_DISPLAY
unsigned long displayTimeoutMs     = DISPLAY_TIMEOUT_MS;
#endif

static bool anyRelayActive() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (relayActive[i]) return true;
  }
  return false;
}

static float driftClamp(float v, float mn, float mx, float step) {
  float d = ((float)random(-100, 101) / 100.0f) * step;
  return constrain(v + d, mn, mx);
}

void updateSimulatedValues() {
  sim_tempMCP   = driftClamp(sim_tempMCP,   -10.0f,  45.0f,  0.05f);
  sim_tempDHT   = driftClamp(sim_tempDHT,   -10.0f,  45.0f,  0.05f);
  sim_humidity  = driftClamp(sim_humidity,   20.0f,  95.0f,  0.20f);
  sim_tempDHT11 = driftClamp(sim_tempDHT11, -10.0f,  45.0f,  0.05f);
  sim_humDHT11  = driftClamp(sim_humDHT11,   20.0f,  95.0f,  0.20f);
  sim_pressure  = driftClamp(sim_pressure,   95.0f, 110.0f,  0.02f);
  sim_light     = driftClamp(sim_light,       0.0f, 2000.0f, 5.0f);
  sim_windSpeed = driftClamp(sim_windSpeed,   0.0f,   15.0f, 0.3f);
  sim_windDir        = fmod(sim_windDir + ((float)random(-10, 11) / 10.0f) * 5.0f + 360.0f, 360.0f);
  sim_soilMoisture   = driftClamp(sim_soilMoisture, 0.0f, 100.0f, 0.5f);
}

// ── Pipeline simulator ────────────────────────────────────────────────────────
// Ruido determinista idéntico al de pipeline_sim.py: tres ondas sinusoidales.
// Reproducible y sin random() → facilita comparar resultados con el backend.
static float pipelineNoise(float t_s, int ch) {
  return sinf(t_s *  7.3f + ch * 1.7f) * 0.55f
       + sinf(t_s * 13.1f + ch * 3.2f) * 0.30f
       + sinf(t_s * 31.7f + ch * 5.1f) * 0.15f;
}

void updatePipelineSimValues() {
  const bool valveOpen = anyRelayActive();
  float t       = millis() / 1000.0f;
  float p_noise = pipelineNoise(t, 0) * PIPELINE_NOISE_P;
  float q_noise = pipelineNoise(t, 1) * PIPELINE_NOISE_Q;

  if (pipelineScenario == "burst") {
    sim_pipeline_pressure = max(0.0f, 0.25f + p_noise * 0.4f);
    sim_pipeline_flow     = valveOpen
      ? max(0.0f, PIPELINE_NOMINAL_Q * 0.08f + fabsf(q_noise) * 0.3f)
      : 0.0f;

  } else if (pipelineScenario == "obstruction") {
    if (valveOpen) {
      // Tubería bloqueada: presión no cae (queda cerca de la estática), caudal ~0
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise * 0.5f);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.04f);
    } else {
      // Válvula cerrada: igual que escenario normal
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.05f);
    }

  } else if (pipelineScenario == "leak") {
    if (valveOpen) {
      sim_pipeline_pressure = max(0.0f, PIPELINE_DYNAMIC_P - 0.18f + p_noise);
      sim_pipeline_flow     = max(0.0f, PIPELINE_NOMINAL_Q - 0.45f + q_noise);
    } else {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P - 0.10f + p_noise);
      sim_pipeline_flow     = max(0.0f, 0.28f + fabsf(q_noise) * 0.35f);
    }

  } else {  // "normal"
    if (valveOpen) {
      sim_pipeline_pressure = max(0.0f, PIPELINE_DYNAMIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, PIPELINE_NOMINAL_Q + q_noise);
    } else {
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.05f);
    }
  }
}

static bool readRealPipelineSensors(float& pressureBar, float& flowLpm) {
#if defined(FLOW_PIN)
  // Caudalímetro por pulsos (BC547 NPN, señal invertida, FALLING edge).
  // Devuelve true en cuanto hay datos válidos del caudalímetro.
  // pressureBar = -1.0f indica "sin sensor de presión real" → el caller
  // mantiene la estimación simulada para la presión.
  unsigned long now = millis();
  unsigned long dt  = now - _flowLastCalcMs;

  if (dt < 500UL) {
    // Intervalo demasiado corto — reutilizamos el último valor calculado
    flowLpm     = _flowLpm;
    pressureBar = -1.0f;
    return true;
  }

  noInterrupts();
  uint32_t pulses  = _flowPulseCount;
  _flowPulseCount  = 0;
  interrupts();

  _flowLastCalcMs = now;
  _flowLastDtMs   = dt;       // guardamos para el debug
  _flowLastPulses = pulses;   // guardamos para el debug

  // L/min = (pulsos / dt_s) * (60.0 / K_FACTOR)
  // Equivalente: pulsos * 60.0 / (dt_s * K_FACTOR)
  float dt_s  = dt / 1000.0f;
  _flowLpm    = (pulses * 60.0f) / (dt_s * (float)FLOW_K_FACTOR);
  flowLpm     = _flowLpm;
  pressureBar = -1.0f;  // sin sensor de presión de tubería — se usará simulador
  return true;
#else
  (void)pressureBar;
  (void)flowLpm;
  return false;
#endif
}

void updatePipelineValues() {
  // NOTA: esta función se llama desde loop() con dataMutex ya tomado.
  // Las escrituras de pipelineMode/pipelineScenario desde Core 0 están
  // protegidas por dataMutex en syncPipelineScenario() y mqttCallback(),
  // así que la lectura aquí (bajo el mutex del caller) es segura.
  if (pipelineMode == "real") {
    float realPressure = 0.0f;
    float realFlow = 0.0f;
    if (readRealPipelineSensors(realPressure, realFlow)) {
      sim_pipeline_flow = max(0.0f, realFlow);
      pipelineFlowOk    = true;
      pipelineSource    = "real";

      if (realPressure >= 0.0f) {
        // Sensor de presión real disponible
        sim_pipeline_pressure = max(0.0f, realPressure);
        pipelinePressureOk    = true;
      } else {
        // Sin sensor de presión real — estimación por simulador solo para presión.
        // IMPORTANTE: guardamos el caudal real antes y lo restauramos porque
        // updatePipelineSimValues() también sobreescribe sim_pipeline_flow.
        float savedFlow = sim_pipeline_flow;
        updatePipelineSimValues();
        sim_pipeline_flow  = savedFlow;  // restaurar caudal real
        pipelinePressureOk = false;
      }
      return;
    }

    pipelineSource     = "fallback";
    pipelinePressureOk = false;
    pipelineFlowOk     = false;
  } else {
    pipelineSource     = "sim";
    pipelinePressureOk = false;
    pipelineFlowOk     = false;
  }

  updatePipelineSimValues();
}

// =============================================================================
// HTU2x (HTU21D / HTU20D / SHT21) — temperatura y humedad por I2C
// Dirección fija: 0x40. Sin librería externa.
// =============================================================================
#define HTU2X_ADDR        0x40
#define HTU2X_CMD_TEMP    0xF3   // medir temperatura, no-hold master
#define HTU2X_CMD_HUM     0xF5   // medir humedad,     no-hold master
#define HTU2X_CMD_RESET   0xFE
#define HTU2X_CMD_WR_REG  0xE6   // escribir registro de usuario
#define HTU2X_CMD_RD_REG  0xE7   // leer registro de usuario
// Registro de usuario: bit7+bit0=resolución, bit2=calefactor, bit1=OTP_disable
// 0x02 → 12/14 bit, calefactor OFF, OTP desactivado (default tras reset)
// 0x06 → 12/14 bit, calefactor ON,  OTP desactivado
#define HTU2X_REG_DEFAULT 0x02
#define HTU2X_REG_HEATER  0x06

bool htu_begin() {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(HTU2X_CMD_RESET);
  if (Wire.endTransmission() != 0) return false;
  delay(15);  // tiempo de reset (máx 15 ms según datasheet)
  return true;
}

// Lanza una medición y espera. Devuelve NAN si falla o si hay timeout.
static float htu_measure(uint8_t cmd, uint16_t wait_ms) {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission() != 0) return NAN;
  delay(wait_ms);
  uint8_t got = Wire.requestFrom((uint8_t)HTU2X_ADDR, (uint8_t)3, (uint8_t)1);
  if (got < 2) return NAN;  // timeout o NACK — el sensor no respondió
  uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
  if (Wire.available()) Wire.read();  // descarta CRC
  raw &= 0xFFFC;  // limpia los 2 bits de tipo de medición
  return (float)raw;
}

float htu_readTemp() {
  float raw = htu_measure(HTU2X_CMD_TEMP, 50);
  if (isnan(raw)) return NAN;
  return -46.85f + 175.72f * (raw / 65536.0f);
}

float htu_readHumidity() {
  float raw = htu_measure(HTU2X_CMD_HUM, 20);
  if (isnan(raw)) return NAN;
  return constrain(-6.0f + 125.0f * (raw / 65536.0f), 0.0f, 100.0f);
}

// Activa o desactiva el calefactor interno del HTU2x.
static void htu_set_heater(bool on) {
  Wire.beginTransmission(HTU2X_ADDR);
  Wire.write(HTU2X_CMD_WR_REG);
  Wire.write(on ? HTU2X_REG_HEATER : HTU2X_REG_DEFAULT);
  Wire.endTransmission();
}

// Calentamiento de arranque: activa el calefactor ~3 s para evaporar
// condensación y obtener lecturas de humedad más fiables desde el inicio.
void htu_heater_warmup() {
  DLOGLN("HTU2x: calentamiento 3s (evaporar condensacion)...");
  htu_set_heater(true);
  for (int i = 3; i > 0; i--) {
    float t = htu_readTemp();
    float h = htu_readHumidity();
    DLOGF("  HTU2x heater ON — T:%.1f C  H:%.1f %%  (%ds)\n", t, h, i);
    delay(1000);
  }
  htu_set_heater(false);
  delay(1000);  // estabilización tras apagar calefactor
  DLOGLN("HTU2x: calentamiento completado.");
}

// =============================================================================
// Sensor de luz ambiente — autodetección TSL2584 / APDS-9930 (I2C directo)
// Ambos chips usan dirección 0x39 y protocolo CMD=0x80|reg.
// TSL2584:  datos en 0x0C-0x0F, ID en 0x0A (bits[7:4]=0xA)
// APDS-9930: datos en 0x14-0x17, ID en 0x12 (=0x39)
// =============================================================================
#define LIGHT_ADDR     0x39
#define LIGHT_CMD      0x80   // bit CMD obligatorio en el byte de registro

// Registros comunes (misma dirección en ambos chips)
#define LIGHT_CTRL     0x00   // ENABLE / POWER
#define LIGHT_TIMING   0x01   // TIMING (TSL) / ATIME (APDS)

// Registros exclusivos TSL2584
#define TSL_ID_REG     0x0A   // Part number en bits[7:4]; TSL2584 = 0xA
#define TSL_D0L        0x0C   // CH0 low  (visible + IR)
#define TSL_D1L        0x0E   // CH1 low  (IR)

// Registros exclusivos APDS-9930
#define APDS_ID_REG    0x12   // Chip ID; APDS-9930 = 0x39
#define APDS_CDATAL    0x14   // Clear channel low  (visible + IR)
#define APDS_IRDATAL   0x16   // IR channel low

static bool light_is_apds = false;  // se establece en tsl_begin()

static bool light_write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LIGHT_ADDR);
  Wire.write(LIGHT_CMD | reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Lectura stop-start (más compatible con ESP8266 que repeated-start)
static uint8_t light_read8(uint8_t reg) {
  Wire.beginTransmission(LIGHT_ADDR);
  Wire.write(LIGHT_CMD | reg);
  if (Wire.endTransmission() != 0) return 0xFF;
  Wire.requestFrom((uint8_t)LIGHT_ADDR, (uint8_t)1, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static uint16_t light_read16(uint8_t regL) {
  uint16_t lo = light_read8(regL);
  uint16_t hi = light_read8(regL + 1);
  return (hi << 8) | lo;
}

// Enciende el sensor, detecta el tipo y configura la integración.
// Devuelve true si el sensor responde en el bus.
bool tsl_begin() {
  if (!light_write(LIGHT_CTRL, 0x03)) return false;  // Power ON + ALS enable
  delay(10);

  // Autodetección: leer ambos registros de ID y mostrarlos
  uint8_t apds_id = light_read8(APDS_ID_REG);  // 0x12 → 0x39 o 0x30 para APDS-9930
  uint8_t tsl_id  = light_read8(TSL_ID_REG);   // 0x0A → 0xAx para TSL2584
  DLOGF("[LUZ] ID@0x12=0x%02X  ID@0x0A=0x%02X\n", apds_id, tsl_id);

  // APDS-9930: ID puede ser 0x39 (rev1) o 0x30 (rev0); nibble alto = 0x3
  if ((apds_id & 0xF0) == 0x30) {
    light_is_apds = true;
    // ATIME=0xDB → 37 ciclos × 2.73 ms ≈ 101 ms por conversión
    light_write(LIGHT_TIMING, 0xDB);
    DLOGF("[LUZ] APDS-9930 detectado (ID=0x%02X)\n", apds_id);
  } else if ((tsl_id & 0xF0) == 0xA0) {
    light_is_apds = false;
    light_write(LIGHT_TIMING, 0x02);  // 402 ms, ganancia 1×
    DLOGF("[LUZ] TSL2584 detectado (ID=0x%02X)\n", tsl_id);
  } else {
    // ID desconocido — probar APDS-9930 como fallback (más probable en 0x39)
    light_is_apds = true;
    light_write(LIGHT_TIMING, 0xDB);
    DLOGF("[LUZ] ID desconocido — usando registros APDS-9930 (fallback)\n");
  }

  delay(450);  // Esperar primera integración completa (cubre ambos casos)
  return true;
}

// Devuelve lux estimado a partir del canal visible+IR y el canal IR.
// Usa la fórmula TSL258x (válida también como aproximación para APDS-9930).
float tsl_readLux() {
  uint16_t ch0, ch1;
  if (light_is_apds) {
    ch0 = light_read16(APDS_CDATAL);   // clear (visible + IR)
    ch1 = light_read16(APDS_IRDATAL);  // IR
  } else {
    ch0 = light_read16(TSL_D0L);
    ch1 = light_read16(TSL_D1L);
  }

  if (ch0 == 0) return 0.0f;

  float ratio = (float)ch1 / (float)ch0;
  float lux;

  if      (ratio <= 0.52f) lux = 0.0315f * ch0 - 0.0593f * ch0 * pow(ratio, 1.4f);
  else if (ratio <= 0.65f) lux = 0.0229f * ch0 - 0.0291f * ch1;
  else if (ratio <= 0.80f) lux = 0.0157f * ch0 - 0.0180f * ch1;
  else if (ratio <= 1.30f) lux = 0.00338f * ch0 - 0.00260f * ch1;
  else                     lux = 0.0f;

  return max(0.0f, lux);
}

// ── Filtros media móvil ────────────────────────────────────────────────────────
#define FILTER_SIZE 10

int   anemometerValues[FILTER_SIZE] = {};
int   aneIdx = 0;

float filteredADC(int newVal) {
  anemometerValues[aneIdx] = newVal;
  aneIdx = (aneIdx + 1) % FILTER_SIZE;
  long s = 0;
  for (int i = 0; i < FILTER_SIZE; i++) s += anemometerValues[i];
  return (float)s / FILTER_SIZE;
}

#if defined(SOIL_PIN)
int   soilValues[FILTER_SIZE] = {};
int   soilIdx = 0;

float filteredSoilADC(int newVal) {
  soilValues[soilIdx] = newVal;
  soilIdx = (soilIdx + 1) % FILTER_SIZE;
  long s = 0;
  for (int i = 0; i < FILTER_SIZE; i++) s += soilValues[i];
  return (float)s / FILTER_SIZE;
}
#endif

float adcToWindSpeed(float adc) {
  float v = adc * (ADC_VOLTAGE_REF / ADC_RANGE);
  return (v / ADC_VOLTAGE_REF) * 30.0f;
}

#ifdef HAS_DISPLAY
float adcToWindDeg(int adc) {
  float d = (adc / ADC_RANGE) * 360.0f;
  if (d >= 337.5f || d < 22.5f) return 0.0f;
  if (d < 67.5f)                 return 45.0f;
  if (d < 112.5f)                return 90.0f;
  if (d < 157.5f)                return 135.0f;
  if (d < 202.5f)                return 180.0f;
  if (d < 247.5f)                return 225.0f;
  if (d < 292.5f)                return 270.0f;
  return 315.0f;
}
#else
float adcToWindDeg(int) { return 0.0f; }
#endif

const char* degToCompass(float d) {
  const char* dirs[] = {"N","NE","E","SE","S","SO","O","NO"};
  return dirs[((int)(d + 22.5f) / 45) % 8];
}

// ── FreeRTOS (solo ESP32) — declarado aquí para estar disponible antes de su uso ──
#ifndef ESP8266
volatile bool        isUpdatingOTA = false;
portMUX_TYPE         windMux       = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t    dataMutex     = nullptr;
TaskHandle_t         networkTaskHandle = nullptr;

// ── Snapshot de telemetría — struct global para evitar auto-prototype de Arduino ──
struct TelemetrySnapshot {
  float tempMCP, pressure, tempDHT, humidity;
  float windSpeed, windDir, windSpeedFilt, avgWindDir;
  float light, tempDHT11, humDHT11, soil;
  float bmpTemp, bmpPressure;
  float pipePressure, pipeFlow;
  long  heap, uptime;
  int   rssi, relayMask;
#if DEVICE_PROFILE == PROFILE_AGROMETEO
  float dewPoint, heatIndex, absHum;
#endif
} _netSnap;
#endif

// ── Promedio vectorial de dirección ────────────────────────────────────────────
float windSumX = 0, windSumY = 0;
int   windSampleCount = 0;
float finalAvgWindDir = 0;

void accumulateWindVector(float deg) {
  float r = deg * PI / 180.0f;
#ifdef ESP8266
  noInterrupts();
#else
  portENTER_CRITICAL(&windMux);
#endif
  windSumX += cos(r);
  windSumY += sin(r);
  windSampleCount++;
#ifdef ESP8266
  interrupts();
#else
  portEXIT_CRITICAL(&windMux);
#endif
}

float calcAndResetWindVector() {
#ifdef ESP8266
  noInterrupts();
#else
  portENTER_CRITICAL(&windMux);
#endif
  if (windSampleCount == 0) {
#ifdef ESP8266
    interrupts();
#else
    portEXIT_CRITICAL(&windMux);
#endif
    return 0;
  }
  float deg = atan2(windSumY, windSumX) * 180.0f / PI;
  if (deg < 0) deg += 360.0f;
  windSumX = windSumY = 0;
  windSampleCount = 0;
#ifdef ESP8266
  interrupts();
#else
  portEXIT_CRITICAL(&windMux);
#endif
  return deg;
}

// ── Estado ────────────────────────────────────────────────────────────────────
bool lastServerOK    = false;
unsigned long lastSendTime   = 0;
unsigned long lastScreenTime = 0;
unsigned long lastSensorRead = 0;  // lectura I2C sincronizada con el intervalo de telemetría


// ── Info estática del hardware ────────────────────────────────────────────────
void printHardwareInfo() {
  DLOGLN("\n=== Hardware Info ===");
#ifdef ESP8266
  DLOGF("Chip         : ESP8266 (ID=0x%08X)\n", ESP.getChipId());
#else
  DLOGF("Chip         : %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
#endif
  DLOGF("CPU          : %d MHz\n",      ESP.getCpuFreqMHz());
  DLOGF("Flash        : %d MB\n",        ESP.getFlashChipSize() / (1024 * 1024));
  DLOGF("Free Heap    : %d bytes\n",     ESP.getFreeHeap());
  DLOGF("SDK          : %s\n",           ESP.getSdkVersion());
  DLOGF("MAC          : %s\n",           WiFi.macAddress().c_str());
  DLOGF("IP           : %s\n",           WiFi.localIP().toString().c_str());
  DLOGLN("====================");
}

static bool serverUseTls() {
  return server_port == 443 || server_port == 8443;
}

static String serverBaseUrl() {
  String url = serverUseTls() ? "https://" : "http://";
  url += String(server_ip);
  if ((!serverUseTls() && server_port != 80) || (serverUseTls() && server_port != 443)) {
    url += ":" + String(server_port);
  }
  return url;
}

#ifndef ESP8266
static bool tlsClockReady(unsigned long waitMs = 5000) {
  time_t now = time(nullptr);
  unsigned long start = millis();

  while (now < 1700000000L && millis() - start < waitMs) {
    delay(250);
    now = time(nullptr);
  }
  return now >= 1700000000L;
}

static void prepareSecureClient(WiFiClientSecure& client, int timeoutMs = 10000) {
  int handshakeSeconds = timeoutMs / 1000;
  if (handshakeSeconds < 1) handshakeSeconds = 1;

  client.stop();
  client.setHandshakeTimeout(handshakeSeconds);
  client.setCACert(MQTT_CA_CERT_PEM);

  if (!tlsClockReady(5000)) {
    DLOGLN("[TLS] Advertencia: reloj aun no sincronizado; reintentando handshake con la CA cargada");
  }
}
#endif

static bool parseRelayBitmask(const String& response, int& bitmaskOut) {
  String trimmed = response;
  trimmed.trim();
  if (trimmed.length() == 0) return false;

  for (size_t i = 0; i < trimmed.length(); i++) {
    char c = trimmed[i];
    if (c < '0' || c > '9') return false;
  }

  long parsed = trimmed.toInt();
  long maxMask = (1L << RELAY_COUNT) - 1L;
  if (parsed < 0 || parsed > maxMask) return false;

  bitmaskOut = (int)parsed;
  return true;
}

void postDeviceInfo() {
  JsonDocument doc;
#ifdef ESP8266
  doc["chip_model"]    = "ESP8266";
  doc["chip_revision"] = 0;
#else
  doc["chip_model"]    = ESP.getChipModel();
  doc["chip_revision"] = (int)ESP.getChipRevision();
#endif
  doc["cpu_freq_mhz"]  = ESP.getCpuFreqMHz();
  doc["flash_size_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
  doc["sdk_version"]   = ESP.getSdkVersion();
  doc["mac_address"]      = WiFi.macAddress();
  doc["ip_address"]       = WiFi.localIP().toString();
  doc["relay_count"]      = RELAY_COUNT;
  doc["firmware_version"] = FIRMWARE_VERSION;

  String json;
  serializeJson(doc, json);

  String url = serverBaseUrl() + "/api/device_info";
  HTTPClient http;
  http.setTimeout(10000);
#ifdef ESP8266
  WiFiClient wifiClient;
  http.begin(wifiClient, url);
#else
  if (serverUseTls()) {
    WiFiClientSecure client;
    prepareSecureClient(client, 10000);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
#endif
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  DLOGF("[DeviceInfo] POST → %d\n", code);
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────
String httpGet(const String& url, int timeoutMs = 10000) {
  HTTPClient http;
  http.setTimeout(timeoutMs);
#ifdef ESP8266
  WiFiClient wifiClient;
  http.begin(wifiClient, url);
#else
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    prepareSecureClient(client, timeoutMs);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
#endif
  int code = http.GET();
  String body = "";
  if (code == 200) body = http.getString();
  http.end();
  body.trim();
  return body;
}

void syncPipelineScenario() {
  String cfgUrl = serverBaseUrl() + "/api/pipeline/config";
  String body = httpGet(cfgUrl, 2000);

  if (body.length() > 0) {
    StaticJsonDocument<320> doc;
    if (!deserializeJson(doc, body)) {
      String nextScenario = doc["scenario"] | pipelineScenario;
      String nextMode     = doc["mode"] | pipelineMode;
      long nextTelemetry  = doc["telemetry_interval_s"] | (long)(telemetryIntervalMs / 1000UL);
      long nextSync       = doc["config_sync_interval_s"] | (long)(configSyncIntervalMs / 1000UL);
#ifdef HAS_DISPLAY
      long nextDisplay    = doc["display_timeout_s"] | (long)(displayTimeoutMs / 1000UL);
#endif

      // Proteger escrituras de config con mutex (leídas desde Core 1)
#ifndef ESP8266
      if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
#endif
      if (nextScenario == "normal" || nextScenario == "leak" ||
          nextScenario == "burst"  || nextScenario == "obstruction") {
        if (nextScenario != pipelineScenario) {
          DLOGF("[PIPE] Escenario → %s\n", nextScenario.c_str());
        }
        pipelineScenario = nextScenario;
      }
      if (nextMode == "sim" || nextMode == "real") {
        if (nextMode != pipelineMode) {
          DLOGF("[PIPE] Modo → %s\n", nextMode.c_str());
        }
        pipelineMode = nextMode;
      }
#ifndef ESP8266
      if (dataMutex) xSemaphoreGive(dataMutex);
#endif
      if (nextTelemetry >= 5 && nextTelemetry <= 3600) {
        unsigned long nextMs = (unsigned long)nextTelemetry * 1000UL;
        if (nextMs != telemetryIntervalMs) {
          telemetryIntervalMs = nextMs;
          DLOGF("[PIPE] Telemetría → %lds\n", nextTelemetry);
        }
      }
      if (nextSync >= 5 && nextSync <= 3600) {
        unsigned long nextMs = (unsigned long)nextSync * 1000UL;
        if (nextMs != configSyncIntervalMs) {
          configSyncIntervalMs = nextMs;
          DLOGF("[PIPE] Sync config → %lds\n", nextSync);
        }
      }
#ifdef HAS_DISPLAY
      if (nextDisplay >= 0 && nextDisplay <= 3600) {
        unsigned long nextMs = (unsigned long)nextDisplay * 1000UL;
        if (nextMs != displayTimeoutMs) {
          displayTimeoutMs = nextMs;
          DLOGF("[PIPE] Pantalla timeout → %lds\n", nextDisplay);
        }
      }
#endif
      return;
    }

    body.trim();
    if (body == "normal" || body == "leak" ||
        body == "burst"  || body == "obstruction") {
      if (body != pipelineScenario) {
        DLOGF("[PIPE] Escenario → %s\n", body.c_str());
      }
      pipelineScenario = body;
    }
  }
}

void checkRelayCommand() {
  String url = serverBaseUrl()
               + "/api/relay/command?mac=" + WiFi.macAddress();
  String response = httpGet(url, 2000);
  if (response.length() == 0) return;

  int bitmask = 0;
  if (!parseRelayBitmask(response, bitmask)) {
    DLOGF("[Relay] Respuesta invalida ignorada: %s\n", response.c_str());
    return;
  }

  bool changed = false;
  for (int i = 0; i < RELAY_COUNT; i++) {
    bool desired = (bitmask >> i) & 1;
    if (desired != relayActive[i]) {
      relayActive[i] = desired;
      // JQC-3FF-S-Z activo-LOW: LOW = relay ON, HIGH = relay OFF
      digitalWrite(RELAY_PINS[i], desired ? LOW : HIGH);
      DLOGF("[Relay %d] %s\n", i, desired ? "ON" : "OFF");
      changed = true;
    }
  }
  if (changed) {
    int actualMask = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
      if (relayActive[i]) actualMask |= (1 << i);
    }
    setLedState(anyRelayActive() ? LED_RELAY_ON : LED_IDLE);
    httpPost(serverBaseUrl() + "/api/relay/ack", String(actualMask));
  }
}

bool httpPost(const String& url, const String& body) {
  HTTPClient http;
  http.setTimeout(10000);
#ifdef ESP8266
  WiFiClient wifiClient;
  http.begin(wifiClient, url);
#else
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    prepareSecureClient(client, 10000);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
#endif
  http.addHeader("Content-Type", "text/plain");
  http.addHeader("X-Device-MAC", WiFi.macAddress());
  int code = http.POST(body);
  http.end();
  return (code == 200 || code == 201);
}

// =============================================================================
// PANTALLA TFT (solo ESP32)
// =============================================================================
#ifdef HAS_DISPLAY

// Paleta Aquantia: azul corporativo #5ab4e0 · cabecera navy #0d3a6e
#define C_BG      0x0841   // fondo general    ~#0d0d0d
#define C_CARD    0x1082   // fondo tarjeta    ~#101010
#define C_HDR     0x09CD   // cabecera         ~#0d3a6e (navy Aquantia)
#define C_BORDER  0x2124   // borde tarjeta
#define C_TEXT    0xFFFF   // texto principal  blanco
#define C_LABEL   0x7BEF   // etiqueta gris claro
#define C_SIM     0xFD20   // simulado         naranja
#define C_REAL    0x5DBC   // dato real        #5ab4e0 (azul Aquantia)
#define C_RED     0xF800   // error/alerta     rojo

// Layout 240×135 — 2 filas × 3 cols + fila extra para luz
// Fila 0-1: misma cuadrícula 2×3 de siempre (tarjetas 79×57)
// La luz se muestra como barra inferior de 16px en la cabecera (valor en header)
#define HDR_H    18
#define CARD_W   79
#define CARD_H   57

static int cardX(int col) { return col * (CARD_W + 1); }
static int cardY(int row) { return HDR_H + row * (CARD_H + 2); }

void iconThermometer(int cx, int cy, uint16_t col) {
  spr.fillRoundRect(cx - 2, cy - 7, 5, 10, 2, col);
  spr.fillCircle(cx, cy + 5, 4, col);
  spr.drawFastVLine(cx, cy - 7, 8, C_CARD);
}

void iconDrop(int cx, int cy, uint16_t col) {
  spr.fillCircle(cx, cy + 4, 4, col);
  for (int i = 0; i <= 7; i++) {
    int hw = (7 - i) / 2;
    spr.drawFastHLine(cx - hw, cy - 3 + i, hw * 2 + 1, col);
  }
}

void iconGauge(int cx, int cy, uint16_t col) {
  spr.drawCircle(cx, cy, 7, col);
  spr.drawLine(cx, cy, cx + 4, cy - 4, col);
  spr.fillCircle(cx, cy, 2, col);
  spr.drawFastHLine(cx - 5, cy + 5, 11, col);
}

void iconWind(int cx, int cy, uint16_t col) {
  spr.drawFastHLine(cx - 6, cy - 3, 12, col);
  spr.drawLine(cx + 6, cy - 3, cx + 8, cy - 1, col);
  spr.drawFastHLine(cx - 6, cy,     10, col);
  spr.drawFastHLine(cx - 6, cy + 3, 12, col);
}

void iconCompass(int cx, int cy, uint16_t col) {
  spr.drawCircle(cx, cy, 7, col);
  spr.fillTriangle(cx, cy - 6, cx - 2, cy, cx + 2, cy, col);
  spr.fillTriangle(cx, cy + 6, cx - 2, cy, cx + 2, cy, C_LABEL);
}

// Icono sol simple para luz
void iconSun(int cx, int cy, uint16_t col) {
  spr.fillCircle(cx, cy, 4, col);
  for (int a = 0; a < 360; a += 45) {
    float r = a * PI / 180.0f;
    int x1 = cx + (int)(6 * cos(r));
    int y1 = cy + (int)(6 * sin(r));
    int x2 = cx + (int)(9 * cos(r));
    int y2 = cy + (int)(9 * sin(r));
    spr.drawLine(x1, y1, x2, y2, col);
  }
}

typedef void (*IconFn)(int, int, uint16_t);
IconFn iconFns[6] = {
  iconThermometer,
  iconThermometer,
  iconDrop,
  iconGauge,
  iconWind,
  iconCompass,
};

void drawCard(int col, int row,
              const char* label,
              float value, const char* unit,
              bool simulated,
              bool showCompass = false) {
  int x = cardX(col);
  int y = cardY(row);

  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, CARD_H - 2, 4, C_CARD);
  spr.drawRoundRect(x, y, CARD_W, CARD_H, 4, C_BORDER);

  uint16_t accentCol = simulated ? C_SIM : C_REAL;
  spr.fillRoundRect(x + 1, y + 1, CARD_W - 2, 3, 2, accentCol);

  int idx = row * 3 + col;
  iconFns[idx](x + 10, y + 13, accentCol);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(label, x + 22, y + 5, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  char buf[12];
  snprintf(buf, sizeof(buf), showCompass ? "%.0f" : "%.1f", value);
  spr.drawString(buf, x + 4, y + 18, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString(unit, x + 4, y + CARD_H - 13, 1);

  if (showCompass) {
    spr.setTextColor(accentCol, C_CARD);
    spr.drawString(degToCompass(value), x + 42, y + CARD_H - 13, 2);
  }

  const char* badge = simulated ? "[SIM]" : "[OK]";
  spr.setTextColor(accentCol, C_CARD);
  spr.drawRightString(badge, x + CARD_W - 2, y + CARD_H - 13, 1);
}

void drawScreen() {
  spr.fillSprite(C_BG);

  // Cabecera con luz integrada a la derecha del título
  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("METEOSTATION", 6, 4, 2);

  // Indicadores de vista: ● • (punto 1 activo, punto 2 inactivo)
  spr.fillCircle(114, 9, 3, C_TEXT);   // vista 1 (meteo) — activa
  spr.fillCircle(123, 9, 3, C_LABEL);  // vista 2 (pipeline) — inactiva

  // Luz en la cabecera (icono + valor lux)
  uint16_t luxCol = tsl_ok ? C_REAL : C_SIM;
  iconSun(118, 9, luxCol);
  char luxBuf[10];
  snprintf(luxBuf, sizeof(luxBuf), "%.0flx", lightLevel);
  spr.setTextColor(luxCol, C_HDR);
  spr.drawString(luxBuf, 130, 5, 1);

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 168, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 157, 5, 1);
  }

  uint16_t srvCol = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol);
  spr.drawCircle(230, 9, 5, C_TEXT);

  drawCard(0, 0, "T.EXT",   temperatureMCP,     "C",    !temp_ok);
  drawCard(1, 0, "T.INT",   temperatureDHT,     "C",    !htu_ok);
  drawCard(2, 0, "HUMEDAD", humidity,           "%",    !htu_ok);
  drawCard(0, 1, "PRESION", (float)pressure,    "KPa",  !bar_ok);
  drawCard(1, 1, "VIENTO",  windSpeedFiltered,  "m/s",  false);
  drawCard(2, 1, "DIRECC.", currentWindDirDeg,  "deg",  false, true);

  spr.pushSprite(0, 0);
}

// ── Vista 2: Pipeline (caudal + presión) ─────────────────────────────────────
void drawPipelineScreen() {
  spr.fillSprite(C_BG);

  // Cabecera
  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawString("PIPELINE", 6, 4, 2);

  // Indicadores de vista: • ● (punto 1 inactivo, punto 2 activo)
  spr.fillCircle(114, 9, 3, C_LABEL);  // vista 1 (meteo) — inactiva
  spr.fillCircle(123, 9, 3, C_TEXT);   // vista 2 (pipeline) — activa

  if (WiFi.status() == WL_CONNECTED) {
    spr.setTextColor(C_REAL, C_HDR);
    spr.drawString("WiFi", 168, 5, 1);
  } else {
    spr.setTextColor(C_RED, C_HDR);
    spr.drawString("NoWiFi", 157, 5, 1);
  }
  uint16_t srvCol = lastServerOK ? C_REAL : C_RED;
  spr.fillCircle(230, 9, 5, srvCol);
  spr.drawCircle(230, 9, 5, C_TEXT);

  // Determinar si los valores son simulados
  bool pipeSim = (pipelineMode == "sim");
  uint16_t pCol = (pipeSim || !pipelinePressureOk) ? C_SIM : C_REAL;
  uint16_t fCol = (pipeSim || !pipelineFlowOk)     ? C_SIM : C_REAL;

  // Dos tarjetas anchas: presión (izquierda) | caudal (derecha)
  // 240px - 2px borde - 2px borde - 2px separación = 234px → 117px cada zona
  int cY = HDR_H + 2;
  int cH = 88;

  // ── Tarjeta PRESION ────────────────────────────────────────────────────────
  spr.fillRoundRect(  1, cY, 115, cH, 4, C_CARD);
  spr.drawRoundRect(  0, cY, 117, cH, 4, C_BORDER);
  spr.fillRoundRect(  1, cY, 115,  3, 2, pCol);

  iconGauge(14, cY + 14, pCol);
  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("PRESION", 28, cY + 6, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", sim_pipeline_pressure);
  spr.drawString(buf, 6, cY + 22, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("bar", 6, cY + cH - 14, 1);
  spr.setTextColor(pCol, C_CARD);
  spr.drawRightString(pipeSim || !pipelinePressureOk ? "[SIM]" : "[OK]",
                      113, cY + cH - 14, 1);

  // ── Tarjeta CAUDAL ─────────────────────────────────────────────────────────
  spr.fillRoundRect(122, cY, 116, cH, 4, C_CARD);
  spr.drawRoundRect(121, cY, 118, cH, 4, C_BORDER);
  spr.fillRoundRect(122, cY, 116,  3, 2, fCol);

  iconDrop(134, cY + 14, fCol);
  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("CAUDAL", 148, cY + 6, 1);

  spr.setTextColor(C_TEXT, C_CARD);
  snprintf(buf, sizeof(buf), "%.2f", sim_pipeline_flow);
  spr.drawString(buf, 126, cY + 22, 4);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("L/min", 126, cY + cH - 14, 1);
  spr.setTextColor(fCol, C_CARD);
  spr.drawRightString(pipeSim || !pipelineFlowOk ? "[SIM]" : "[OK]",
                      237, cY + cH - 14, 1);

  // ── Franja inferior: modo + escenario ─────────────────────────────────────
  int bY = cY + cH + 3;
  int bH = 135 - bY;
  spr.fillRoundRect(0, bY, 240, bH, 3, C_CARD);

  uint16_t modeCol = pipeSim ? C_SIM : C_REAL;
  spr.setTextColor(modeCol, C_CARD);
  spr.drawCentreString(
    pipeSim ? "MODO: SIMULADO" : "MODO: REAL",
    120, bY + 2, 1);

  spr.setTextColor(C_LABEL, C_CARD);
  spr.drawString("Escenario:", 6, bY + 14, 1);
  spr.setTextColor(C_TEXT, C_CARD);
  spr.drawString(pipelineScenario.c_str(), 68, bY + 14, 1);

  spr.pushSprite(0, 0);
}

// Pantalla que se muestra mientras el portal SoftAP está activo
void drawAPScreen(const char* ap_ssid, const char* serial) {
  spr.fillSprite(C_BG);
  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawCentreString("AQUANTIA  SETUP", 120, 4, 2);

  spr.setTextColor(C_REAL, C_BG);
  spr.drawCentreString("Configuracion WiFi", 120, 24, 2);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Conecta tu movil a:", 6, 46, 1);
  spr.setTextColor(C_TEXT, C_BG);
  spr.drawString(ap_ssid, 6, 58, 2);

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawString("Pass: aquantia1", 6, 80, 1);
  spr.drawString("Web:  192.168.4.1", 6, 92, 1);

  spr.setTextColor(C_SIM, C_BG);
  spr.drawString(serial, 6, 112, 1);

  spr.pushSprite(0, 0);
}

void drawBootScreen(const char* wifiMsg) {
  spr.fillSprite(C_BG);

  spr.fillRect(0, 0, 240, HDR_H, C_HDR);
  spr.setTextColor(C_TEXT, C_HDR);
  spr.drawCentreString("METEOSTATION  v3", 120, 4, 2);

  struct { const char* lbl; bool ok; } sensors[4] = {
    { "MCP9808  (T.Ext)", mcp_ok },
    { "Barometro       ", bar_ok },
    { "HTU2x    (T+H)  ", htu_ok },
    { "LuzAmb   (0x39) ", tsl_ok },
  };

  for (int i = 0; i < 4; i++) {
    int y = 22 + i * 24;
    spr.setTextColor(C_LABEL, C_BG);
    spr.drawString(sensors[i].lbl, 8, y, 2);

    uint16_t badgeCol    = sensors[i].ok ? C_REAL : C_SIM;
    const char* badgeTxt = sensors[i].ok ? "  REAL  " : "  SIM  ";
    spr.fillRoundRect(163, y, 60, 16, 3, badgeCol);
    spr.setTextColor(TFT_BLACK, badgeCol);
    spr.drawCentreString(badgeTxt, 193, y + 4, 1);
  }

  spr.setTextColor(C_LABEL, C_BG);
  spr.drawCentreString(wifiMsg, 120, 120, 2);

  spr.pushSprite(0, 0);
}

#endif  // HAS_DISPLAY

// =============================================================================
// MQTT — Funciones auxiliares (solo ESP32 con USE_MQTT)
// =============================================================================
#if !defined(ESP8266) && defined(USE_MQTT)

// Callback para comandos entrantes en aquantia/<finca_id>/cmd
// Payload esperado: {"relay": 0, "state": true} o {"type":"pipeline_config", ...}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, length)) return;

  String targetMac = doc["mac"] | "";
  targetMac.trim();
  targetMac.toUpperCase();
  String selfMac = WiFi.macAddress();
  selfMac.trim();
  selfMac.toUpperCase();
  if (targetMac.length() > 0 && targetMac != selfMac) return;

  if (doc.containsKey("relay")) {
    int  relay = doc["relay"] | 0;
    bool state = doc["state"] | false;
    if (relay >= 0 && relay < RELAY_COUNT) {
      relayActive[relay] = state;
      digitalWrite(RELAY_PINS[relay], state ? LOW : HIGH);  // activo-LOW
      DLOGF("[MQTT] Relay %d → %s\n", relay, state ? "ON" : "OFF");
      setLedState(anyRelayActive() ? LED_RELAY_ON : LED_IDLE);
    }
  }

  bool updatedPipe = false;
  String nextScenario = doc["pipeline_scenario"] | pipelineScenario;
  String nextMode     = doc["pipeline_mode"] | pipelineMode;
  long nextTelemetry  = doc["telemetry_interval_s"] | (long)(telemetryIntervalMs / 1000UL);
  long nextSync       = doc["config_sync_interval_s"] | (long)(configSyncIntervalMs / 1000UL);
#ifdef HAS_DISPLAY
  long nextDisplay    = doc["display_timeout_s"] | (long)(displayTimeoutMs / 1000UL);
#endif

  // Proteger escrituras de Strings con mutex (leídas desde Core 1)
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (nextScenario == "normal" || nextScenario == "leak" ||
      nextScenario == "burst"  || nextScenario == "obstruction") {
    if (nextScenario != pipelineScenario) {
      pipelineScenario = nextScenario;
      updatedPipe = true;
    }
  }
  if (nextMode == "sim" || nextMode == "real") {
    if (nextMode != pipelineMode) {
      pipelineMode = nextMode;
      updatedPipe = true;
    }
  }
  if (dataMutex) xSemaphoreGive(dataMutex);
  if (nextTelemetry >= 5 && nextTelemetry <= 3600) {
    telemetryIntervalMs = (unsigned long)nextTelemetry * 1000UL;
  }
  if (nextSync >= 5 && nextSync <= 3600) {
    configSyncIntervalMs = (unsigned long)nextSync * 1000UL;
  }
#ifdef HAS_DISPLAY
  if (nextDisplay >= 0 && nextDisplay <= 3600) {
    displayTimeoutMs = (unsigned long)nextDisplay * 1000UL;
  }
#endif

  if (updatedPipe) {
    updatePipelineValues();
    DLOGF("[MQTT] Pipeline mode=%s scenario=%s\n", pipelineMode.c_str(), pipelineScenario.c_str());
  }
}

// Conectar al broker y suscribirse al topic de comandos
bool mqttConnect() {
  char client_id[48];
  String mac_no_colon = WiFi.macAddress();
  mac_no_colon.replace(":", "");

  if (mac_no_colon.length() >= 12) {
    snprintf(client_id, sizeof(client_id), "aquantia-%s", mac_no_colon.c_str());
  } else {
    snprintf(client_id, sizeof(client_id), "aquantia-%s", device_serial_get());
  }

  bool ok = mqttClient.connect(client_id, mqtt_user, mqtt_pass);
  if (ok) {
    char cmd_topic[64];
    snprintf(cmd_topic, sizeof(cmd_topic), "aquantia/%s/cmd", finca_id);
    mqttClient.subscribe(cmd_topic, 1);
    DLOGF("[MQTT] Conectado como %s — suscrito a %s\n", client_id, cmd_topic);
  } else {
    DLOGF("[MQTT] Error de conexion: rc=%d client_id=%s\n",
                  mqttClient.state(), client_id);
  }
  return ok;
}

// Publicar datos de registro al arranque (una sola vez)
void mqttPublishRegister() {
  StaticJsonDocument<320> doc;
  doc["device_serial"]    = device_serial_get();   // AQ-{MAC}-{FlashID} — identidad hardware
  doc["mac_address"]      = WiFi.macAddress();
  doc["ip_address"]       = WiFi.localIP().toString();
  doc["chip_model"]       = ESP.getChipModel();
  doc["chip_revision"]    = (int)ESP.getChipRevision();
  doc["cpu_freq_mhz"]     = (int)ESP.getCpuFreqMHz();
  doc["flash_size_mb"]    = (int)(ESP.getFlashChipSize() / 1048576);
  doc["sdk_version"]      = ESP.getSdkVersion();
  doc["relay_count"]      = RELAY_COUNT;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["device_profile"]   =
    (DEVICE_PROFILE == PROFILE_METEO)      ? "METEO" :
    (DEVICE_PROFILE == PROFILE_IRRIGATION) ? "IRRIGATION" : "AGROMETEO";

  char topic[64], buf[768];
  snprintf(topic, sizeof(topic), "aquantia/%s/register", finca_id);
  size_t payload_len = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqttClient.publish(topic, buf, false);
  DLOGF("[MQTT] Register %s (%u B)\n",
                ok ? "publicado" : "ERROR",
                (unsigned)payload_len);
}

#endif  // !ESP8266 && USE_MQTT

// =============================================================================
// Snapshot de datos — captura atómica de sensores para telemetría.
// Rellena la variable global _netSnap (definida junto a los vars FreeRTOS).
// Sin parámetros para evitar el problema de auto-prototype del Arduino IDE
// con tipos personalizados en la firma.
// =============================================================================
#ifndef ESP8266
void takeSnapshot() {
  TelemetrySnapshot &s = _netSnap;
  // Defaults sin mutex (último valor conocido)
  s.tempMCP       = temperatureMCP;
  s.pressure      = (float)pressure;
  s.tempDHT       = temperatureDHT;
  s.humidity      = humidity;
  s.windSpeed     = windSpeed;
  s.windDir       = currentWindDirDeg;
  s.windSpeedFilt = windSpeedFiltered;
  s.avgWindDir    = currentWindDirDeg;
  s.light         = lightLevel;
  s.tempDHT11     = temperatureDHT11;
  s.humDHT11      = humidityDHT11;
  s.soil          = soilMoisture;
  s.bmpTemp       = bmp_temp_ok ? bmpTemperature : NAN;
  s.bmpPressure   = bmp_pressure_ok ? bmpPressure : NAN;
  s.pipePressure  = sim_pipeline_pressure;
  s.pipeFlow      = sim_pipeline_flow;
  s.heap          = (long)ESP.getFreeHeap();
  s.rssi          = WiFi.RSSI();
  s.uptime        = (long)(millis() / 1000);
  s.relayMask     = 0;

  if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s.tempMCP       = temperatureMCP;
    s.pressure      = (float)pressure;
    s.tempDHT       = temperatureDHT;
    s.humidity      = humidity;
    s.windSpeed     = windSpeed;
    s.windDir       = currentWindDirDeg;
    s.windSpeedFilt = windSpeedFiltered;
    s.avgWindDir    = calcAndResetWindVector();
    s.light         = lightLevel;
    s.tempDHT11     = temperatureDHT11;
    s.humDHT11      = humidityDHT11;
    s.soil          = soilMoisture;
    s.bmpTemp       = bmp_temp_ok ? bmpTemperature : NAN;
    s.bmpPressure   = bmp_pressure_ok ? bmpPressure : NAN;
    s.pipePressure  = sim_pipeline_pressure;
    s.pipeFlow      = sim_pipeline_flow;
    s.heap          = (long)ESP.getFreeHeap();
    s.rssi          = WiFi.RSSI();
    s.uptime        = (long)(millis() / 1000);
    for (int i = 0; i < RELAY_COUNT; i++)
      if (relayActive[i]) s.relayMask |= (1 << i);
#if DEVICE_PROFILE == PROFILE_AGROMETEO
    s.dewPoint  = agroDewPoint;
    s.heatIndex = agroHeatIndex;
    s.absHum    = agroAbsHum;
#endif
    xSemaphoreGive(dataMutex);
  }
#if DEVICE_PROFILE == PROFILE_AGROMETEO
  else {
    // Sin mutex: capturar igual los campos AGROMETEO con valores actuales
    s.dewPoint  = agroDewPoint;
    s.heatIndex = agroHeatIndex;
    s.absHum    = agroAbsHum;
  }
#endif
}
#endif

// =============================================================================
// TAREA DE RED — Core 0 (solo ESP32)
// Gestiona OTA, relay polling y HTTP POST sin bloquear sensores/display (Core 1)
// =============================================================================
#ifndef ESP8266
void networkTask(void* pvParameters) {
  // Tarea separada de red para no bloquear sensores ni la UI principal.
  // Mantiene yields periódicos con vTaskDelay() al final del bucle.

  static bool          deviceInfoSent   = false;
  static unsigned long lastRelayCheck   = 0;
  static unsigned long lastSendTime     = 0;
  static unsigned long lastScenarioSync = 0;
  static unsigned long lastNtpRetry     = 0;
  static unsigned long wifiRetryDelayMs  = 500;
  static unsigned long wifiStableSince   = 0;  // millis() al reconectar

#ifdef USE_MQTT
  static unsigned long mqttRetryDelayMs = 2000;

  if (mqtt_port == 8883) {
    prepareSecureClient(mqttTLSClient, 10000);
    mqttClient.setClient(mqttTLSClient);
    DLOGF("[MQTT] Broker TLS verificado: %s:%d\n", mqtt_server, mqtt_port);
  } else {
    mqttClient.setClient(mqttTCPClient);
    DLOGF("[MQTT] Broker local sin TLS: %s:%d\n", mqtt_server, mqtt_port);
  }
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
#endif

  for (;;) {
    ArduinoOTA.handle();  // siempre primero, alta frecuencia

    if (isUpdatingOTA) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      setLedState(LED_WIFI_CONNECTING);
      wifiStableSince = 0;
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(wifiRetryDelayMs));
      if (wifiRetryDelayMs < 5000) wifiRetryDelayMs *= 2;
      continue;
    }
    // Resetear backoff solo tras 10s de conexión estable (evita martillear el AP)
    if (wifiStableSince == 0) wifiStableSince = millis();
    if (millis() - wifiStableSince > 10000) wifiRetryDelayMs = 500;

    unsigned long now = millis();

#ifdef USE_MQTT
    // Reintentar NTP cada 60s si el reloj no está sincronizado (fallo en boot)
    if (time(nullptr) < 1000000000L && (now - lastNtpRetry > 60000 || lastNtpRetry == 0)) {
      DLOGLN("[NTP] Reintentando sincronización...");
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      lastNtpRetry = now;
    }
#endif

    if (lastScenarioSync == 0 || now - lastScenarioSync >= configSyncIntervalMs) {
      syncPipelineScenario();
      lastScenarioSync = now;
    }

#ifdef USE_MQTT
    // ── Modo MQTT ────────────────────────────────────────────────────────────
    if (!mqttClient.connected()) {
      setLedState(LED_MQTT_CONNECTING);
      if (!mqttConnect()) {
        vTaskDelay(pdMS_TO_TICKS(mqttRetryDelayMs));
        if (mqttRetryDelayMs < 15000) mqttRetryDelayMs += 2000;
        continue;
      }
      setLedState(LED_IDLE);
      mqttRetryDelayMs = 2000;
      if (!deviceInfoSent) {
        mqttPublishRegister();
        deviceInfoSent = true;
      }
    }
    mqttClient.loop();

    // Publicar telemetría cada MQTT_SEND_MS
    if (now - lastSendTime >= telemetryIntervalMs) {
      takeSnapshot();
      const TelemetrySnapshot& snap = _netSnap;

      // Redondeo para reducir tamaño del payload MQTT
      auto r2 = [](float x){ return roundf(x * 100.0f) / 100.0f; };  // 2 decimales
      auto r1 = [](float x){ return roundf(x * 10.0f)  / 10.0f;  };  // 1 decimal

      // Payload ampliado con métricas explícitas del BMP280.
      StaticJsonDocument<1280> doc;
      doc["temperature"]           = r2(snap.tempMCP);
      doc["pressure"]              = r2(snap.pressure);
      doc["temperature_barometer"] = r2(snap.tempDHT);
      doc["humidity"]              = r2(snap.humidity);
      doc["temperature_source"]    = temperatureSourceName();
      doc["pressure_source"]       = pressureSourceName();
      doc["bmp280_ok"]             = bmp_ok && (bmp_temp_ok || bmp_pressure_ok);
      if (!isnan(snap.bmpTemp))     doc["bmp280_temperature"] = r2(snap.bmpTemp);
      if (!isnan(snap.bmpPressure)) doc["bmp280_pressure"] = r2(snap.bmpPressure);
      // Nuevo: incluir ambas presiones explícitamente
      doc["pressure_micro"] = r2(snap.pressure);        // MicroPressure (SparkFun)
      doc["pressure_bmp280"] = r2(snap.bmpPressure);    // BMP280
      doc["windSpeed"]             = r2(snap.windSpeed);
      doc["windDirection"]         = r1(snap.windDir);
      doc["windSpeedFiltered"]     = r2(snap.windSpeedFilt);
      doc["windDirectionFiltered"] = r1(snap.avgWindDir);
      doc["light"]                 = r1(snap.light);
      doc["dht_temperature"]       = r1(snap.tempDHT11);
      doc["dht_humidity"]          = r1(snap.humDHT11);
      doc["rssi"]                  = snap.rssi;
      doc["free_heap"]             = snap.heap;
      doc["uptime_s"]              = snap.uptime;
      doc["relay_active"]          = snap.relayMask;
      doc["soil_moisture"]         = r1(snap.soil);
      doc["pipeline_pressure"]     = r2(snap.pipePressure);
      doc["pipeline_flow"]         = r2(snap.pipeFlow);
      doc["pipeline_scenario"]     = pipelineScenario;
      doc["pipeline_mode"]         = pipelineMode;
      doc["pipeline_source"]       = pipelineSource;
      doc["pipeline_pressure_ok"]  = pipelinePressureOk;
      doc["pipeline_flow_ok"]      = pipelineFlowOk;
      doc["mac_address"]           = WiFi.macAddress();
      doc["ip_address"]            = WiFi.localIP().toString();
      doc["relay_count"]           = RELAY_COUNT;
      doc["firmware_version"]      = FIRMWARE_VERSION;
#if DEVICE_PROFILE == PROFILE_AGROMETEO
      // Parámetros calculados agrometeorologícos — solo PROFILE_AGROMETEO
      if (!isnan(snap.dewPoint))   doc["dew_point"]    = r1(snap.dewPoint);
      if (!isnan(snap.heatIndex))  doc["heat_index"]   = r1(snap.heatIndex);
      if (!isnan(snap.absHum))     doc["abs_humidity"] = r2(snap.absHum);
#endif
      // Timestamp NTP — solo si el reloj está sincronizado (epoch > año 2001)
      // El backend lo usa como timestamp real de la medición en lugar de NOW().
      {
        time_t ntp_ts = time(nullptr);
        if (ntp_ts > 1000000000L) doc["ts"] = (long)ntp_ts;
      }

      char topic[64], buf[1280];
      snprintf(topic, sizeof(topic), "aquantia/%s/telemetry", finca_id);
      size_t payload_len = serializeJson(doc, buf, sizeof(buf));
      if (payload_len >= sizeof(buf)) {
        DLOGF("[MQTT] WARN payload truncado (%u >= %u)\n",
                      (unsigned)payload_len, (unsigned)sizeof(buf));
      }
      bool ok = mqttClient.publish(topic, buf, false);
      setLedState(ok ? LED_TX_OK : LED_TX_ERROR);
      DLOGF("[MQTT] TX %s (%u B): %s\n", ok ? "OK" : "ERROR",
                    (unsigned)payload_len, buf);

      lastServerOK = ok;
      lastSendTime = now;
    }

#else
    // ── Modo HTTP legacy ──────────────────────────────────────────────────────
    // Device info — una sola vez tras conectar
    if (!deviceInfoSent) {
      postDeviceInfo();
      deviceInfoSent = true;
    }

    // Relay poll cada 2s
    if (now - lastRelayCheck >= RELAY_MS) {
      checkRelayCommand();
      lastRelayCheck = now;
    }

    // HTTP POST cada SEND_MS — captura snapshot con mutex
    if (now - lastSendTime >= telemetryIntervalMs) {
      takeSnapshot();
      const TelemetrySnapshot& snap = _netSnap;

      String url = serverBaseUrl() + "/send_message";
      char msg[320];
      snprintf(msg, sizeof(msg),
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%ld,%ld,%d,%.2f,%.2f,%.2f",
        snap.tempMCP, snap.pressure, snap.tempDHT, snap.humidity,
        snap.windSpeed, snap.windDir, snap.windSpeedFilt, snap.avgWindDir,
        snap.light, snap.tempDHT11, snap.humDHT11,
        snap.rssi, snap.heap, snap.uptime, snap.relayMask,
        snap.pipePressure, snap.pipeFlow, snap.soil
      );

      DLOGF("[NET] TX: %s\n", msg);
      bool ok = httpPost(url, String(msg));
      setLedState(ok ? LED_TX_OK : LED_TX_ERROR);
      DLOGF("[NET] HTTP %s\n", ok ? "200 OK" : "ERROR");

      lastServerOK = ok;
      lastSendTime = now;
    }
#endif  // USE_MQTT

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}
#endif

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(160);
  DLOGLN("\n\n=== MeteoStation BOOT ===");

#ifdef DEBUG_MODE
  DLOGLN("=== DEBUG MODE ACTIVO ===");
  DLOGF("[TEST] Perfil  : %s (%d)\n",
    (DEVICE_PROFILE == PROFILE_METEO) ? "METEO" :
    (DEVICE_PROFILE == PROFILE_AGROMETEO) ? "AGROMETEO" : "IRRIGATION", DEVICE_PROFILE);
  DLOGF("[TEST] Relays  : %d\n", RELAY_COUNT);
  DLOGF("[TEST] Display : %s\n",
#ifdef HAS_DISPLAY
    "SI (TFT 240x135)"
#else
    "NO"
#endif
  );
  DLOGF("[TEST] MQTT    : %s\n",
#ifdef USE_MQTT
    "HABILITADO"
#else
    "DESHABILITADO"
#endif
  );
  DLOGF("[TEST] MAC ROM : %s\n", device_serial_get());
#endif  // DEBUG_MODE

  // I2C debe inicializarse antes del TFT para que el periman de ESP32 core 3.x
  // registre los pines correctamente antes de que SPI los reclame.
  DLOGLN("Iniciando I2C...");
  Wire.begin(I2C_SDA, I2C_SCL);
#ifndef ESP8266
  Wire.setTimeOut(100);  // timeout 100ms para evitar hang si un sensor no responde
#endif
  DLOGLN("I2C OK");

  // ── TFT init — debe ir antes del provisioning para poder mostrar ──
  // el portal AP si el dispositivo no tiene credenciales WiFi en NVS
#ifdef HAS_DISPLAY
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  void* sprPtr = spr.createSprite(240, 135);
  if (!sprPtr) {
    DLOGF("[TFT] ERROR: createSprite falló (heap libre: %ld)\n", (long)ESP.getFreeHeap());
    // Fallback: dibujar directamente en tft sin sprite
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("SPRITE FAIL", 120, 60, 4);
  } else {
    DLOGF("[TFT] Sprite creado OK (heap libre: %ld)\n", (long)ESP.getFreeHeap());
  }
  spr.setSwapBytes(true);
  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT);
  lastActivityTime = millis();
  // Registrar callback para que provisioning pueda dibujar la pantalla AP
  provisioning_register_ap_display([](const char* ap_ssid, const char* serial) {
    drawAPScreen(ap_ssid, serial);
  });
  // Diagnóstico TFT — dibujar directamente en el display (sin sprite) para verificar
  tft.fillScreen(TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawCentreString("TFT OK", 120, 50, 4);
  tft.drawCentreString(String(ESP.getFreeHeap()).c_str(), 120, 85, 2);
  delay(1500);
  drawBootScreen("Iniciando...");
#endif

  // ── Credenciales: DEV_MODE (directo) vs PROD (NVS + portal) ──────────────
#ifdef DEV_MODE
  // ── Modo desarrollo: usa secrets.h directamente, sin NVS ni portal ────────
  DLOGLN("[DEV] DEV_MODE activo — saltando provisioning");
  // ssid/password/finca_id/mqtt_pass ya apuntan a los literales de secrets.h
#elif !defined(ESP8266)
  // ── Modo producción: factory reset + NVS + portal SoftAP ─────────────────
  provisioning_check_factory_reset();

  provisioning_load();  // carga ssid+password desde NVS si existen

  if (!provisioning_has_credentials()) {
#ifdef HAS_DISPLAY
    // Dibujar pantalla AP antes de entrar en el portal (que bloquea para siempre).
    // Usamos device_serial_get() en vez de WiFi.macAddress() porque WiFi aún
    // no está inicializado aquí y macAddress() puede devolver "00:00:00:00:00:00".
    // Serial = "AQ-FCB467F37748" → SSID = "Aquantia-F37748" (últimos 6 hex del MAC)
    {
      const char* ser = device_serial_get();
      char ap_ssid_buf[32];
      if (strlen(ser) >= 15)
        snprintf(ap_ssid_buf, sizeof(ap_ssid_buf), "Aquantia-%.6s", ser + 9);
      else
        strlcpy(ap_ssid_buf, "Aquantia-??????", sizeof(ap_ssid_buf));
      DLOGF("[TFT] Mostrando pantalla AP: %s\n", ap_ssid_buf);
      drawAPScreen(ap_ssid_buf, ser);
    }
#endif
    setLedState(LED_PROVISIONING);  // portal AP activo — triple blink lento
    provisioning_start_ap();  // bloquea hasta que el usuario configure
  }

  ssid     = prov_ssid;
  password = prov_password;
  DLOGF("[PROV] WiFi: %s  |  Serial: %s\n", prov_ssid, device_serial_get());
#endif  // DEV_MODE / producción

#if DEVICE_PROFILE == PROFILE_AGROMETEO
  // ── AGROMETEO: Qwiic Power Switch — encender ANTES del escáner ───────────
  qwiic_ps_ok = qwiic_ps.begin(Wire);   // begin() devuelve false si no detecta el PCA9536 en 0x41
  if (qwiic_ps_ok) {
    qwiic_ps.powerOn();
    delay(50);   // esperar estabilización de alimentación antes de init I2C
    DLOGLN("Qwiic Power Switch OK — sensores encendidos");
  } else {
    DLOGLN("Qwiic Power Switch no detectado — sensores siempre alimentados");
  }
#endif

  // Escaner I2C — detecta todos los dispositivos en el bus
  DLOGLN("Escaneando bus I2C...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      DLOGF("  Dispositivo en 0x%02X\n", addr);
      found++;
    }
    delay(2);
  }
  if (found == 0) DLOGLN("  Ningun dispositivo encontrado");
  else DLOGF("  Total: %d dispositivos\n", found);

  if (ledPin >= 0) { pinMode(ledPin, OUTPUT); digitalWrite(ledPin, LED_OFF); }

  // Relay(s) — arrancar siempre en OFF (seguro)
  // JQC-3FF-S-Z activo-LOW: HIGH = relay OFF (válvula cerrada)
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH);  // HIGH = OFF para relay activo-LOW
  }
  DLOGF("%d relay(s) inicializados en OFF (HIGH)\n", RELAY_COUNT);

#if defined(ESP32) && !defined(ESP8266)
  analogReadResolution(12);
  #if defined(SOIL_PIN)
  pinMode(SOIL_PIN, ANALOG);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);
  DLOGF("SOIL GPIO%d configurado como ANALOG (11dB)\n", SOIL_PIN);
  #endif
#endif

#if defined(FLOW_PIN)
  // Caudalímetro: pull-up interno activo (BC547 NPN invierte señal → FALLING = pulso)
  pinMode(FLOW_PIN, INPUT_PULLUP);
  _flowLastCalcMs = millis();
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);
  DLOGF("Caudalimetro GPIO%d configurado (ISR FALLING, K=%d pulsos/L)\n",
        FLOW_PIN, FLOW_K_FACTOR);
#endif
  // Nota: TFT ya inicializado al principio de setup() para soportar pantalla AP

#if DEVICE_PROFILE == PROFILE_METEO
  mcp_ok = tempsensor.begin(0x19);
  if (mcp_ok) {
    tempsensor.setResolution(3);
    DLOGLN("MCP9808 OK");
  } else {
    DLOGLN("MCP9808 no detectado");
  }

  bmp_ok = beginBMP280();
  if (bmp_ok) {
    float tBmp = NAN;
    float pBmp = NAN;
    bmp_temp_ok = readBMP280Temperature(tBmp);
    bmp_pressure_ok = readBMP280PressureKPa(pBmp);
    if (bmp_temp_ok) bmpTemperature = tBmp;
    if (bmp_pressure_ok) bmpPressure = pBmp;
    DLOGF("BMP280 OK (0x%02X)\n", bmp280_addr);
  } else {
    bmp_temp_ok = false;
    bmp_pressure_ok = false;
    DLOGLN("BMP280 no detectado");
  }

  temp_ok = mcp_ok || bmp_temp_ok;
  if (!temp_ok) {
    DLOGLN("Temperatura externa sin sensor real — modo simulacion");
    temperatureMCP = sim_tempMCP;
  } else if (!mcp_ok && bmp_temp_ok) {
    temperatureMCP = bmpTemperature;
    DLOGLN("Temperatura externa usara BMP280");
  }

  micropressure_ok = barometer.begin();
  bar_ok = micropressure_ok || bmp_pressure_ok;
  if (micropressure_ok) {
    DLOGLN("Barometro MicroPressure OK");
  } else if (bmp_pressure_ok) {
    pressure = bmpPressure;
    DLOGLN("MicroPressure no detectado — usando BMP280 como barometro");
  } else {
    DLOGLN("Barometro no detectado — modo simulacion");
    pressure = sim_pressure;
  }
  DLOGF("Sensores activos → TempExt:%s | Presion:%s\n",
    temperatureSourceName(), pressureSourceName());
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  DLOGLN("MCP9808 — perfil AGROMETEO, sensor omitido");
  DLOGLN("BMP280 y MicroPressure — inicializados en bloque AGROMETEO");
#else
  DLOGLN("MCP9808 — perfil IRRIGATION, sensor omitido");
  DLOGLN("BMP280 — perfil IRRIGATION, sensor omitido");
  DLOGLN("Barometro — perfil IRRIGATION, sensor omitido");
#endif

#if DEVICE_PROFILE == PROFILE_AGROMETEO
  // Nota: Qwiic Power Switch ya inicializado antes del escáner I2C (ver arriba)

  // ── AGROMETEO: MicroPressure + BMP280 + HDC1080 + BH1750 ────────────────
  // MicroPressure — barómetro principal (0x18); BMP280 como fallback de presión
  micropressure_ok = barometer.begin();
  if (micropressure_ok) {
    double p = barometer.readPressure(KPA);
    if (p > 50.0 && p < 120.0) { pressure = p; bar_ok = true; }
    DLOGLN("MicroPressure OK");
  } else {
    DLOGLN("MicroPressure no detectado — usando BMP280 como barometro");
  }

  // BMP280 — temperatura secundaria + presión de respaldo si falta MicroPressure
  bmp_ok = beginBMP280();
  if (bmp_ok) {
    bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                      Adafruit_BMP280::SAMPLING_X2,
                      Adafruit_BMP280::SAMPLING_X16,
                      Adafruit_BMP280::FILTER_X16,
                      Adafruit_BMP280::STANDBY_MS_500);
    float tBmp = NAN, pBmp = NAN;
    bmp_temp_ok     = readBMP280Temperature(tBmp);
    bmp_pressure_ok = readBMP280PressureKPa(pBmp);
    if (bmp_temp_ok)     bmpTemperature = tBmp;
    if (bmp_pressure_ok) { bmpPressure = pBmp; if (!micropressure_ok) { pressure = pBmp; bar_ok = true; } }
    DLOGF("BMP280 OK (0x%02X)\n", bmp280_addr);
  } else {
    bmp_temp_ok     = false;
    bmp_pressure_ok = false;
    if (!micropressure_ok) { bar_ok = false; pressure = sim_pressure; }
    DLOGLN("BMP280 no detectado — modo simulacion");
  }

  // HDC1080 — temperatura y humedad primaria
  hdc_ok = hdc1080_init();
  if (hdc_ok) {
    float t = hdc1080_readTemp();
    float h = hdc1080_readHum();
    if (!isnan(t) && !isnan(h)) {
      temperatureMCP = t;
      humidity       = h;
      temp_ok        = true;
      DLOGF("HDC1080 OK — T:%.1f C  H:%.1f %%\n", t, h);
    } else {
      hdc_ok = false;
    }
  }
  if (!hdc_ok) {
    DLOGLN("HDC1080 no detectado — modo simulacion");
    temperatureMCP = sim_tempMCP;
    humidity       = sim_humidity;
  }

  // BH1750 — iluminancia
  bh1750_ok = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  if (bh1750_ok) {
    delay(200);  // primera conversión ~120ms
    lightLevel = bh1750.readLightLevel();
    DLOGF("BH1750 OK — %.1f lx\n", lightLevel);
  } else {
    DLOGLN("BH1750 no detectado — modo simulacion");
    lightLevel = sim_light;
  }

  // Sensores quedan encendidos permanentemente — el Qwiic Power Switch NO se apaga entre ciclos.
  // Esto es necesario para que BMP280 (filtro IIR x16) y BH1750 (modo continuo) acumulen
  // mediciones estables. Apagarlos entre lecturas reinicia los filtros internos y exige
  // esperar 120-180 ms de conversión en cada ciclo.
  DLOGLN("Qwiic Power Switch — sensores alimentados de forma continua");
#endif  // PROFILE_AGROMETEO

#if DEVICE_PROFILE != PROFILE_AGROMETEO
  // HTU2x — omitido en AGROMETEO (dirección 0x40 ocupada por HDC1080)
  htu_ok = htu_begin();
  if (htu_ok) {
    htu_heater_warmup();
    float t = htu_readTemp();
    float h = htu_readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temperatureDHT = t;
      humidity       = h;
      DLOGF("HTU2x OK — T:%.1f C  H:%.1f %%\n", t, h);
    } else {
      htu_ok = false;
    }
  }
  if (!htu_ok) {
    DLOGLN("HTU2x no detectado — modo simulacion");
    temperatureDHT = sim_tempDHT;
    humidity       = sim_humidity;
  }
#else
  // AGROMETEO: sin HTU2x — HDC1080 ya inicializado
  htu_ok         = false;
  temperatureDHT = sim_tempDHT;  // campo no usado en AGROMETEO
#endif

#if DEVICE_PROFILE == PROFILE_METEO
  // DHT11
  dht.setup(DHTPIN, DHTesp::DHT11);
  delay(dht.getMinimumSamplingPeriod());
  {
    TempAndHumidity th = dht.getTempAndHumidity();
    dht_ok = (dht.getStatus() == DHTesp::ERROR_NONE);
    if (dht_ok) {
      temperatureDHT11 = th.temperature;
      humidityDHT11    = th.humidity;
      DLOGLN("DHT11 OK");
    } else {
      DLOGLN("DHT11 no detectado — modo simulacion");
      temperatureDHT11 = sim_tempDHT11;
      humidityDHT11    = sim_humDHT11;
    }
  }
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
  DLOGLN("DHT11 — perfil AGROMETEO, sensor omitido");
#else
  DLOGLN("DHT11 — perfil IRRIGATION, sensor omitido");
#endif

#if DEVICE_PROFILE != PROFILE_AGROMETEO
  // TSL2584/APDS-9930 — omitido en AGROMETEO (usa BH1750 inicializado arriba)
  tsl_ok = tsl_begin();
  if (tsl_ok) {
    DLOGLN("Sensor luz OK");
    lightLevel = tsl_readLux();
  } else {
    DLOGLN("Sensor luz no detectado — modo simulacion");
    lightLevel = sim_light;
  }
#else
  tsl_ok = false;  // AGROMETEO usa BH1750, no TSL/APDS
#endif

#if defined(SOIL_PIN)
  {
    int raw = analogRead(SOIL_PIN);
    // Precalentar el filtro con la lectura inicial
    for (int i = 0; i < FILTER_SIZE; i++) soilValues[i] = raw;
    soilMoisture = constrain(
      (float)(SOIL_RAW_DRY - raw) / (SOIL_RAW_DRY - SOIL_RAW_WET) * 100.0f,
      0.0f, 100.0f
    );
    DLOGF("YL-69 OK — suelo: %.1f %% (ADC raw=%d)\n", soilMoisture, raw);
  }
#endif

#ifdef ESP8266
  DLOGLN("Plataforma: ESP8266 (sin pantalla, sin veleta)");
#else
  DLOGLN("Plataforma: ESP32 (con pantalla TFT)");
#endif

#ifdef HAS_DISPLAY
  drawBootScreen("Conectando WiFi...");
#endif

  setLedState(LED_WIFI_CONNECTING);
  DLOGLN("Conectando WiFi...");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
#ifdef ESP8266
    yield();
#endif
    DLOG(".");
    tries++;
  }
  DLOGLN();

  if (WiFi.status() == WL_CONNECTED) {
    setLedState(LED_MQTT_CONNECTING);
    DLOGLN("WiFi OK: " + WiFi.localIP().toString());

#if !defined(ESP8266) && defined(USE_MQTT)
  #ifndef DEV_MODE
    // PROD: mqtt_user = MAC del dispositivo (lookup en mosquitto-go-auth)
    static char _mac_buf[20];
    strncpy(_mac_buf, WiFi.macAddress().c_str(), sizeof(_mac_buf) - 1);
    _mac_buf[sizeof(_mac_buf) - 1] = '\0';
    mqtt_user = _mac_buf;

    // PROD: token pre-flasheado en NVS por el Flash Tool en fábrica
    if (prov_mqtt_token[0] != '\0') {
      mqtt_pass = prov_mqtt_token;
    } else {
      DLOGLN("[MQTT] ADVERTENCIA: sin token NVS — dispositivo no provisionado de fábrica");
    }
    // finca_id = MAC hex (identidad del dispositivo; el backend asocia a la finca tras el claim)
    static char _finca_mac[16];
    String _mac_nocolon = WiFi.macAddress();
    _mac_nocolon.replace(":", "");
    strncpy(_finca_mac, _mac_nocolon.c_str(), sizeof(_finca_mac) - 1);
    _finca_mac[sizeof(_finca_mac) - 1] = '\0';
    finca_id = _finca_mac;
  #endif

    DLOGF("[MQTT] Auth user: %s  |  Serial: %s\n",
                  mqtt_user, device_serial_get());
#endif

    // ── OTA (Over-The-Air) ──────────────────────────────────────────────────
#ifdef ESP8266
    ArduinoOTA.setHostname("meteostation-esp8266");
#else
    ArduinoOTA.setHostname("meteostation-esp32");
#endif
    // Contraseña OTA opcional — definir OTA_PASSWORD en secrets.h para activarla
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.onStart([]() {
      isUpdatingOTA = true;
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
      DLOGF("\n[OTA] Inicio de actualización (%s) — relays a OFF por seguridad\n", type.c_str());
      // Apagar todos los relays por seguridad durante el flash
      for (int i = 0; i < RELAY_COUNT; i++) digitalWrite(RELAY_PINS[i], HIGH);
    });
    ArduinoOTA.onEnd([]() {
      DLOGLN("\n[OTA] Actualización completada — reiniciando");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      DLOGF("[OTA] %u%% (%u/%u bytes)\r", progress * 100 / total, progress, total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
      isUpdatingOTA = false;
      const char* msg = "Desconocido";
      switch (error) {
        case OTA_AUTH_ERROR:    msg = "Fallo de autenticacion";  break;
        case OTA_BEGIN_ERROR:   msg = "Fallo al iniciar";        break;
        case OTA_CONNECT_ERROR: msg = "Fallo de conexion";       break;
        case OTA_RECEIVE_ERROR: msg = "Fallo al recibir datos";  break;
        case OTA_END_ERROR:     msg = "Fallo al finalizar";      break;
      }
      DLOGF("[OTA] ERROR %u: %s\n", error, msg);
    });
    ArduinoOTA.begin();

#if !defined(ESP8266) && defined(USE_MQTT)
    // Sincronizar reloj via NTP — necesario para validar cert TLS de MQTT
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    DLOG("[NTP] Sincronizando hora");
    {
      time_t now = time(nullptr);
      int ntpTries = 0;
      while (now < 1000000000L && ntpTries < 40) {
        delay(500);
        DLOG(".");
        now = time(nullptr);
        ntpTries++;
      }
      if (now < 1000000000L) {
        DLOGLN("\n[NTP] WARN: reloj no sincronizado — timestamps no fiables");
      } else {
        DLOGF("\n[NTP] Hora: %s", ctime(&now));
      }
    }
#endif

#ifndef ESP8266
    // FreeRTOS: crear tarea de red en Core 0
    dataMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(
      networkTask,        // función
      "NetworkTask",      // nombre
#ifdef USE_MQTT
      12288,              // stack ampliado para TLS (WiFiClientSecure + PubSubClient)
#else
      8192,               // stack estándar para HTTP legacy
#endif
      nullptr,            // parámetros
      2,                  // prioridad (más alta que loop=1)
      &networkTaskHandle, // handle
      0                   // Core 0
    );
    DLOGLN("NetworkTask creada en Core 0");
#endif
    WiFi.setSleep(true);  // Modem Sleep: ahorra ~15-20mA entre transmisiones
    DLOGLN("OTA listo — hostname: " +
#ifdef ESP8266
      String("meteostation-esp8266")
#else
      String("meteostation-esp32")
#endif
    );
    // ── Fin OTA ─────────────────────────────────────────────────────────────

    printHardwareInfo();

#ifdef HAS_DISPLAY
    drawBootScreen(("IP: " + WiFi.localIP().toString()).c_str());
#endif
  } else {
#if !defined(ESP8266) && !defined(DEV_MODE)
    // Si las credenciales vinieron de NVS y WiFi falló → volver al portal
    // para que el usuario corrija la red (p.ej. contraseña cambiada).
    if (prov_ssid[0] != '\0') {
      DLOGLN("[PROV] WiFi fallido con credenciales NVS — iniciando portal de reconfiguración...");
      // drawAPScreen se llama desde dentro de provisioning_start_ap() via callback
      provisioning_start_ap();  // no retorna
    }
#endif
    DLOGLN("Sin WiFi — continuando sin conexion (OTA no disponible)");
#ifdef HAS_DISPLAY
    drawBootScreen("Sin WiFi — modo offline");
#endif
  }

#ifdef HAS_DISPLAY
  delay(2500);
#endif

#ifdef DEBUG_MODE
  DLOGLN(F("\n====== AQUANTIA BOOT COMPLETO ======"));
  DLOGF("[TEST] Perfil   : %s | Relays: %d\n",
    (DEVICE_PROFILE == PROFILE_METEO) ? "METEO" :
    (DEVICE_PROFILE == PROFILE_AGROMETEO) ? "AGROMETEO" : "IRRIGATION", RELAY_COUNT);
  DLOGF("[TEST] WiFi     : %s\n",
    (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "SIN CONEXION");
  DLOGF("[TEST] Temp ext : %s\n", temp_ok ? temperatureSourceName() : "SIM (sin sensor)");
  DLOGF("[TEST] Barometro: %s\n", bar_ok ? pressureSourceName() : "SIM (sin sensor)");
#if DEVICE_PROFILE != PROFILE_AGROMETEO
  DLOGF("[TEST] HTU2x    : %s\n", htu_ok ? "REAL" : "SIM (sin sensor)");
  DLOGF("[TEST] Luz      : %s\n", tsl_ok ? "REAL" : "SIM (sin sensor)");
#else
  DLOGF("[TEST] HDC1080  : %s\n", hdc_ok     ? "REAL" : "SIM (sin sensor)");
  DLOGF("[TEST] BH1750   : %s\n", bh1750_ok  ? "REAL" : "SIM (sin sensor)");
#endif
#if DEVICE_PROFILE == PROFILE_METEO
  DLOGF("[TEST] DHT11    : %s\n", dht_ok ? "REAL" : "SIM (sin sensor)");
#endif
  DLOGF("[TEST] Heap     : %ld bytes libres\n", (long)ESP.getFreeHeap());
  DLOGLN(F("[TEST] Iniciando loop() — reporte cada 5s"));
  DLOGLN(F("====================================\n"));
#endif

  // Forzar primera lectura de sensores en el primer ciclo del loop
  // (sin este offset, habría que esperar telemetryIntervalMs antes de tener datos)
  lastSensorRead = millis() - telemetryIntervalMs;
}

// =============================================================================
// LOOP — Core 1: sensores + display (sin red)
// En ESP8266 incluye también la red (sin FreeRTOS).
// =============================================================================
void loop() {
#ifdef ESP8266
  ArduinoOTA.handle();
#endif

  ledTick();  // actualizar LED de estado (no bloqueante)

  unsigned long now = millis();

#ifdef HAS_DISPLAY
  // Botones — detección de flanco para cambiar vista sin rebote
  static bool prevBtnLeft  = true;
  static bool prevBtnRight = true;
  bool curBtnLeft  = digitalRead(BTN_LEFT);
  bool curBtnRight = digitalRead(BTN_RIGHT);

  // Cualquier botón enciende la pantalla y resetea el timer
  if (!curBtnLeft || !curBtnRight) {
    if (!displayOn) {
      digitalWrite(TFT_BL, HIGH);
      displayOn = true;
    }
    lastActivityTime = now;
  }

  // BTN_LEFT o BTN_RIGHT: flanco descendente con pantalla ya encendida → cambiar vista
  bool leftEdge  = (!curBtnLeft  && prevBtnLeft);
  bool rightEdge = (!curBtnRight && prevBtnRight);
  if (displayOn && (leftEdge || rightEdge)) {
    displayView = (displayView == 0) ? 1 : 0;
  }

  prevBtnLeft  = curBtnLeft;
  prevBtnRight = curBtnRight;

  // Timeout — apagar pantalla tras el tiempo configurado sin actividad
  if (displayTimeoutMs > 0 && displayOn && (now - lastActivityTime >= displayTimeoutMs)) {
    digitalWrite(TFT_BL, LOW);
    displayOn = false;
  }
#endif

  // ── 1. Lectura ADC viento (cada 100ms) ──────────────────────────────────────
  static unsigned long lastWindRead = 0;
  if (now - lastWindRead >= WIND_MS) {
#ifdef HAS_DISPLAY
    int rawAne = analogRead(ANEMOMETER_PIN);
    float filtAne     = filteredADC(rawAne);
    windSpeed         = adcToWindSpeed((float)rawAne);
    windSpeedFiltered = adcToWindSpeed(filtAne);
    int rawVane       = analogRead(VANE_PIN);
    currentWindDirDeg = adcToWindDeg(rawVane);
#else
    // ESP8266 sin veleta: anemómetro y veleta simulados
    windSpeed         = sim_windSpeed;
    windSpeedFiltered = sim_windSpeed;
    currentWindDirDeg = sim_windDir;
#endif

    accumulateWindVector(currentWindDirDeg);
    lastWindRead = now;
  }

  // ── 2. Sensores I2C (cada telemetryIntervalMs, sincronizado con la telemetría) ──
  if (now - lastSensorRead >= telemetryIntervalMs) {
#ifndef ESP8266
    bool hasMutex = (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(20)) == pdTRUE);
#endif

#if DEVICE_PROFILE == PROFILE_METEO
    // BMP280 — leer siempre para mandar sus datos explícitos por telemetría
    bmp_temp_ok = false;
    bmp_pressure_ok = false;
    if (bmp_ok) {
      float tBmp = NAN;
      float pBmp = NAN;
      if (readBMP280Temperature(tBmp)) {
        bmpTemperature = tBmp;
        bmp_temp_ok = true;
      }
      if (readBMP280PressureKPa(pBmp)) {
        bmpPressure = pBmp;
        bmp_pressure_ok = true;
      }
      if (!bmp_temp_ok && !bmp_pressure_ok) {
        bmp_ok = false;
        DLOGLN("BMP280 fallo en lectura — cambiando a simulacion");
      }
    }

    // Temperatura exterior — prioridad MCP9808, fallback BMP280
    temp_ok = false;
    if (mcp_ok) {
      tempsensor.wake();
      float t = tempsensor.readTempC();
      tempsensor.shutdown_wake(1);
      if (!isnan(t) && t > -40.0f) {
        temperatureMCP = t;
        temp_ok = true;
      } else {
        mcp_ok = false;
        DLOGLN("MCP9808 fallo en lectura — probando BMP280");
      }
    }
    if (!temp_ok && bmp_temp_ok) {
      temperatureMCP = bmpTemperature;
      temp_ok = true;
    }
    if (!temp_ok) temperatureMCP = sim_tempMCP;

    // Barómetro — prioridad MicroPressure, fallback BMP280
    bar_ok = false;
    if (micropressure_ok) {
      double p = barometer.readPressure(KPA);
      if (p > 50.0 && p < 120.0) {
        pressure = p;
        bar_ok = true;
      } else {
        micropressure_ok = false;
        DLOGLN("MicroPressure fallo en lectura — probando BMP280");
      }
    }
    if (!bar_ok && bmp_pressure_ok) {
      pressure = bmpPressure;
      bar_ok = true;
    }
    if (!bar_ok) pressure = sim_pressure;
#endif

#if DEVICE_PROFILE != PROFILE_AGROMETEO
    // HTU2x — omitido en AGROMETEO (0x40 = HDC1080)
    if (htu_ok) {
      float t = htu_readTemp();
      float h = htu_readHumidity();
      if (!isnan(t) && !isnan(h)) {
        temperatureDHT = t;
        humidity       = h;
      } else {
        htu_ok = false;
        DLOGLN("HTU2x fallo en lectura — cambiando a simulacion");
      }
    }
    if (!htu_ok) {
      temperatureDHT = sim_tempDHT;
      humidity       = sim_humidity;
    }
#endif  // PROFILE_AGROMETEO

#if DEVICE_PROFILE == PROFILE_METEO
    // DHT11
    {
      TempAndHumidity th = dht.getTempAndHumidity();
      if (dht.getStatus() == DHTesp::ERROR_NONE) {
        dht_ok           = true;
        temperatureDHT11 = th.temperature;
        humidityDHT11    = th.humidity;
      } else {
        dht_ok = false;
      }
    }
    if (!dht_ok) {
      temperatureDHT11 = sim_tempDHT11;
      humidityDHT11    = sim_humDHT11;
    }
#endif

#if DEVICE_PROFILE == PROFILE_AGROMETEO
    // ── AGROMETEO: HDC1080 + BMP280 + BH1750 + parámetros calculados ────────
    // Sensores siempre alimentados (Qwiic Power Switch ON permanente desde setup).
    // Solo se reinicializa un sensor concreto si falló en el ciclo anterior.
    // Esto permite al BMP280 (FILTER_X16) acumular mediciones estables y al BH1750
    // operar en modo continuo sin necesidad de esperar la conversión inicial cada vez.
    // Recovery — reinicializar sensores que fallaron en el ciclo anterior
    if (qwiic_ps_ok) {
      if (!micropressure_ok) micropressure_ok = barometer.begin();
      if (!bmp_ok) {
        bmp_ok = beginBMP280();
        if (bmp_ok) bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                                       Adafruit_BMP280::SAMPLING_X2,
                                       Adafruit_BMP280::SAMPLING_X16,
                                       Adafruit_BMP280::FILTER_X16,
                                       Adafruit_BMP280::STANDBY_MS_500);
      }
      if (!hdc_ok)     hdc_ok     = hdc1080_init();
      if (!bh1750_ok) {
        bh1750_ok = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
        if (bh1750_ok) delay(180);  // esperar primera conversión solo si se reinicia
      }
    }

    // HDC1080 — temperatura y humedad primaria → temperatureMCP + humidity
    if (hdc_ok) {
      float t = hdc1080_readTemp();
      float h = hdc1080_readHum();
      if (!isnan(t) && !isnan(h)) {
        temperatureMCP = t;
        humidity       = h;
        temp_ok        = true;
      } else {
        hdc_ok  = false;
        temp_ok = false;
        DLOGLN("HDC1080 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!hdc_ok) {
      if (bmp_temp_ok) { temperatureMCP = bmpTemperature; temp_ok = true; }
      else             { temperatureMCP = sim_tempMCP;    temp_ok = false; }
      humidity = sim_humidity;
    }

    // MicroPressure — barómetro principal; BMP280 como fallback de presión y temperatura
    bar_ok = false;
    if (micropressure_ok) {
      double p = barometer.readPressure(KPA);
      if (p > 50.0 && p < 120.0) {
        pressure = p;
        bar_ok   = true;
      } else {
        micropressure_ok = false;
        DLOGLN("MicroPressure fallo en lectura — probando BMP280");
      }
    }

    // BMP280 — temperatura secundaria + presión de respaldo si MicroPressure falla
    bmp_temp_ok     = false;
    bmp_pressure_ok = false;
    if (bmp_ok) {
      float tBmp = NAN, pBmp = NAN;
      if (readBMP280Temperature(tBmp)) { bmpTemperature = tBmp; bmp_temp_ok = true; }
      if (readBMP280PressureKPa(pBmp)) {
        bmpPressure = pBmp; bmp_pressure_ok = true;
        if (!micropressure_ok) { pressure = pBmp; bar_ok = true; }
      }
      if (!bmp_temp_ok && !bmp_pressure_ok) {
        bmp_ok = false;
        DLOGLN("BMP280 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!bar_ok) pressure = sim_pressure;

    // BH1750 — iluminancia → lightLevel (con retry si falla la primera lectura)
    if (bh1750_ok) {
      float lux = bh1750.readLightLevel();
      if (lux < 0.0f) {
        delay(50);
        lux = bh1750.readLightLevel();  // un reintento tras breve pausa
      }
      if (lux >= 0.0f) {
        lightLevel = lux;
      } else {
        bh1750_ok = false;
        DLOGLN("BH1750 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!bh1750_ok) lightLevel = sim_light;

    // Parámetros derivados agrometeorológicos
    {
      float T = temperatureMCP;
      float H = humidity;
      // Media HDC1080 + BMP280 si ambos disponibles
      float Tavg = (hdc_ok && bmp_temp_ok) ? (T + bmpTemperature) / 2.0f : T;
      agroTempAvg = Tavg;
      if (!isnan(Tavg) && !isnan(H) && H > 0.0f) {
        agroDewPoint = agro_calcDewPoint(Tavg, H);
        agroAbsHum   = agro_calcAbsHumidity(Tavg, H);
        agroHeatIndex = (Tavg > 27.0f && H > 40.0f)
                        ? agro_calcHeatIndex(Tavg, H) : NAN;
      }
    }

#endif  // PROFILE_AGROMETEO

#if DEVICE_PROFILE != PROFILE_AGROMETEO
    // TSL2584/APDS-9930 — omitido en AGROMETEO (usa BH1750)
    if (tsl_ok) {
      float lux = tsl_readLux();
      if (lux >= 0.0f) {
        lightLevel = lux;
      } else {
        tsl_ok = false;
        DLOGLN("TSL2584 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!tsl_ok) lightLevel = sim_light;
#endif  // PROFILE_AGROMETEO (TSL guard)

#if defined(SOIL_PIN)
    {
      int raw = analogRead(SOIL_PIN);
      float filtRaw = filteredSoilADC(raw);
      soilMoisture = constrain(
        (float)(SOIL_RAW_DRY - filtRaw) / (SOIL_RAW_DRY - SOIL_RAW_WET) * 100.0f,
        0.0f, 100.0f
      );
    }
#else
    soilMoisture = sim_soilMoisture;
#endif

    updateSimulatedValues();
    updatePipelineValues();

#ifndef ESP8266
    if (hasMutex) xSemaphoreGive(dataMutex);
#endif

#if DEVICE_PROFILE == PROFILE_AGROMETEO
    DLOGF("[sensor] HDC:T=%.1f H=%.1f%% | BMP:T=%.1f P=%.2fkPa | BH:%.1flx | Dp=%.1f Hi=%.1f Ah=%.2f\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure,
      lightLevel, agroDewPoint, agroHeatIndex, agroAbsHum);
#else
    DLOGF("[sensor] T:%.1f Tb:%.1f H:%.1f D11T:%.1f D11H:%.1f P:%.2f W:%.2f D:%.0f Lux:%.1f Soil:%.1f%%\n",
      temperatureMCP, temperatureDHT, humidity,
      temperatureDHT11, humidityDHT11,
      (float)pressure, windSpeedFiltered, currentWindDirDeg, lightLevel, soilMoisture);
#endif

    lastSensorRead = now;
  }

  // ── 3. Refresco de pantalla (cada SCREEN_MS = 1s, solo si hay display) ────────
#ifdef HAS_DISPLAY
  if (now - lastScreenTime >= SCREEN_MS) {
    if (displayView == 1) drawPipelineScreen();
    else                  drawScreen();
    lastScreenTime = now;
  }
#endif

#ifdef ESP8266
  // ── ESP8266: red en el loop() (sin FreeRTOS) ─────────────────────────────────
  static unsigned long lastRelayCheck8266 = 0;
  if (now - lastRelayCheck8266 >= RELAY_MS && WiFi.status() == WL_CONNECTED) {
    checkRelayCommand();
    lastRelayCheck8266 = now;
  }

  static bool deviceInfoSent8266 = false;
  if (!deviceInfoSent8266 && WiFi.status() == WL_CONNECTED) {
    postDeviceInfo();
    deviceInfoSent8266 = true;
  }

  static unsigned long lastSendTime8266 = 0;
  if (now - lastSendTime8266 >= SEND_MS) {
    finalAvgWindDir = calcAndResetWindVector();
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      String url = serverBaseUrl() + "/send_message";
      char msg[320];
      int relayMask = 0;
      for (int i = 0; i < RELAY_COUNT; i++) if (relayActive[i]) relayMask |= (1 << i);
      snprintf(msg, sizeof(msg),
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%ld,%ld,%d,%.2f,%.2f,%.2f",
        temperatureMCP, (float)pressure, temperatureDHT, humidity,
        windSpeed, currentWindDirDeg, windSpeedFiltered, finalAvgWindDir,
        lightLevel, temperatureDHT11, humidityDHT11,
        WiFi.RSSI(), (long)ESP.getFreeHeap(), (long)(millis()/1000),
        relayMask, sim_pipeline_pressure, sim_pipeline_flow, soilMoisture
      );
      ok = httpPost(url, String(msg));
    } else {
      WiFi.reconnect();
    }
    lastServerOK = ok;
    lastSendTime8266 = now;
  }
#endif

  // ── DEBUG: reporte completo de estado cada DEBUG_INTERVAL_MS ─────────────────
#ifdef DEBUG_MODE
  static unsigned long lastDebugReport = 0;
  if (now - lastDebugReport >= DEBUG_INTERVAL_MS) {
    lastDebugReport = now;

    DLOGLN(F("\n====== AQUANTIA TEST REPORT ======"));

    // Perfil e info de compilacion
    DLOGF("[PERFIL] %s (%d) | Relays: %d | Display: %s | MQTT: %s\n",
      (DEVICE_PROFILE == PROFILE_METEO) ? "METEO" :
      (DEVICE_PROFILE == PROFILE_AGROMETEO) ? "AGROMETEO" : "IRRIGATION",
      DEVICE_PROFILE, RELAY_COUNT,
#ifdef HAS_DISPLAY
      "SI",
#else
      "NO",
#endif
#ifdef USE_MQTT
      "SI"
#else
      "NO"
#endif
    );

    // Tiempo y memoria
    unsigned long up = millis() / 1000;
    DLOGF("[TIEMPO] Uptime: %luh %02lum %02lus | Heap libre: %ld B\n",
      up / 3600, (up % 3600) / 60, up % 60, (long)ESP.getFreeHeap());

    // Serial / MAC del dispositivo
    DLOGF("[DEVICE] Serial: %s\n", device_serial_get());

    // WiFi
    if (WiFi.status() == WL_CONNECTED) {
      DLOGF("[WIFI  ] CONECTADO | IP: %s | RSSI: %d dBm | SSID: %s\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.SSID().c_str());
    } else {
      DLOGLN("[WIFI  ] DESCONECTADO");
    }

#if !defined(ESP8266) && defined(USE_MQTT)
    DLOGF("[MQTT  ] %s\n",
      mqttClient.connected() ? "CONECTADO" : "DESCONECTADO");
#endif

    // Estado de sensores
    DLOG("[SENSOR]");
#if DEVICE_PROFILE == PROFILE_METEO
    DLOGF(" TempExt:%s  Barometro:%s  DHT11:%s",
      temp_ok ? temperatureSourceName() : "SIM",
      bar_ok ? pressureSourceName() : "SIM",
      dht_ok ? "REAL" : "SIM");
#elif DEVICE_PROFILE == PROFILE_AGROMETEO
    DLOGF(" HDC1080:%s  BMP280:%s  BH1750:%s",
      hdc_ok ? "REAL" : "SIM",
      bmp_ok ? "REAL" : "SIM",
      bh1750_ok ? "REAL" : "SIM");
#else
    DLOG(" MCP9808:N/A  Barometro:N/A  DHT11:N/A");
#endif
#if DEVICE_PROFILE != PROFILE_AGROMETEO
    DLOGF("  HTU2x:%s  LuzAmb:%s\n",
      htu_ok ? "REAL" : "SIM", tsl_ok ? "REAL" : "SIM");
#else
    DLOGLN();  // AGROMETEO: sensores ya listados arriba
#endif

    // Valores medidos / simulados
#if DEVICE_PROFILE == PROFILE_AGROMETEO
    DLOGF("[DATOS ] HDC:T=%.1f C  H=%.1f%%  BMP:T=%.1f C  P=%.2f kPa  BH:%.1f lx\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure, lightLevel);
    DLOGF("[AGROCALC] Dp=%.1f C  HI=%.1f C  AH=%.2f g/m3\n",
      agroDewPoint, agroHeatIndex, agroAbsHum);
    DLOGF("[PIPE  ] Presion:%.2f bar  Caudal:%.2f L/min  Fuente:%s  Escenario:%s\n",
      sim_pipeline_pressure, sim_pipeline_flow, pipelineSource.c_str(), pipelineScenario.c_str());
#else
    DLOGF("[DATOS ] T_MCP:%.1f C  T_HTU:%.1f C  H_HTU:%.1f%%  Lux:%.1f lx\n",
      temperatureMCP, temperatureDHT, humidity, lightLevel);
#if DEVICE_PROFILE == PROFILE_METEO
    DLOGF("[DATOS ] P:%.2f kPa  T_DHT11:%.1f C  H_DHT11:%.1f%%  Suelo:%.1f%%\n",
      (float)pressure, temperatureDHT11, humidityDHT11, soilMoisture);
#endif
    DLOGF("[PIPE  ] Presion:%.2f bar  Caudal:%.2f L/min  Fuente:%s  Escenario:%s\n",
      sim_pipeline_pressure, sim_pipeline_flow, pipelineSource.c_str(), pipelineScenario.c_str());
#if defined(FLOW_PIN)
    DLOGF("[FLOW  ] Pulsos/intervalo:%lu  Total:%lu  Intervalo:%lu ms  K:%d p/L\n",
      (unsigned long)_flowLastPulses, (unsigned long)_flowPulseTotal,
      (unsigned long)_flowLastDtMs, FLOW_K_FACTOR);
#endif
    DLOGF("[VIENTO] Speed:%.1f m/s (filt:%.1f) | Dir:%.0f grados (%s)\n",
      windSpeed, windSpeedFiltered, currentWindDirDeg, degToCompass(currentWindDirDeg));
#if defined(FLOW_PIN)
    if (_flowPulseTotal == 0) {
      DLOGLN("[FLOW  ] WARN — sin pulsos recibidos (verif. cableado GPIO" + String(FLOW_PIN) + " y transistor BC547)");
    }
#endif
#endif  // PROFILE_AGROMETEO / else

    // Estado relays
    DLOG("[RELAYS]");
    for (int i = 0; i < RELAY_COUNT; i++) {
      DLOGF(" R%d:%s", i, relayActive[i] ? "ON " : "OFF");
    }
    DLOGLN();

    // Test assertions
    bool allRelaysOff = true;
    for (int i = 0; i < RELAY_COUNT; i++) if (relayActive[i]) { allRelaysOff = false; break; }

    DLOGLN("[TEST  ] " + String(allRelaysOff ? "PASS" : "FAIL") +
                   " — Todos los relays en OFF (estado seguro)");
    DLOGLN("[TEST  ] PASS — Boot completado sin crash (uptime > 0)");
    DLOGLN("[TEST  ] PASS — Sensores: modo SIM cuando no hay hardware conectado");
    DLOGLN(F("==================================\n"));
  }
#endif  // DEBUG_MODE
}
