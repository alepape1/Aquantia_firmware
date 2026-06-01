// MeteoStation — Firmware v3
// ESP32 (con pantalla ST7789 240×135)
// Sensores: MCP9808, BMP280, HTU2x, SparkFun MicroPressure, TSL2584/APDS, anemómetro, veleta
// Tres temporizadores independientes: 100ms viento / 1s pantalla / 20s envío

// ── Versión del firmware ───────────────────────────────────────────────────────
// Incrementar según SemVer al crear un release. El backend almacena este valor
// en device_info.firmware_version para mostrar en el dashboard y detectar
// dispositivos desactualizados (comparado con app_settings.min_firmware_version).
#define FIRMWARE_VERSION "0.2.0-beta.3"

// ── Perfiles de dispositivo — deben ir PRIMERO para que los #if funcionen ─────
#define PROFILE_METEO       1   // ECU meteorológica — 1 relay (GPIO RELAY_PIN)
#define PROFILE_IRRIGATION  2   // ECU irrigación   — 4 relays (GPIOs RELAY_PIN_1..4)
#define PROFILE_AQUALEAK          3   // ECU AquaLeak — 1 relay para válvula, sensores CJMCU-14 (BH1750+HDC1080+BMP280)
#define PROFILE_AQUA_SMART_REMOTE 4   // ECU remota — LilyGO T-SIM7000G + SIM Onomondo (idéntico a IRRIGATION + celular)

#ifndef DEVICE_PROFILE
  #define DEVICE_PROFILE PROFILE_METEO
#endif

// ── Modo debug — pasar -DDEBUG_MODE=1 al compilador para activarlo ───────────
// En producción NO definir: elimina toda la salida serie y reduce consumo.
// El Flash Tool GUI tiene la casilla "Debug" que inyecta el flag automáticamente.
#define DEBUG_INTERVAL_MS 10000UL   // reporte completo cada 10 s

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

// ── Plataforma: ESP32 ─────────────────────────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // AQUA_SMART_REMOTE usa conectividad celular — sin WiFi
  #define TINY_GSM_MODEM_SIM7000SSL
  #include <TinyGsmClient.h>
#else
  #include "WiFi.h"
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
  #include <ArduinoOTA.h>
#endif
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
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
  // K factor según modelo (nominal):
  //   YF-S201 → 450 p/L  (F = 7.5·Q Hz)
  //   YF-B4   → 240 p/L  (F = 4.0·Q Hz)  ← nominal datasheet
  //   YF-B9   → 288 p/L  (F = 4.8·Q Hz)
  // CALIBRADO empíricamente: 5 L en 6m32s (392 s) → 3300 pulsos contados → K = 660 p/L
  // El valor nominal YF-B4 (240) discrepa probablemente por variación de fabricante
  // y/o el trigger de ISR contando ambos flancos con el circuito BC547 NPN.
  #define FLOW_K_FACTOR   660   // YF-B4 calibrado — 660 p/L (medido: 5 L en 392 s)
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  #define FLOW_PIN         17   // Caudalímetro — GPIO17 en WEMOS D1 MINI ESP32 (sin función especial, soporta ISR)
  // K factor según modelo:
  //   YF-S201 → 450 p/L  (F = 7.5·Q Hz)
  //   YF-B4   → 240 p/L  (F = 4.0·Q Hz)
  //   YF-B9   → 288 p/L  (F = 4.8·Q Hz)
  #define FLOW_K_FACTOR   288   // YF-B9 calibrado — 288 p/L (medido: ~9 L / 2678 pulsos)
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  #define FLOW_PIN         34   // Caudalímetro — GPIO34, ADC1_CH6, solo entrada (no usar como salida)
  // K factor según modelo (sin calibrar — ajustar empíricamente con el caudalímetro instalado):
  //   YF-S201 → 450 p/L  (F = 7.5·Q Hz)
  //   YF-B4   → 240 p/L  (F = 4.0·Q Hz)  ← nominal datasheet
  //   YF-B9   → 288 p/L  (F = 4.8·Q Hz)
  #define FLOW_K_FACTOR   660   // YF-B4 — usar mismo valor calibrado que METEO hasta nueva calibración
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  #define FLOW_PIN         34   // Caudalímetro — GPIO34, ADC1_CH6, solo entrada
  #define FLOW_K_FACTOR   660   // YF-B4 — mismo valor que IRRIGATION hasta calibración propia
#endif
#if DEVICE_PROFILE == PROFILE_METEO
  #define HAS_DISPLAY
#endif
#define RELAY_PIN        26   // GPIO libre para relay electroválvula

// Sensor presión tubería I2C (familia XGZP6847D / XDB401 digital)
// Configuración en pressure_sensor_i2c.h: PRESSURE_SENSOR_I2C_ADDR y PRESSURE_SENSOR_FULLSCALE

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
  #include "SoilSensor.h"
  #include "halisense_sensor.h"
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  // IRRIGATION: RS485 Helissense + INA219 + AHT20 + BMP280
  #include <Wire.h>
  #include <Adafruit_BMP280.h>
  #include "SoilSensor.h"
  #include "halisense_sensor.h"
  #include "aht20_driver.h"
  #include "ina219_driver.h"
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // AQUA_SMART_REMOTE: mismo sensor suite que IRRIGATION + conectividad celular SIM7000G
  #include <Wire.h>
  #include <Adafruit_BMP280.h>
  #include "SoilSensor.h"
  #include "halisense_sensor.h"
  #include "aht20_driver.h"
  #include "ina219_driver.h"
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
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

// Certificado TLS reutilizado por HTTPS y MQTT.
#include "mqtt_cert.h"

// PubSubClient — solo cuando USE_MQTT está definido
// IMPORTANTE: este include debe ir DESPUÉS de secrets.h para que USE_MQTT esté definido
#include <time.h>

#if defined(USE_MQTT)
  #include <PubSubClient.h>
#endif

// Provisioning: SoftAP captive portal + NVS
#include "provisioning.h"
#include "LeakDetector.h"

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
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // LilyGO T-SIM7000G — pines libres (sin conflicto con modem UART ni SD card)
  #define RELAY_COUNT 4
  #define RELAY_PIN_1  32
  #define RELAY_PIN_2  33
  #define RELAY_PIN_3  16
  #define RELAY_PIN_4  17
  static const uint8_t RELAY_PINS[RELAY_COUNT] = {32, 33, 16, 17};
  // ── Modem SIM7000G ──────────────────────────────────────────────────────────
  #define MODEM_TX_PIN      27   // Serial1 AT → modem
  #define MODEM_RX_PIN      26   // Serial1 AT ← modem
  #define MODEM_DTR_PIN     25   // Mantiene modem despierto (LOW)
  #define MODEM_PWRKEY_PIN   4   // Secuencia power-on (pulso 1000 ms)
  #define MODEM_LED_PIN     12   // LED onboard (activo-LOW)
  // ── RS485 Helissense — pines libres, SD card intacta (2/13/14/15) ─────────
  #define ASR_RS485_RX_PIN  18   // GPIO libre
  #define ASR_RS485_TX_PIN  19   // GPIO libre
  #define ASR_RS485_DERE    23   // GPIO libre
  // ── ADC — tensión batería y panel solar ────────────────────────────────────
  #define BOARD_BAT_ADC_PIN   35
  #define BOARD_SOLAR_ADC_PIN 36
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  // AQUALEAK: 1 relay para válvula de corte electroválvula — GPIO RELAY_PIN (Wemos D1 Mini ESP32)
  #define RELAY_COUNT 1
  static const uint8_t RELAY_PINS[RELAY_COUNT] = {RELAY_PIN};
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
//   AQUALEAK   — Wemos D1 Mini ESP32   : GPIO 2
//   IRRIGATION — ESP32 4-Relay Board   : GPIO 23
#if DEVICE_PROFILE == PROFILE_METEO
  const int ledPin = -1;   // LilyGo T-Display no tiene LED onboard
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  const int ledPin = 23;
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  const int ledPin = MODEM_LED_PIN;   // GPIO12, activo-LOW
#else
  const int ledPin = 2;
#endif
#define LED_ON  HIGH
#define LED_OFF LOW

#include "led_control.h"


// ── Intervalos ─────────────────────────────────────────────────────────────────
#define WIND_MS          100
#define SCREEN_MS       1000
#define SEND_MS         2000
#define RELAY_MS        2000   // Consulta estado relay cada 2s para respuesta casi inmediata
#define PIPELINE_SYNC_MS 20000UL
// Intervalo mínimo entre lecturas del XDB401 + caudalímetro.
// El sensor tarda ~120 ms por lectura (60 ms adquisición + 2 polls de 30 ms).
// 200 ms deja ~80 ms de margen entre lecturas → tasa máxima real ~5 Hz.
// No tiene sentido bajar más: el sensor completo necesita 60 ms mínimo según datasheet.
#define PIPELINE_FAST_MS  200UL

// Intervalos de muestreo del sensor de suelo RS485 — configurables en runtime vía MQTT/HTTP
#define SOIL_POST_IRRIG_MS 120000UL   // ventana de absorción tras apagar riego (2 min)

#ifdef HAS_DISPLAY
#define DISPLAY_TIMEOUT_MS 600000UL  // Apagar pantalla tras 10 minutos sin actividad
#define BTN_LEFT   0                // Botón izquierdo (BOOT), INPUT_PULLUP, activo LOW
#define BTN_RIGHT 35                // Botón derecho, activo LOW
#endif

// ── Objetos ────────────────────────────────────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_METEO
  SparkFun_MicroPressure barometer;
  Adafruit_MCP9808       tempsensor = Adafruit_MCP9808();
  Adafruit_BMP280        bmp280;
  DHTesp                 dht;
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  Adafruit_BMP280        bmp280;
  BH1750                 bh1750;
  QWIIC_POWER            qwiic_ps;
  SparkFun_MicroPressure barometer;
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  Adafruit_BMP280        bmp280;
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  Adafruit_BMP280        bmp280;
#endif

#ifdef HAS_DISPLAY
  TFT_eSPI    tft = TFT_eSPI();
  TFT_eSprite spr = TFT_eSprite(&tft);
  unsigned long lastActivityTime = 0;
  bool          displayOn        = true;
  uint8_t       displayView      = 0;   // 0 = meteo, 1 = pipeline, 2 = info
#endif

// ── Info de arranque (reset reason + timestamp de primera conexión) ────────────
#include "esp_system.h"
static char g_rebootReason[40]    = "desconocido";
static char g_lastConnectStr[20]  = "--:--:-- --/--/--";  // HH:MM:SS DD/MM/YY

// Última tarea activa antes de un WDT reset — sobrevive al reset en RTC RAM
RTC_NOINIT_ATTR static char g_wdtLastTask[16];
RTC_NOINIT_ATTR static uint32_t g_wdtMagic;  // 0xDEAD1234 → g_wdtLastTask válido
static constexpr uint32_t WDT_MAGIC = 0xDEAD1234;

// Llamar antes de operaciones bloqueantes para registrar tarea+fase en RTC RAM.
// Si el WDT dispara durante esa operación, el reboot alert incluirá ambos datos.
static inline void wdt_heartbeat(const char* taskName, const char* phase = nullptr) {
  char buf[16];
  if (phase) {
    snprintf(buf, sizeof(buf), "%s/%s", taskName, phase);
    strlcpy(g_wdtLastTask, buf, sizeof(g_wdtLastTask));
  } else {
    strlcpy(g_wdtLastTask, taskName, sizeof(g_wdtLastTask));
  }
  g_wdtMagic = WDT_MAGIC;
}

static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "encendido";
    case ESP_RST_SW:        return "reinicio SW";
    case ESP_RST_PANIC:     return "panic/crash";
    case ESP_RST_INT_WDT:   return "WDT interrup.";
    case ESP_RST_TASK_WDT:  return "WDT tarea";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_EXT:       return "reset externo";
    default:                return "desconocido";
  }
}

// ── Relay electroválvula(s) ────────────────────────────────────────────────────
// Relay activo-HIGH: HIGH = relay ON (válvula abierta), LOW = relay OFF
// relayActive[i] — estado actual de cada relay (índice = bit en bitmask)
// Protección: si RELAY_COUNT es 0 (algún perfil futuro sin relay) evitamos array de tamaño 0
bool relayActive[RELAY_COUNT > 0 ? RELAY_COUNT : 1] = {};

#ifdef USE_MQTT
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static TinyGsm             modemSIM(Serial1);
static TinyGsmClientSecure gsmTLSClient(modemSIM);
static TinyGsmClient       gsmTCPClient(modemSIM);
#else
static WiFiClient       mqttTCPClient;
static WiFiClientSecure mqttTLSClient;
#endif
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
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
bool hdc_ok        = false;   // HDC1080 — temperatura y humedad primaria
bool bh1750_ok     = false;   // BH1750  — iluminancia
bool qwiic_ps_ok   = false;   // Qwiic Power Switch (PCA9536) — alimenta el bus de sensores
static uint8_t bmp280_addr = 0x00;
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
bool aht20_ok  = false;   // AHT20  — temperatura y humedad ambiente (I2C 0x38)
bool ina219_ok = false;   // INA219 — voltaje / corriente / potencia  (I2C 0x40)
static uint8_t bmp280_addr = 0x00;
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
bool aht20_ok  = false;   // AHT20  — temperatura y humedad ambiente (I2C 0x38)
bool ina219_ok = false;   // INA219 — voltaje / corriente / potencia  (I2C 0x40)
static uint8_t bmp280_addr = 0x00;
#endif
bool tsl_ok   = false;
bool xdb401_ok = false;   // XDB401 — sensor de presión de tubería I2C
static uint8_t       xdb401_failures  = 0;       // fallos consecutivos de lectura
static unsigned long xdb401_retry_at  = 0;        // millis() cuando intentar reinit
static constexpr uint8_t  XDB401_MAX_FAILURES  = 8;    // más tolerante con cable largo (~1 m)
static constexpr uint32_t XDB401_RETRY_INTERVAL = 15000UL;  // reintento más rápido tras recovery

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK \
 || DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
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
#endif  // PROFILE_METEO || PROFILE_AQUALEAK

#include "hdc1080_driver.h"

#include "pressure_sensor_i2c.h"

// Detecta el sensor en el bus I2C ya inicializado por setup().
// Registra los pines en el driver (para recovery futuro) y fuerza
// PRESSURE_SENSOR_I2C_FREQ_HZ (50 kHz) para acomodar el cable de ~1 m.
// En el 2.º y 3.er intento ejecuta bus recovery (9 pulsos SCL + STOP)
// antes de volver a intentar la detección.
static bool xdb401_begin() {
  // Init del driver: almacena I2C_SDA/I2C_SCL. Pasa 100000 como _normal_freq
  // para que el driver restaure 100 kHz al terminar cada transacción.
  pressureSensor_init(I2C_SDA, I2C_SCL, 100000);

  for (int i = 0; i < 3; i++) {
    if (i > 0) {
      DLOGF("[XDB401] Reintento %d — recuperando bus I2C\n", i);
      pressureSensor_recover();  // 9 pulsos SCL + STOP + Wire.begin a 50 kHz
      delay(30);
    }
    if (pressureSensor_isPresent()) {
      DLOGF("[XDB401] Detectado en 0x%02X (intento %d)\n", PRESSURE_SENSOR_I2C_ADDR, i + 1);
      return true;
    }
  }
  return false;
}

// Lee presión (bar) y temperatura (°C) usando el driver pressure_sensor_i2c.
// Devuelve true si la lectura es válida; pressureBar y temperatureC = NAN si falla.
static bool xdb401_read(float& pressureBar, float& temperatureC) {
  pressureBar  = NAN;
  temperatureC = NAN;

  PressureSensorData_t data;
  if (!pressureSensor_read(&data) || !data.valid) {
    DLOGLN("[XDB401] Read FAIL");
    return false;
  }

  float pb = data.pressure_kpa / 100.0f;  // kPa → bar
  float tc = data.temperature_c;

  static unsigned long _lastXdbPrint = 0;
  unsigned long _now = millis();
  if (_now - _lastXdbPrint >= DEBUG_INTERVAL_MS) {
    _lastXdbPrint = _now;
    DLOGF("[XDB401] Presion=%.3f bar  Temp=%.1f C\n", pb, tc);
  }

  float fs_bar = PRESSURE_SENSOR_FULLSCALE / 100.0f;
  bool ok = (pb >= -0.5f && pb <= fs_bar * 1.05f
             && tc > -10.0f && tc < 125.0f);
  if (!ok) {
    DLOGF("[XDB401] Validacion FAIL — pb=%.3f bar (max=%.1f) tc=%.1f C\n", pb, fs_bar, tc);
    return false;
  }

  pressureBar  = pb;
  temperatureC = tc;
  return true;
}

// Wrapper conveniente: solo presión en bar (NAN si falla).
static float xdb401_readPressureBar() {
  float pb, tc;
  return xdb401_read(pb, tc) ? pb : NAN;
}

static const char* temperatureSourceName() {
#if DEVICE_PROFILE == PROFILE_METEO
  if (mcp_ok) return "MCP9808";
  if (bmp_temp_ok) return "BMP280";
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  if (hdc_ok) return "HDC1080";
  if (bmp_temp_ok) return "BMP280";
#elif DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  if (aht20_ok) return "AHT20";
  if (bmp_temp_ok) return "BMP280";
#endif
  return "SIM";
}

static const char* pressureSourceName() {
  if (xdb401_ok) return "XDB401";
#if DEVICE_PROFILE == PROFILE_METEO
  if (micropressure_ok) return "MicroPressure";
  if (bmp_pressure_ok) return "BMP280";
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  if (micropressure_ok) return "MicroPressure";
  if (bmp_pressure_ok)  return "BMP280";
#elif DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  if (bmp_pressure_ok) return "BMP280";
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
#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
float  ina219BusVoltage = NAN;  // INA219 voltaje bus (V)
float  ina219Current    = NAN;  // INA219 corriente (mA)
float  ina219Power      = NAN;  // INA219 potencia (mW)
#endif
float  windSpeed         = 0;
float  windSpeedFiltered = 0;
float  currentWindDirDeg = 0;
float  lightLevel        = 0;
float  soilMoisture      = 0;   // humedad suelo (0=seco, 100=saturado)
#if DEVICE_PROFILE == PROFILE_METEO
HalisenseData halisenseData = {};
SoilSensor    soilSensor(Serial2, 13, 17, 27);  // RX=GPIO13, TX=GPIO17, DE/RE=GPIO27 (GPIO16=TFT_DC, no usar)
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
HalisenseData halisenseData = {};
SoilSensor    soilSensor(Serial2, 14, 13, 27);  // RX=GPIO14, TX=GPIO13, DE/RE=GPIO27
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
HalisenseData halisenseData = {};
SoilSensor    soilSensor(Serial2, ASR_RS485_RX_PIN, ASR_RS485_TX_PIN, ASR_RS485_DERE);  // RX=18, TX=19, DE/RE=23
#endif

// ── Parámetros calculados AQUALEAK ────────────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_AQUALEAK
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
static portMUX_TYPE      _flowMux         = portMUX_INITIALIZER_UNLOCKED;  // protección ISR ↔ Core1
static volatile uint32_t _flowPulseCount  = 0;     // contador crudo (escrito solo por ISR, se resetea cada intervalo)
static volatile uint32_t _flowPulseTotal  = 0;     // acumulador histórico — nunca se resetea
static volatile uint32_t _flowSessionBase = 0;     // base de pulsos para conteo de sesión
static unsigned long     _flowLastCalcUs  = 0;     // marca de tiempo última lectura (micros)
static unsigned long     _flowLastDtUs    = 0;     // duración del último intervalo de cálculo (µs)
static uint32_t          _flowLastPulses  = 0;     // pulsos contados en el último intervalo
static float             _flowLpm         = 0.0f;  // último caudal calculado (L/min)

void IRAM_ATTR flowPulseISR() {
  _flowPulseCount++;
  _flowPulseTotal++;
}

// Reinicia el contador de sesión (litros desde apertura de válvula).
// Llamar cuando el relay abre la válvula (transition OFF→ON).
void flowSessionReset() {
  portENTER_CRITICAL(&_flowMux);
  _flowSessionBase = _flowPulseTotal;
  portEXIT_CRITICAL(&_flowMux);
}
#endif

// ── Pipeline simulado / hardware-ready ───────────────────────────────────────
// char[16] en lugar de String evita races de heap entre Core 0 y Core 1.
// Escrituras protegidas con dataMutex; lecturas eventual-consistent (máx 1 ciclo).
char pipelineScenario[16]    = "normal";    // normal | leak | burst | obstruction
// Si FLOW_PIN está definido arrancamos en "real" directamente; de lo contrario "sim".
// El backend puede cambiar este valor en runtime via MQTT pipeline_config o HTTP /api/pipeline/config.
#if defined(FLOW_PIN)
char pipelineMode[16]        = "real";      // sim | real
#else
char pipelineMode[16]        = "sim";       // sim | real
#endif
String pipelineSource        = "sim";       // sim | real | real_flow | fallback
bool   pipelinePressureOk    = false;
bool   pipelineFlowOk        = false;
float  sim_pipeline_pressure = PIPELINE_STATIC_P;
float  sim_pipeline_flow     = 0.0f;
float  xdb401Temperature     = NAN;  // temperatura interna del sensor de presión (XDB401)
#if defined(FLOW_PIN)
float    g_flowSessionL      = 0.0f; // litros desde última apertura de válvula — actualizado en pipeline tick (Core 1)
// ── Contadores de pulsos por tipo de flujo ────────────────────────────────────
// Acumulamos pulsos (uint32_t) en lugar de litros (float) para evitar pérdida
// de precisión al sumar incrementos pequeños a un acumulador grande.
// Overflow de uint32_t a K=660: ~4.3e9 / 660 ≈ 6.5 millones de litros → inaceptable
// en campo, pero overflow aritmético uint32_t es predecible y se puede wrap con
// la misma técnica que _flowPulseTotal (diferencia siempre correcta si el sistema
// no acumula más de 4.3e9 pulsos entre lecturas).
static volatile uint32_t _flowIrrigPulses = 0; // pulsos con relay ON  (riego)
static volatile uint32_t _flowLeakPulses  = 0; // pulsos con relay OFF (fuga)
#endif
IrrigationType irrigationType = IRRIG_SPRINKLER;  // perfil de riego activo (configurable vía MQTT/HTTP)
LeakDetector   leakDetector;                       // detector automático de fugas (solo modo real)
unsigned long telemetryIntervalMs  = 20000UL;
unsigned long configSyncIntervalMs = PIPELINE_SYNC_MS;
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
unsigned long soilFastIntervalMs   = 3000UL;    // durante riego activo y ventana post-riego
unsigned long soilSlowIntervalMs   = 20000UL;   // reposo normal
#endif
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

  if (strcmp(pipelineScenario, "burst") == 0) {
    sim_pipeline_pressure = max(0.0f, 0.25f + p_noise * 0.4f);
    sim_pipeline_flow     = valveOpen
      ? max(0.0f, PIPELINE_NOMINAL_Q * 0.08f + fabsf(q_noise) * 0.3f)
      : 0.0f;

  } else if (strcmp(pipelineScenario, "obstruction") == 0) {
    if (valveOpen) {
      // Tubería bloqueada: presión no cae (queda cerca de la estática), caudal ~0
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise * 0.5f);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.04f);
    } else {
      // Válvula cerrada: igual que escenario normal
      sim_pipeline_pressure = max(0.0f, PIPELINE_STATIC_P + p_noise);
      sim_pipeline_flow     = max(0.0f, fabsf(q_noise) * 0.05f);
    }

  } else if (strcmp(pipelineScenario, "leak") == 0) {
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
  // pressureBar = NAN indica "sin sensor de presión en este ciclo" → el caller
  // mantiene la estimación simulada para la presión.
  unsigned long now = micros();
  unsigned long dt  = now - _flowLastCalcUs;

  if (dt < 500000UL) {  // < 500 ms en microsegundos
    // Intervalo demasiado corto para recalcular caudal — reutilizar último valor.
    // Pero SÍ leemos el XDB401: presión y caudal son independientes; sin esto
    // el caller alterna entre valor real y simulado cada 2-3 ciclos de 200 ms.
    flowLpm = _flowLpm;
    pressureBar = NAN;
    if (!xdb401_ok && millis() >= xdb401_retry_at) {
      // Exigir lectura exitosa antes de declarar el sensor recuperado.
      // Evita el ciclo begin→ok→8 lecturas fallidas→not-ok que genera alertas repetidas
      // cuando la sonda responde al ping I2C pero no devuelve datos válidos.
      if (xdb401_begin()) {
        float _p0, _t0;
        if (xdb401_read(_p0, _t0)) {
          xdb401_ok         = true;
          xdb401_failures   = 0;
          pressureBar       = _p0;
          xdb401Temperature = _t0;
          DLOGLN("[XDB401] Reconectado tras fallo");
        }
      }
      if (!xdb401_ok) xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
    }
    if (xdb401_ok) {
      float p, tc;
      if (xdb401_read(p, tc)) {
        pressureBar       = p;
        xdb401Temperature = tc;
        xdb401_failures   = 0;
      } else if (++xdb401_failures >= XDB401_MAX_FAILURES) {
        xdb401_ok       = false;
        xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
        DLOGF("[XDB401] %u fallos consecutivos — suspendido %lus\n",
              (unsigned)XDB401_MAX_FAILURES, (unsigned long)XDB401_RETRY_INTERVAL / 1000);
      }
    }
    return true;
  }

  noInterrupts();
  uint32_t pulses  = _flowPulseCount;
  _flowPulseCount  = 0;
  interrupts();

  _flowLastCalcUs = now;
  _flowLastDtUs   = dt;       // guardamos para el debug
  _flowLastPulses = pulses;   // guardamos para el debug

  // L/min = (pulsos / dt_s) * (60.0 / K_FACTOR)
  float dt_s  = dt * 1e-6f;  // µs → s (precisión µs reduce error de cuantización a caudales bajos)
  _flowLpm    = (pulses * 60.0f) / (dt_s * (float)FLOW_K_FACTOR);
  flowLpm     = _flowLpm;
  // Acumular pulsos del intervalo en riego o fuga según estado del relay.
  // Usamos contadores de pulsos uint32_t para evitar pérdida de precisión float.
  if (anyRelayActive()) _flowIrrigPulses += pulses;
  else                  _flowLeakPulses  += pulses;
  // Actualizar litros de sesión visibles por la pantalla (Core 1 → no mutex, noInterrupts suficiente)
  noInterrupts();
  uint32_t _fTot  = _flowPulseTotal;
  uint32_t _fBase = _flowSessionBase;
  interrupts();
  g_flowSessionL = (_fTot - _fBase) / (float)FLOW_K_FACTOR;
  // Intentar leer presión real del XDB401; NAN si no disponible (el caller usará sim)
  // Usar NAN como sentinel permite que lecturas negativas reales (vacío, golpe de ariete) pasen al caller.
  pressureBar = NAN;
  if (!xdb401_ok && millis() >= xdb401_retry_at) {
    if (xdb401_begin()) {
      float _p0, _t0;
      if (xdb401_read(_p0, _t0)) {
        xdb401_ok         = true;
        xdb401_failures   = 0;
        pressureBar       = _p0;
        xdb401Temperature = _t0;
        DLOGLN("[XDB401] Reconectado tras fallo");
      }
    }
    if (!xdb401_ok) xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
  }
  if (xdb401_ok) {
    float p, tc;
    if (xdb401_read(p, tc)) {
      pressureBar       = p;
      xdb401Temperature = tc;
      xdb401_failures   = 0;
    } else if (++xdb401_failures >= XDB401_MAX_FAILURES) {
      xdb401_ok        = false;
      xdb401_retry_at  = millis() + XDB401_RETRY_INTERVAL;
      DLOGF("[XDB401] %u fallos consecutivos — suspendido %lus\n",
            (unsigned)XDB401_MAX_FAILURES, (unsigned long)XDB401_RETRY_INTERVAL / 1000);
    }
  }
  return true;
#else
  // Sin caudalímetro: si hay XDB401 devolvemos presión real con caudal 0
  if (!xdb401_ok && millis() >= xdb401_retry_at) {
    if (xdb401_begin()) {
      float _p0, _t0;
      if (xdb401_read(_p0, _t0)) {
        xdb401_ok         = true;
        xdb401_failures   = 0;
        pressureBar       = _p0;
        xdb401Temperature = _t0;
        DLOGLN("[XDB401] Reconectado tras fallo");
        return true;
      }
    }
    if (!xdb401_ok) xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
  }
  if (xdb401_ok) {
    float p, tc;
    if (xdb401_read(p, tc)) {
      pressureBar       = p;
      xdb401Temperature = tc;
      flowLpm           = 0.0f;
      xdb401_failures   = 0;
      return true;
    } else if (++xdb401_failures >= XDB401_MAX_FAILURES) {
      xdb401_ok        = false;
      xdb401_retry_at  = millis() + XDB401_RETRY_INTERVAL;
      DLOGF("[XDB401] %u fallos consecutivos — suspendido %lus\n",
            (unsigned)XDB401_MAX_FAILURES, (unsigned long)XDB401_RETRY_INTERVAL / 1000);
    }
  }
  (void)pressureBar;
  (void)flowLpm;
  return false;
#endif
}

void updatePipelineValues() {
  // pipelineMode/pipelineScenario son char[16]: sin heap, seguro como eventual-consistent.
  // Escrituras desde Core 0 y Core 1 protegidas con dataMutex; lecturas aquí sin mutex.
  if (strcmp(pipelineMode, "real") == 0) {
    float realPressure = 0.0f;
    float realFlow = 0.0f;
    if (readRealPipelineSensors(realPressure, realFlow)) {
      sim_pipeline_flow = max(0.0f, realFlow);
      pipelineFlowOk    = true;

      if (!isnan(realPressure)) {
        // Sensor de presión real disponible — se permiten valores negativos (vacío, golpe de ariete)
        sim_pipeline_pressure = realPressure;
        pipelinePressureOk    = true;
        pipelineSource        = "real";
      } else {
        // Sin sensor de presión real — estimación por simulador solo para presión.
        // IMPORTANTE: guardamos el caudal real antes y lo restauramos porque
        // updatePipelineSimValues() también sobreescribe sim_pipeline_flow.
        float savedFlow = sim_pipeline_flow;
        updatePipelineSimValues();
        sim_pipeline_flow  = savedFlow;  // restaurar caudal real
        pipelinePressureOk = false;
        pipelineSource     = "real_flow";  // caudal real, presión simulada
      }
      // Detección automática de fugas (EMA baseline; reemplaza pipelineScenario en modo real)
      leakDetector.update(sim_pipeline_pressure, sim_pipeline_flow, anyRelayActive());
      // char[16]: strlcpy es suficiente — Core 0 escribe bajo mutex; lectura eventual-consistent.
      strlcpy(pipelineScenario, leakDetector.scenario(), sizeof(pipelineScenario));
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

#include "htu2x_driver.h"
#include "light_sensor.h"

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
volatile bool        isUpdatingOTA = false;
portMUX_TYPE         windMux       = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t    dataMutex     = nullptr;
QueueHandle_t        telemetryQueue = nullptr;  // Core 1 escribe, Core 0 lee — sin mutex
TaskHandle_t         networkTaskHandle = nullptr;

// ── Snapshot de telemetría — struct global para evitar auto-prototype de Arduino ──
struct TelemetrySnapshot {
  float tempMCP, pressure, tempDHT, humidity;
  float windSpeed, windDir, windSpeedFilt, avgWindDir;
  float light, tempDHT11, humDHT11, soil;
  float bmpTemp, bmpPressure;
  float pipePressure, pipeFlow;
  float flowTotalL;    ///< Litros acumulados desde el arranque (_flowPulseTotal / K). 0 si sin caudalimetro.
  float flowSessionL;  ///< Litros desde la última apertura de válvula (se resetea al abrir relay). 0 si sin caudalimetro.
  float flowIrrigL;    ///< Litros acumulados con relay ON  (riego). 0 si sin caudalimetro.
  float flowLeakL;     ///< Litros acumulados con relay OFF (fuga detectada). 0 si sin caudalimetro.
  float xdb401Temp;   // temperatura interna sensor de presión (XDB401)
  long  heap, uptime;
  int   rssi, relayMask;
#if DEVICE_PROFILE == PROFILE_AQUALEAK
  float dewPoint, heatIndex, absHum;
#endif
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  float soilTemp, soilEc, soilPh, soilTds;
  int   soilN, soilP, soilK;
  bool  halisenseOk;
  bool  soilIrrigMode;  // true si la lectura fue tomada en riego o ventana post-riego
#endif
#if DEVICE_PROFILE == PROFILE_IRRIGATION
  float inaVbus, inaCurrent, inaPower;  // INA219
#endif
} _netSnap;

// ── Promedio vectorial de dirección ────────────────────────────────────────────
float windSumX = 0, windSumY = 0;
int   windSampleCount = 0;
float finalAvgWindDir = 0;

void accumulateWindVector(float deg) {
  float r = deg * PI / 180.0f;
  portENTER_CRITICAL(&windMux);
  windSumX += cos(r);
  windSumY += sin(r);
  windSampleCount++;
  portEXIT_CRITICAL(&windMux);
}

float calcAndResetWindVector() {
  portENTER_CRITICAL(&windMux);
  if (windSampleCount == 0) {
    portEXIT_CRITICAL(&windMux);
    return 0;
  }
  float deg = atan2(windSumY, windSumX) * 180.0f / PI;
  if (deg < 0) deg += 360.0f;
  windSumX = windSumY = 0;
  windSampleCount = 0;
  portEXIT_CRITICAL(&windMux);
  return deg;
}

// ── Estado ────────────────────────────────────────────────────────────────────
bool lastServerOK    = false;
unsigned long lastSendTime        = 0;
unsigned long lastScreenTime      = 0;
unsigned long lastSlowSensorRead   = 0;  // sensores lentos I2C: MCP9808, BMP280, HTU21, DHT, luz, suelo
unsigned long lastPipelineFastRead = 0;  // XDB401 + caudalímetro: tan rápido como PIPELINE_FAST_MS
unsigned long lastSensorRead       = 0;  // alias para compatibilidad con el bloque telemetría
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
unsigned long lastSoilReadMs      = 0;   // último muestreo del sensor de suelo RS485
unsigned long soilPostIrrigEndMs  = 0;   // millis() cuando se apagó el último relay (0 = inactivo)
#endif


// ── Info estática del hardware ────────────────────────────────────────────────
void printHardwareInfo() {
  DLOGLN("\n=== Hardware Info ===");
  DLOGF("Chip         : %s rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
  DLOGF("CPU          : %d MHz\n",      ESP.getCpuFreqMHz());
  DLOGF("Flash        : %d MB\n",        ESP.getFlashChipSize() / (1024 * 1024));
  DLOGF("Free Heap    : %d bytes\n",     ESP.getFreeHeap());
  DLOGF("SDK          : %s\n",           ESP.getSdkVersion());
  DLOGF("MAC          : %s\n",           getDeviceMacAddress().c_str());
#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
  DLOGF("IP           : %s\n",           WiFi.localIP().toString().c_str());
#endif
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

static bool tlsClockReady(unsigned long waitMs = 5000) {
  time_t now = time(nullptr);
  unsigned long start = millis();

  while (now < 1700000000L && millis() - start < waitMs) {
    delay(250);
    now = time(nullptr);
  }
  return now >= 1700000000L;
}

#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
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

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static void prepareGsmTLSClient() {
  gsmTLSClient.setCACert(MQTT_CA_CERT_PEM);
}

// Enciende el modem SIM7000G y establece conexión GPRS con APN Onomondo.
// Retorna true si GPRS queda operativo.
static bool sim7000g_powerOn() {
  // DTR LOW → mantiene el modem despierto (no entra en sleep)
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  // Secuencia PWRKEY: pulso ~1000 ms según datasheet SIM7000G
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH); delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);

  // UART modem en Serial1 (hardware UART independiente del debug)
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  DLOGLN("[SIM] Esperando boot del modem (3 s)...");
  delay(3000);

  DLOGLN("[SIM] Inicializando TinyGSM...");
  if (!modemSIM.init()) {
    DLOGLN("[SIM] ERROR: modem no responde a AT");
    return false;
  }
  String info = modemSIM.getModemInfo();
  DLOGF("[SIM] Modem: %s\n", info.c_str());

  // Esperar registro en red celular (max 60 s)
  DLOGLN("[SIM] Esperando registro en red...");
  if (!modemSIM.waitForNetwork(60000L)) {
    DLOGLN("[SIM] ERROR: sin cobertura o SIM no registrada");
    return false;
  }
  DLOGF("[SIM] Red OK — operador: %s  RSSI: %d\n",
        modemSIM.getOperator().c_str(), modemSIM.getSignalQuality());

  // Conectar GPRS con APN Onomondo (sin usuario ni contraseña)
  DLOGF("[SIM] Conectando GPRS (APN=%s)...\n", GSM_APN);
  if (!modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS)) {
    DLOGLN("[SIM] ERROR: fallo al conectar GPRS");
    return false;
  }
  DLOGF("[SIM] GPRS OK — IP local: %s\n", modemSIM.localIP().toString().c_str());
  return true;
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
  doc["chip_model"]    = ESP.getChipModel();
  doc["chip_revision"] = (int)ESP.getChipRevision();
  doc["cpu_freq_mhz"]  = ESP.getCpuFreqMHz();
  doc["flash_size_mb"] = ESP.getFlashChipSize() / (1024 * 1024);
  doc["sdk_version"]   = ESP.getSdkVersion();
  doc["mac_address"]      = getDeviceMacAddress();
  doc["ip_address"]       = WiFi.localIP().toString();
  doc["relay_count"]      = RELAY_COUNT;
  doc["firmware_version"] = FIRMWARE_VERSION;

  String json;
  serializeJson(doc, json);

  String url = serverBaseUrl() + "/api/device_info";
  HTTPClient http;
  http.setTimeout(10000);
  if (serverUseTls()) {
    WiFiClientSecure client;
    prepareSecureClient(client, 10000);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  DLOGF("[DeviceInfo] POST → %d\n", code);
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────
String httpGet(const String& url, int timeoutMs = 10000) {
  HTTPClient http;
  http.setTimeout(timeoutMs);
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    prepareSecureClient(client, timeoutMs);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
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
      const char* nextScenario = doc["scenario"] | (const char*)pipelineScenario;
      const char* nextMode     = doc["mode"] | (const char*)pipelineMode;
      long nextTelemetry  = doc["telemetry_interval_s"] | (long)(telemetryIntervalMs / 1000UL);
      long nextSync       = doc["config_sync_interval_s"] | (long)(configSyncIntervalMs / 1000UL);
#ifdef HAS_DISPLAY
      long nextDisplay    = doc["display_timeout_s"] | (long)(displayTimeoutMs / 1000UL);
#endif

      // Proteger escrituras de config con mutex (leídas desde Core 1)
#if defined(FLOW_PIN)
      // Reset de contadores de flujo si el backend lo solicita (one-shot)
      if (doc["reset_flow_counters"] | false) {
        portENTER_CRITICAL(&_flowMux);
        _flowIrrigPulses = 0;
        _flowLeakPulses  = 0;
        _flowSessionBase = _flowPulseTotal;  // también resetea el contador de sesión
        portEXIT_CRITICAL(&_flowMux);
        DLOGLN("[FLOW] Contadores reseteados por el backend");
        // Notificar al backend que ya se ejecutó el reset
        httpPost(serverBaseUrl() + "/api/flow/reset-ack?mac=" + WiFi.macAddress(), "");
      }
#endif
      if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
      if (strcmp(nextScenario, "normal") == 0 || strcmp(nextScenario, "leak") == 0 ||
          strcmp(nextScenario, "burst")  == 0 || strcmp(nextScenario, "obstruction") == 0) {
        if (strcmp(nextScenario, pipelineScenario) != 0) {
          DLOGF("[PIPE] Escenario → %s\n", nextScenario);
        }
        strlcpy(pipelineScenario, nextScenario, sizeof(pipelineScenario));
      }
      if (strcmp(nextMode, "sim") == 0 || strcmp(nextMode, "real") == 0) {
        if (strcmp(nextMode, pipelineMode) != 0) {
          DLOGF("[PIPE] Modo → %s\n", nextMode);
        }
        strlcpy(pipelineMode, nextMode, sizeof(pipelineMode));
      }
      // Tipo de riego (afecta umbrales del LeakDetector)
      const char* nextIrrigStr = doc["irrigation_type"] | irrigTypeToStr(irrigationType);
      IrrigationType nextIrrig = irrigStrToType(nextIrrigStr);
      if (nextIrrig != irrigationType) {
        irrigationType = nextIrrig;
        leakDetector.begin(irrigationType);
        DLOGF("[PIPE] Tipo riego → %s\n", irrigTypeToStr(irrigationType));
      }
      if (dataMutex) xSemaphoreGive(dataMutex);
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
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      {
        long nextSoilFast = doc["soil_fast_interval_s"] | (long)(soilFastIntervalMs / 1000UL);
        long nextSoilSlow = doc["soil_slow_interval_s"] | (long)(soilSlowIntervalMs / 1000UL);
        if (nextSoilFast >= 3 && nextSoilFast <= 300)
          soilFastIntervalMs = (unsigned long)nextSoilFast * 1000UL;
        if (nextSoilSlow >= 20 && nextSoilSlow <= 3600)
          soilSlowIntervalMs = (unsigned long)nextSoilSlow * 1000UL;
      }
#endif
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
      if (strcmp(body.c_str(), pipelineScenario) != 0) {
        DLOGF("[PIPE] Escenario → %s\n", body.c_str());
      }
      strlcpy(pipelineScenario, body.c_str(), sizeof(pipelineScenario));
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
#if defined(FLOW_PIN)
      // OFF→ON: resetear contador de sesión para contar litros de este ciclo de riego.
      if (desired && !relayActive[i]) flowSessionReset();
#endif
      relayActive[i] = desired;
      // Relay activo-HIGH: HIGH = relay ON, LOW = relay OFF
      digitalWrite(RELAY_PINS[i], desired ? HIGH : LOW);
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
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    prepareSecureClient(client, 10000);
    http.begin(client, url);
  } else {
    WiFiClient client;
    http.begin(client, url);
  }
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
#include "display_tft.h"
#endif  // HAS_DISPLAY

// =============================================================================
// MQTT — Funciones auxiliares (solo ESP32 con USE_MQTT)
// =============================================================================
#if defined(USE_MQTT)
#include "mqtt_helpers.h"
#endif  // USE_MQTT
// =============================================================================
// Snapshot de datos — captura atómica de sensores para telemetría.
// Rellena la variable global _netSnap (definida junto a los vars FreeRTOS).
// Sin parámetros para evitar el problema de auto-prototype del Arduino IDE
// con tipos personalizados en la firma.
// =============================================================================
void takeSnapshot() {
  // Core 1 construye el snapshot completo y lo publica en telemetryQueue.
  // Core 0 solo lo lee aquí — sin bloqueo, sin mutex, sin latencia de 50ms.
  if (telemetryQueue) {
    xQueuePeek(telemetryQueue, &_netSnap, 0);
  }
  // Campos que Core 0 puede leer directamente (thread-safe en ESP32: 32-bit, same-core read)
  _netSnap.heap   = (long)ESP.getFreeHeap();
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  _netSnap.rssi   = modemSIM.getSignalQuality();  // CSQ 0-31
#else
  _netSnap.rssi   = WiFi.RSSI();
#endif
  _netSnap.uptime = (long)(millis() / 1000);
}

// =============================================================================
// TAREA DE RED — Core 0 (solo ESP32)
// Gestiona OTA, relay polling y HTTP POST sin bloquear sensores/display (Core 1)
// =============================================================================
void networkTask(void* pvParameters) {
  // Tarea separada de red para no bloquear sensores ni la UI principal.
  // Mantiene yields periódicos con vTaskDelay() al final del bucle.

  // En IDF 5.x el sistema ya inicia el TWDT para las tareas IDLE.
  // esp_task_wdt_init() devuelve ESP_ERR_INVALID_STATE si ya está inicializado
  // y el timeout corto del sistema permanecería activo — usar reconfigure como fallback.
  {
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
    if (esp_task_wdt_init(&wdt_cfg) == ESP_ERR_INVALID_STATE) {
      esp_task_wdt_reconfigure(&wdt_cfg);
    }
  }
  esp_task_wdt_add(NULL);

  static bool          deviceInfoSent   = false;
  static unsigned long lastRelayCheck   = 0;
  static unsigned long lastSendTime     = 0;
  static unsigned long lastScenarioSync = 0;
  static unsigned long lastNtpRetry     = 0;
  static unsigned long wifiRetryDelayMs  = 500;
  static unsigned long wifiStableSince   = 0;  // millis() al reconectar
  static int           wifiFailCount     = 0;

#ifdef USE_MQTT
  static unsigned long mqttRetryDelayMs = 2000;

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  if (mqtt_port == 8883) {
    prepareGsmTLSClient();
    mqttClient.setClient(gsmTLSClient);
    DLOGF("[MQTT] Broker TLS (GSM): %s:%d\n", mqtt_server, mqtt_port);
  } else {
    mqttClient.setClient(gsmTCPClient);
    DLOGF("[MQTT] Broker sin TLS (GSM): %s:%d\n", mqtt_server, mqtt_port);
  }
#else
  if (mqtt_port == 8883) {
    prepareSecureClient(mqttTLSClient, 10000);
    mqttClient.setClient(mqttTLSClient);
    DLOGF("[MQTT] Broker TLS verificado: %s:%d\n", mqtt_server, mqtt_port);
  } else {
    mqttClient.setClient(mqttTCPClient);
    DLOGF("[MQTT] Broker local sin TLS: %s:%d\n", mqtt_server, mqtt_port);
  }
#endif
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1536);
#endif

  for (;;) {
    wdt_heartbeat("NetworkTask");
    esp_task_wdt_reset();
#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
    ArduinoOTA.handle();  // siempre primero, alta frecuencia

    if (isUpdatingOTA) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
#endif

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    // ── Conectividad celular: verificar red GSM y GPRS ─────────────────────
    if (!modemSIM.isNetworkConnected()) {
      wdt_heartbeat("NetworkTask", "gsm_network_wait");
      setLedState(LED_WIFI_CONNECTING);
      wifiFailCount++;
      if (wifiFailCount >= 60) {
        // ~5 min sin red celular — reiniciar dispositivo
        esp_restart();
      }
      if (!modemSIM.waitForNetwork(10000L, true)) {
        vTaskDelay(pdMS_TO_TICKS(wifiRetryDelayMs));
        if (wifiRetryDelayMs < 30000) wifiRetryDelayMs = min(wifiRetryDelayMs * 2UL, 30000UL);
        continue;
      }
    }
    if (!modemSIM.isGprsConnected()) {
      wdt_heartbeat("NetworkTask", "gprs_connect");
      setLedState(LED_WIFI_CONNECTING);
      if (!modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS)) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      DLOGF("[SIM] GPRS reconectado — IP: %s\n", modemSIM.localIP().toString().c_str());
    }
    wifiFailCount = 0;
    wifiRetryDelayMs = 500;
#else
    // ── Conectividad WiFi ───────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
      wdt_heartbeat("NetworkTask", "wifi_reconnect");
      setLedState(LED_WIFI_CONNECTING);
      wifiStableSince = 0;
      wifiFailCount++;

      if (wifiFailCount >= 60) {
        // ~5 min sin conectar — reinicio total del dispositivo
        esp_restart();
      } else if (wifiFailCount % 10 == 0) {
        // Cada 10 fallos: reset completo del stack WiFi (WiFi.reconnect() no sale de stack hung)
        WiFi.disconnect(true);
        vTaskDelay(pdMS_TO_TICKS(1000));
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_18_5dBm);
        WiFi.begin(ssid, password);
      } else {
        WiFi.reconnect();
      }

      vTaskDelay(pdMS_TO_TICKS(wifiRetryDelayMs));
      if (wifiRetryDelayMs < 30000) wifiRetryDelayMs = min(wifiRetryDelayMs * 2UL, 30000UL);
      continue;
    }
    wifiFailCount = 0;
    // Resetear backoff solo tras 10s de conexión estable (evita martillear el AP)
    if (wifiStableSince == 0) wifiStableSince = millis();
    if (millis() - wifiStableSince > 10000) wifiRetryDelayMs = 500;
#endif

    unsigned long now = millis();

#ifdef USE_MQTT
    // Reintentar NTP cada 60s si el reloj no está sincronizado (fallo en boot)
#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
    if (time(nullptr) < 1000000000L && (now - lastNtpRetry > 60000 || lastNtpRetry == 0)) {
      DLOGLN("[NTP] Reintentando sincronización...");
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      lastNtpRetry = now;
    }
#endif
#endif

#ifndef USE_MQTT
    if (lastScenarioSync == 0 || now - lastScenarioSync >= configSyncIntervalMs) {
      wdt_heartbeat("NetworkTask", "scenario_sync");
      syncPipelineScenario();
      lastScenarioSync = now;
    }
#endif

#ifdef USE_MQTT
    // ── Modo MQTT ────────────────────────────────────────────────────────────
    if (!mqttClient.connected()) {
      wdt_heartbeat("NetworkTask", "mqtt_connect");
      setLedState(LED_MQTT_CONNECTING);
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      if (mqtt_port == 8883) prepareGsmTLSClient();
#else
      if (mqtt_port == 8883) prepareSecureClient(mqttTLSClient, 10000);
#endif
      esp_task_wdt_reset();
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
        // Guardar timestamp de primera conexión para la pantalla de info
        time_t t = time(nullptr);
        if (t > 1000000000L) {
          struct tm tm_info;
          localtime_r(&t, &tm_info);
          strftime(g_lastConnectStr, sizeof(g_lastConnectStr),
                   "%H:%M:%S %d/%m/%y", &tm_info);
        }
        // Notificar reinicio al backend con motivo
        char msg[64];
        snprintf(msg, sizeof(msg), "Dispositivo reiniciado: %s", g_rebootReason);
        mqttPublishAlert("device_reboot", "info", msg);
      } else {
        static unsigned long _mqttReconnectAlertMs = 0;
        if (millis() - _mqttReconnectAlertMs >= 600000UL) {  // máx 1 alerta cada 10 min
          mqttPublishAlert("mqtt_reconnect", "info", "Dispositivo reconectado al broker MQTT");
          _mqttReconnectAlertMs = millis();
        }
      }
    }
    mqttClient.loop();

    // ── Alarmas MQTT — solo al cambio de estado (no spamear cada ciclo) ──────
    {
      static const unsigned long SENSOR_ALERT_COOLDOWN = 43200000UL;  // 12 h entre alertas repetidas del mismo sensor

      static char   _lastScenario[16] = "normal";
      static bool   _lastXdb401Ok         = true;
      static bool   _lastHeapWarn         = false;
#if DEVICE_PROFILE == PROFILE_METEO
      static bool   _lastMcpOk            = true;
      static bool   _lastBmpOk            = true;
#endif
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK
      static bool   _lastMicroPressureOk  = true;
#endif
#if DEVICE_PROFILE == PROFILE_METEO
      static bool   _lastHtuOk            = true;
#endif
#if DEVICE_PROFILE == PROFILE_AQUALEAK
      static bool   _lastHdcOk            = true;
      static bool   _lastBh1750Ok         = true;
#endif

      // Pipeline: leak / burst / obstruction / recuperacion
      if (strcmp(pipelineScenario, _lastScenario) != 0) {
        if (strcmp(pipelineScenario, "leak") == 0)
          mqttPublishAlert("leak",        "warning",  "Fuga detectada: caudal con valvula cerrada");
        else if (strcmp(pipelineScenario, "burst") == 0)
          mqttPublishAlert("burst",       "critical", "Reventón: presión y caudal anormalmente altos");
        else if (strcmp(pipelineScenario, "obstruction") == 0)
          mqttPublishAlert("obstruction", "warning",  "Obstruccion: presión alta, caudal bajo");
        else if (strcmp(pipelineScenario, "normal") == 0 &&
                 strcmp(_lastScenario, "normal") != 0)
          mqttPublishAlert("pipeline_ok", "info",     "Pipeline recuperado: estado normal");
        strlcpy(_lastScenario, pipelineScenario, sizeof(_lastScenario));
      }

      // Sensor XDB401: fallo y recuperación
      {
        static unsigned long _xdb401AlertMs = 0;
        if (!xdb401_ok && _lastXdb401Ok) {
          mqttPublishAlert("sensor_failure", "warning", "XDB401 sin respuesta — presion no disponible");
          _xdb401AlertMs = millis();
        } else if (!xdb401_ok && !_lastXdb401Ok &&
                   millis() - _xdb401AlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "XDB401 sin respuesta — presion no disponible");
          _xdb401AlertMs = millis();
        } else if (xdb401_ok && !_lastXdb401Ok) {
          mqttPublishAlert("sensor_ok", "info", "XDB401 recuperado");
        }
        _lastXdb401Ok = xdb401_ok;
      }

#if DEVICE_PROFILE == PROFILE_METEO
      // MCP9808
      {
        static unsigned long _mcpAlertMs = 0;
        if (!mcp_ok && _lastMcpOk) {
          mqttPublishAlert("sensor_failure", "warning", "MCP9808 sin respuesta — temperatura exterior no disponible");
          _mcpAlertMs = millis();
        } else if (!mcp_ok && !_lastMcpOk && millis() - _mcpAlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "MCP9808 sin respuesta — temperatura exterior no disponible");
          _mcpAlertMs = millis();
        } else if (mcp_ok && !_lastMcpOk) {
          mqttPublishAlert("sensor_ok", "info", "MCP9808 recuperado");
        }
        _lastMcpOk = mcp_ok;
      }

      // BMP280
      {
        static unsigned long _bmpAlertMs = 0;
        if (!bmp_ok && _lastBmpOk) {
          mqttPublishAlert("sensor_failure", "warning", "BMP280 sin respuesta — temperatura/presion barometrica no disponibles");
          _bmpAlertMs = millis();
        } else if (!bmp_ok && !_lastBmpOk && millis() - _bmpAlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "BMP280 sin respuesta — temperatura/presion barometrica no disponibles");
          _bmpAlertMs = millis();
        } else if (bmp_ok && !_lastBmpOk) {
          mqttPublishAlert("sensor_ok", "info", "BMP280 recuperado");
        }
        _lastBmpOk = bmp_ok;
      }
#endif

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK
      // MicroPressure
      {
        static unsigned long _mpAlertMs = 0;
        if (!micropressure_ok && _lastMicroPressureOk) {
          mqttPublishAlert("sensor_failure", "warning", "MicroPressure sin respuesta — barometro no disponible");
          _mpAlertMs = millis();
        } else if (!micropressure_ok && !_lastMicroPressureOk && millis() - _mpAlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "MicroPressure sin respuesta — barometro no disponible");
          _mpAlertMs = millis();
        } else if (micropressure_ok && !_lastMicroPressureOk) {
          mqttPublishAlert("sensor_ok", "info", "MicroPressure recuperado");
        }
        _lastMicroPressureOk = micropressure_ok;
      }
#endif

#if DEVICE_PROFILE == PROFILE_METEO
      // HTU2x
      {
        static unsigned long _htuAlertMs = 0;
        if (!htu_ok && _lastHtuOk) {
          mqttPublishAlert("sensor_failure", "warning", "HTU2x sin respuesta — temperatura/humedad interior no disponibles");
          _htuAlertMs = millis();
        } else if (!htu_ok && !_lastHtuOk && millis() - _htuAlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "HTU2x sin respuesta — temperatura/humedad interior no disponibles");
          _htuAlertMs = millis();
        } else if (htu_ok && !_lastHtuOk) {
          mqttPublishAlert("sensor_ok", "info", "HTU2x recuperado");
        }
        _lastHtuOk = htu_ok;
      }
#endif

#if DEVICE_PROFILE == PROFILE_AQUALEAK
      // HDC1080
      {
        static unsigned long _hdcAlertMs = 0;
        if (!hdc_ok && _lastHdcOk) {
          mqttPublishAlert("sensor_failure", "warning", "HDC1080 sin respuesta — temperatura/humedad no disponibles");
          _hdcAlertMs = millis();
        } else if (!hdc_ok && !_lastHdcOk && millis() - _hdcAlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "HDC1080 sin respuesta — temperatura/humedad no disponibles");
          _hdcAlertMs = millis();
        } else if (hdc_ok && !_lastHdcOk) {
          mqttPublishAlert("sensor_ok", "info", "HDC1080 recuperado");
        }
        _lastHdcOk = hdc_ok;
      }

      // BH1750
      {
        static unsigned long _bh1750AlertMs = 0;
        if (!bh1750_ok && _lastBh1750Ok) {
          mqttPublishAlert("sensor_failure", "warning", "BH1750 sin respuesta — luz ambiental no disponible");
          _bh1750AlertMs = millis();
        } else if (!bh1750_ok && !_lastBh1750Ok && millis() - _bh1750AlertMs >= SENSOR_ALERT_COOLDOWN) {
          mqttPublishAlert("sensor_failure", "warning", "BH1750 sin respuesta — luz ambiental no disponible");
          _bh1750AlertMs = millis();
        } else if (bh1750_ok && !_lastBh1750Ok) {
          mqttPublishAlert("sensor_ok", "info", "BH1750 recuperado");
        }
        _lastBh1750Ok = bh1750_ok;
      }
#endif

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      // Suelo muy seco
      {
        static const float SOIL_DRY_THRESHOLD = 30.0f;  // % — por debajo = suelo seco
        static bool        _lastSoilDry   = false;
        static unsigned long _soilDryAlertMs = 0;
        bool soilDry = (soilMoisture < SOIL_DRY_THRESHOLD);
        if (soilDry && !_lastSoilDry) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Suelo muy seco — humedad %.1f%% (umbral %.0f%%)", soilMoisture, SOIL_DRY_THRESHOLD);
          mqttPublishAlert("soil_dry", "warning", msg);
          _soilDryAlertMs = millis();
        } else if (soilDry && _lastSoilDry && millis() - _soilDryAlertMs >= SENSOR_ALERT_COOLDOWN) {
          char msg[64];
          snprintf(msg, sizeof(msg), "Suelo muy seco — humedad %.1f%% (umbral %.0f%%)", soilMoisture, SOIL_DRY_THRESHOLD);
          mqttPublishAlert("soil_dry", "warning", msg);
          _soilDryAlertMs = millis();
        } else if (!soilDry && _lastSoilDry) {
          mqttPublishAlert("soil_ok", "info", "Humedad del suelo recuperada");
        }
        _lastSoilDry = soilDry;
      }
#endif

      // Heap bajo (< 30 KB)
      bool heapWarn = (ESP.getFreeHeap() < 30000);
      if (heapWarn && !_lastHeapWarn)
        mqttPublishAlert("low_heap", "warning", "Heap libre < 30 KB — posible memory leak");
      _lastHeapWarn = heapWarn;
    }

    // Publicar telemetría cada MQTT_SEND_MS
    if (now - lastSendTime >= telemetryIntervalMs) {
      wdt_heartbeat("NetworkTask", "mqtt_publish");
      takeSnapshot();
      const TelemetrySnapshot& snap = _netSnap;

      // Redondeo para reducir tamaño del payload MQTT
      auto r2 = [](float x){ return roundf(x * 100.0f) / 100.0f; };  // 2 decimales
      auto r1 = [](float x){ return roundf(x * 10.0f)  / 10.0f;  };  // 1 decimal

      // Payload ampliado con métricas explícitas del BMP280 e INA219 (IRRIGATION).
      StaticJsonDocument<1536> doc;
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
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      doc["halisense_ok"]          = snap.halisenseOk;
      doc["soil_irrig_mode"]       = snap.soilIrrigMode;
      if (snap.halisenseOk) {
        doc["soil_temperature"]    = r1(snap.soilTemp);
        doc["soil_ec"]             = r2(snap.soilEc);
        doc["soil_ph"]             = r1(snap.soilPh);
        doc["soil_tds"]            = r1(snap.soilTds);
        doc["soil_n"]              = snap.soilN;
        doc["soil_p"]              = snap.soilP;
        doc["soil_k"]              = snap.soilK;
      }
#endif
      doc["pipeline_pressure"]     = r2(snap.pipePressure);
      doc["pipeline_flow"]         = r2(snap.pipeFlow);
      doc["flow_total_l"]          = roundf(snap.flowTotalL   * 10.0f) / 10.0f;  // 1 decimal → 100 mL resolución
      doc["flow_session_l"]        = roundf(snap.flowSessionL * 10.0f) / 10.0f;  // litros desde última apertura de válvula
      doc["flow_irrig_l"]          = roundf(snap.flowIrrigL   * 10.0f) / 10.0f;  // litros acumulados con relay ON (riego)
      doc["flow_leak_l"]           = roundf(snap.flowLeakL    * 10.0f) / 10.0f;  // litros acumulados con relay OFF (fuga)
      doc["pipeline_scenario"]     = pipelineScenario;
      doc["pipeline_mode"]         = pipelineMode;
      doc["pipeline_source"]       = pipelineSource;
      doc["irrigation_type"]       = irrigTypeToStr(irrigationType);
      doc["leak_detect_trained"]   = leakDetector.hasBaseline();
      doc["pipeline_pressure_ok"]  = pipelinePressureOk;
      doc["pipeline_flow_ok"]      = pipelineFlowOk;
      doc["leak_baseline_pressure"] = r2(leakDetector.baselinePressure());
      doc["leak_baseline_flow"]     = r2(leakDetector.baselineFlow());
      doc["leak_warmup_progress"]   = leakDetector.warmupProgress();
      doc["xdb401_ok"]             = xdb401_ok;
      if (!isnan(snap.xdb401Temp)) doc["xdb401_temperature"] = r2(snap.xdb401Temp);
      doc["mac_address"]           = getDeviceMacAddress(); // eFuse — idéntico en WiFi y cellular
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      doc["ip_address"]            = modemSIM.localIP().toString();
      doc["network"]               = "cellular";
#else
      doc["ip_address"]            = WiFi.localIP().toString();
#endif
      doc["relay_count"]           = RELAY_COUNT;
      doc["firmware_version"]      = FIRMWARE_VERSION;
#if DEVICE_PROFILE == PROFILE_AQUALEAK
      // Parámetros calculados agroambientales — solo PROFILE_AQUALEAK
      if (!isnan(snap.dewPoint))   doc["dew_point"]    = r1(snap.dewPoint);
      if (!isnan(snap.heatIndex))  doc["heat_index"]   = r1(snap.heatIndex);
      if (!isnan(snap.absHum))     doc["abs_humidity"] = r2(snap.absHum);
#endif
#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      doc["aht20_ok"]  = aht20_ok;
      doc["ina219_ok"] = ina219_ok;
      if (!isnan(snap.inaVbus))    doc["ina219_bus_voltage"] = r2(snap.inaVbus);
      if (!isnan(snap.inaCurrent)) doc["ina219_current_ma"]  = r1(snap.inaCurrent);
      if (!isnan(snap.inaPower))   doc["ina219_power_mw"]    = r1(snap.inaPower);
#endif
      // Timestamp NTP — solo si el reloj está sincronizado (epoch > año 2001)
      // El backend lo usa como timestamp real de la medición en lugar de NOW().
      {
        time_t ntp_ts = time(nullptr);
        if (ntp_ts > 1000000000L) doc["ts"] = (long)ntp_ts;
      }

      char topic[64], buf[1536];
      snprintf(topic, sizeof(topic), "aquantia/%s/telemetry", finca_id);
      size_t payload_len = serializeJson(doc, buf, sizeof(buf));
      if (payload_len >= sizeof(buf)) {
        DLOGF("[MQTT] WARN payload truncado (%u >= %u)\n",
                      (unsigned)payload_len, (unsigned)sizeof(buf));
      }
      bool ok = mqttClient.publish(topic, buf, false);
      setLedState(ok ? LED_TX_OK : LED_TX_ERROR);
      DLOGF("[MQTT] TX %s (%u B)\n"
            "  T:%.1f°C(%s)  H:%.0f%%(%s)  P:%.2fkPa(%s)\n"
            "  Viento:%.1fm/s  Luz:%.0flux\n"
            "  Tuberia:%.3fbar %.2fL/min  escenario:%s  fuente:%s\n"
            "  Heap:%ldB  RSSI:%ddBm\n",
            ok ? "OK" : "ERROR", (unsigned)payload_len,
            snap.tempMCP,  temperatureSourceName(),
            snap.humidity, htu_ok ? "HTU2x" : "SIM",
            snap.pressure, pressureSourceName(),
            snap.windSpeedFilt, snap.light,
            snap.pipePressure, snap.pipeFlow,
            pipelineScenario, pipelineSource.c_str(),
            snap.heap, snap.rssi);

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
      char msg[340];
      snprintf(msg, sizeof(msg),
        "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%ld,%ld,%d,%.2f,%.2f,%.2f,%.1f",
        snap.tempMCP, snap.pressure, snap.tempDHT, snap.humidity,
        snap.windSpeed, snap.windDir, snap.windSpeedFilt, snap.avgWindDir,
        snap.light, snap.tempDHT11, snap.humDHT11,
        snap.rssi, snap.heap, snap.uptime, snap.relayMask,
        snap.pipePressure, snap.pipeFlow, snap.soil,
        snap.flowTotalL   // litros acumulados desde arranque
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

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(160);
  DLOGLN("\n\n=== MeteoStation BOOT ===");

  // Capturar la razón de reinicio lo antes posible
  {
    esp_reset_reason_t rr = esp_reset_reason();
    strlcpy(g_rebootReason, resetReasonStr(rr), sizeof(g_rebootReason));
    if ((rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT)
        && g_wdtMagic == WDT_MAGIC && g_wdtLastTask[0] != '\0') {
      strncat(g_rebootReason, ":", sizeof(g_rebootReason) - strlen(g_rebootReason) - 1);
      strncat(g_rebootReason, g_wdtLastTask, sizeof(g_rebootReason) - strlen(g_rebootReason) - 1);
    }
    g_wdtMagic = 0;  // invalidar hasta el próximo ciclo
  }

#ifdef DEBUG_MODE
  DLOGLN("=== DEBUG MODE ACTIVO ===");
  DLOGF("[TEST] Perfil  : %s (%d)\n",
    (DEVICE_PROFILE == PROFILE_METEO) ? "METEO" :
    (DEVICE_PROFILE == PROFILE_AQUALEAK) ? "AQUALEAK" : "IRRIGATION", DEVICE_PROFILE);
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
  Wire.setClock(PRESSURE_SENSOR_I2C_FREQ_HZ);  // 50 kHz globales — el XDB401 tiene cable
                                                // ~1 m: a 100 kHz la capacidad parásita
                                                // (~100 pF) produce flancos lentos que
                                                // provocan ACK-miss y bloqueos de bus.
                                                // MCP9808, BMP280, HTU2x se leen cada 20 s
                                                // → sin impacto real en rendimiento.
  Wire.setTimeOut(200);   // 200 ms — holgura para cable largo sin bloquear el loop
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
#else
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

#if DEVICE_PROFILE == PROFILE_AQUALEAK
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
    digitalWrite(RELAY_PINS[i], LOW);  // LOW = OFF para relay activo-HIGH
  }
  DLOGF("%d relay(s) inicializados en OFF (LOW)\n", RELAY_COUNT);

#if defined(ESP32)
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
  _flowLastCalcUs = micros();
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
    bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                       Adafruit_BMP280::SAMPLING_X2,
                       Adafruit_BMP280::SAMPLING_X16,
                       Adafruit_BMP280::FILTER_X16,
                       Adafruit_BMP280::STANDBY_MS_500);
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
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  DLOGLN("MCP9808 — perfil AGROMETEO, sensor omitido");
  DLOGLN("BMP280 y MicroPressure — inicializados en bloque AGROMETEO");
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  // ── IRRIGATION: BMP280 + AHT20 + INA219 ─────────────────────────────────
  mcp_ok = false;  // MCP9808 no presente en este perfil

  // BMP280 — temperatura ambiente + presión atmosférica
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
    if (bmp_pressure_ok) { bmpPressure = pBmp; pressure = pBmp; bar_ok = true; }
    DLOGF("BMP280 OK (0x%02X)\n", bmp280_addr);
  } else {
    bmp_temp_ok = bmp_pressure_ok = false;
    DLOGLN("BMP280 no detectado — modo simulacion");
  }

  // AHT20 — temperatura y humedad ambiente
  aht20_ok = aht20_begin();
  if (aht20_ok) {
    float t = NAN, h = NAN;
    if (aht20_read(t, h)) {
      temperatureMCP = t;
      humidity       = h;
      temp_ok        = true;
      DLOGF("AHT20 OK — T:%.1f C  H:%.1f %%\n", t, h);
    } else {
      aht20_ok = false;
    }
  }
  if (!aht20_ok) {
    DLOGLN("AHT20 no detectado — modo simulacion");
    if (bmp_temp_ok) { temperatureMCP = bmpTemperature; temp_ok = true; }
    else             { temperatureMCP = sim_tempMCP; }
    humidity = sim_humidity;
  }

  // INA219 — voltaje / corriente / potencia
  ina219_ok = ina219_begin();
  if (ina219_ok) {
    ina219BusVoltage = ina219_readBusVoltage();
    ina219Current    = ina219_readCurrent_mA();
    ina219Power      = ina219_readPower_mW();
    DLOGF("INA219 OK — Vbus:%.2f V  I:%.1f mA  P:%.1f mW\n",
          ina219BusVoltage, ina219Current, ina219Power);
  } else {
    DLOGLN("INA219 no detectado");
  }

  DLOGF("Sensores activos → TempEnv:%s | Presion:%s\n",
    temperatureSourceName(), pressureSourceName());
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // ── AQUA_SMART_REMOTE: mismo sensor suite que IRRIGATION ────────────────
  mcp_ok = false;

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
    if (bmp_pressure_ok) { bmpPressure = pBmp; pressure = pBmp; bar_ok = true; }
    DLOGF("BMP280 OK (0x%02X)\n", bmp280_addr);
  } else {
    bmp_temp_ok = bmp_pressure_ok = false;
    DLOGLN("BMP280 no detectado — modo simulacion");
  }

  aht20_ok = aht20_begin();
  if (aht20_ok) {
    float t = NAN, h = NAN;
    if (aht20_read(t, h)) {
      temperatureMCP = t;
      humidity       = h;
      temp_ok        = true;
      DLOGF("AHT20 OK — T:%.1f C  H:%.1f %%\n", t, h);
    } else {
      aht20_ok = false;
    }
  }
  if (!aht20_ok) {
    DLOGLN("AHT20 no detectado — modo simulacion");
    if (bmp_temp_ok) { temperatureMCP = bmpTemperature; temp_ok = true; }
    else             { temperatureMCP = sim_tempMCP; }
    humidity = sim_humidity;
  }

  ina219_ok = ina219_begin();
  if (ina219_ok) {
    ina219BusVoltage = ina219_readBusVoltage();
    ina219Current    = ina219_readCurrent_mA();
    ina219Power      = ina219_readPower_mW();
    DLOGF("INA219 OK — Vbus:%.2f V  I:%.1f mA  P:%.1f mW\n",
          ina219BusVoltage, ina219Current, ina219Power);
  } else {
    DLOGLN("INA219 no detectado");
  }

  DLOGF("Sensores activos → TempEnv:%s | Presion:%s\n",
    temperatureSourceName(), pressureSourceName());
#else
  // Perfil desconocido — sin sensores I2C meteo
  DLOGLN("Perfil desconocido — sensores meteo omitidos");
#endif

#if DEVICE_PROFILE == PROFILE_AQUALEAK
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
#endif  // PROFILE_AQUALEAK

#if DEVICE_PROFILE == PROFILE_METEO
  // HTU2x — omitido en IRRIGATION y AGROMETEO
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
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  // AGROMETEO: sin HTU2x — HDC1080 ya inicializado
  htu_ok         = false;
  temperatureDHT = sim_tempDHT;
#else
  // IRRIGATION: sin HTU2x
  htu_ok         = false;
  temperatureDHT = sim_tempDHT;
  humidity       = sim_humidity;
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
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  DLOGLN("DHT11 — perfil AGROMETEO, sensor omitido");
#else
  DLOGLN("DHT11 — perfil IRRIGATION, sensor omitido");
#endif

#if DEVICE_PROFILE == PROFILE_METEO
  tsl_ok = tsl_begin();
  if (tsl_ok) {
    DLOGLN("Sensor luz OK");
    lightLevel = tsl_readLux();
  } else {
    DLOGLN("Sensor luz no detectado — modo simulacion");
    lightLevel = sim_light;
  }
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  tsl_ok = false;  // AGROMETEO usa BH1750, no TSL/APDS
#else
  tsl_ok    = false;
  lightLevel = sim_light;
  DLOGLN("Sensor luz — perfil IRRIGATION, sensor omitido");
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

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  if (soilSensor.begin(4800))
    DLOGLN("SoilSensor RS485 iniciado OK");
  else
    DLOGLN("[WARN] SoilSensor: sin respuesta al arranque — continuando");
#endif

#if DEVICE_PROFILE == PROFILE_METEO
  DLOGLN("Plataforma: LilyGo TTGO T-Display (con pantalla TFT)");
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  DLOGLN("Plataforma: ESP32 4-Relay Board (sin pantalla)");
#else
  DLOGLN("Plataforma: Wemos D1 Mini ESP32 (sin pantalla)");
#endif

  // ── XDB401 — sensor de presión de tubería I2C (todos los perfiles ESP32) ──
  xdb401_ok = xdb401_begin();
  if (xdb401_ok) {
    DLOGLN("XDB401 OK — sensor presion tuberia detectado");
    // Si no hay caudalímetro, activar modo real para aprovechar la presión real
    if (strcmp(pipelineMode, "sim") == 0) strlcpy(pipelineMode, "real", sizeof(pipelineMode));
  } else {
    DLOGLN("XDB401 no detectado — presion tuberia en modo simulacion");
  }
  leakDetector.begin(irrigationType);

#ifdef HAS_DISPLAY
  drawBootScreen("Conectando WiFi...");
#endif

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // ── Conectividad celular (SIM7000G + Onomondo) ─────────────────────────────
  setLedState(LED_WIFI_CONNECTING);
  DLOGLN("[ASR] Iniciando modem SIM7000G...");
  bool simOk = sim7000g_powerOn();
  if (!simOk) {
    DLOGLN("[ASR] WARN: sin GPRS al arranque — networkTask reintentará");
  }

#if defined(USE_MQTT)
  #ifndef DEV_MODE
    // PROD: mqtt_user = serial hardware del dispositivo (MAC eFuse)
    mqtt_user = device_serial_get();
    // PROD: token pre-flasheado en NVS
    if (prov_mqtt_token[0] != '\0') {
      mqtt_pass = prov_mqtt_token;
    } else {
      DLOGLN("[MQTT] ADVERTENCIA: sin token NVS — dispositivo no provisionado de fábrica");
    }
    // finca_id = MAC hex (sin separadores)
    static char _finca_mac[16];
    const char* serial = device_serial_get();  // "AQ-FFFFFFFFFFFF"
    if (strlen(serial) >= 15) {
      strncpy(_finca_mac, serial + 3, 12);  // saltar "AQ-"
      _finca_mac[12] = '\0';
    } else {
      strncpy(_finca_mac, serial, sizeof(_finca_mac) - 1);
      _finca_mac[sizeof(_finca_mac) - 1] = '\0';
    }
    finca_id = _finca_mac;
  #endif
  DLOGF("[MQTT] Auth user: %s  |  Serial: %s\n", mqtt_user, device_serial_get());
#endif

  // Obtener hora desde la red GSM (alternativa a NTP en cellular)
  if (simOk) {
    int yr, mo, dy, hr, mn, sc;
    float tz;
    if (modemSIM.getNetworkTime(&yr, &mo, &dy, &hr, &mn, &sc, &tz)) {
      // Construir epoch aproximado y setear el RTC del sistema
      struct tm tm_info = {};
      tm_info.tm_year = yr - 1900;
      tm_info.tm_mon  = mo - 1;
      tm_info.tm_mday = dy;
      tm_info.tm_hour = hr;
      tm_info.tm_min  = mn;
      tm_info.tm_sec  = sc;
      time_t epoch = mktime(&tm_info);
      struct timeval tv = { epoch, 0 };
      settimeofday(&tv, nullptr);
      DLOGF("[SIM] Hora de red: %04d-%02d-%02d %02d:%02d:%02d\n",
            yr, mo, dy, hr, mn, sc);
    } else {
      DLOGLN("[SIM] WARN: no se pudo obtener hora de red — timestamps no fiables");
    }
  }

  // FreeRTOS: crear tarea de red en Core 0 (gestiona GPRS + MQTT)
  dataMutex      = xSemaphoreCreateMutex();
  telemetryQueue = xQueueCreate(1, sizeof(TelemetrySnapshot));
  xTaskCreatePinnedToCore(
    networkTask, "NetworkTask",
    12288,       // stack ampliado para TLS (TinyGSM + PubSubClient)
    nullptr, 2, &networkTaskHandle, 0
  );
  DLOGLN("[ASR] NetworkTask creada en Core 0");
  printHardwareInfo();

#else
  // ── Conectividad WiFi ───────────────────────────────────────────────────────
  setLedState(LED_WIFI_CONNECTING);
  DLOGLN("Ajustando potencia WiFi...");
  WiFi.setTxPower(WIFI_POWER_18_5dBm); // Potencia ajustada a ~18.5 dBm
  WiFi.mode(WIFI_STA); // Modo estación, más robusto
  DLOGLN("Conectando WiFi...");
  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    DLOG(".");
    tries++;
  }
  DLOGLN();

  if (WiFi.status() == WL_CONNECTED) {
    setLedState(LED_MQTT_CONNECTING);
    DLOGLN("WiFi OK: " + WiFi.localIP().toString());

#if defined(USE_MQTT)
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
    ArduinoOTA.setHostname("meteostation-esp32");
    // Contraseña OTA opcional — definir OTA_PASSWORD en secrets.h para activarla
#ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
    ArduinoOTA.onStart([]() {
      isUpdatingOTA = true;
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
      DLOGF("\n[OTA] Inicio de actualización (%s) — relays a OFF por seguridad\n", type.c_str());
      // Apagar todos los relays por seguridad durante el flash
      for (int i = 0; i < RELAY_COUNT; i++) digitalWrite(RELAY_PINS[i], LOW);
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

#if defined(USE_MQTT)
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

    // FreeRTOS: crear tarea de red en Core 0
    dataMutex      = xSemaphoreCreateMutex();
    telemetryQueue = xQueueCreate(1, sizeof(TelemetrySnapshot));
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
    WiFi.setSleep(true);  // Modem Sleep: ahorra ~15-20mA entre transmisiones
    DLOGLN("OTA listo — hostname: meteostation-esp32");
    // ── Fin OTA ─────────────────────────────────────────────────────────────

    printHardwareInfo();

#ifdef HAS_DISPLAY
    drawBootScreen(("IP: " + WiFi.localIP().toString()).c_str());
#endif
  } else {
#if !defined(DEV_MODE)
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
#endif  // DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE

#ifdef HAS_DISPLAY
  delay(2500);
#endif

#ifdef DEBUG_MODE
  DLOGLN(F("\n====== AQUANTIA BOOT COMPLETO ======"));
  DLOGF("[TEST] Perfil   : %s | Relays: %d\n",
    (DEVICE_PROFILE == PROFILE_METEO)             ? "METEO" :
    (DEVICE_PROFILE == PROFILE_AQUALEAK)          ? "AQUALEAK" :
    (DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE) ? "AQUA_SMART_REMOTE" : "IRRIGATION", RELAY_COUNT);
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  DLOGF("[TEST] GSM/GPRS : %s\n",
    modemSIM.isGprsConnected() ? modemSIM.localIP().toString().c_str() : "SIN CONEXION");
#else
  DLOGF("[TEST] WiFi     : %s\n",
    (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "SIN CONEXION");
#endif
  DLOGF("[TEST] Temp ext : %s\n", temp_ok ? temperatureSourceName() : "SIM (sin sensor)");
  DLOGF("[TEST] Barometro: %s\n", bar_ok ? pressureSourceName() : "SIM (sin sensor)");
#if DEVICE_PROFILE == PROFILE_METEO
  DLOGF("[TEST] HTU2x    : %s\n", htu_ok ? "REAL" : "SIM (sin sensor)");
  DLOGF("[TEST] Luz      : %s\n", tsl_ok ? "REAL" : "SIM (sin sensor)");
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  DLOGF("[TEST] HDC1080  : %s\n", hdc_ok     ? "REAL" : "SIM (sin sensor)");
  DLOGF("[TEST] BH1750   : %s\n", bh1750_ok  ? "REAL" : "SIM (sin sensor)");
#endif
#if DEVICE_PROFILE == PROFILE_METEO
  DLOGF("[TEST] DHT11    : %s\n", dht_ok ? "REAL" : "SIM (sin sensor)");
#endif
  DLOGF("[TEST] Heap     : %ld bytes libres\n", (long)ESP.getFreeHeap());
  // ── Assertions PASS / FAIL ───────────────────────────────────────────────
  {
    // 1. Todos los relays en OFF al arranque (estado seguro)
    bool allRelaysOff = true;
    for (int i = 0; i < RELAY_COUNT; i++) if (relayActive[i]) { allRelaysOff = false; break; }
    DLOGF("[TEST  ] %s — Todos los relays en OFF (estado seguro)\n",
          allRelaysOff ? "PASS" : "FAIL");

    // 2. Boot completado sin crash (uptime > 0)
    bool uptimeOk = (millis() > 0);
    DLOGF("[TEST  ] %s — Boot completado sin crash (uptime > 0)\n",
          uptimeOk ? "PASS" : "FAIL");

    // 3. Sensores en modo SIM cuando no hay hardware conectado
    bool allSim = !temp_ok && !bar_ok && !htu_ok;
    bool anyReal = temp_ok || bar_ok || htu_ok || xdb401_ok;
    DLOGF("[TEST  ] %s — Sensores: %s\n",
          anyReal ? "INFO" : "PASS",
          anyReal ? "hardware detectado (modo REAL)" : "modo SIM cuando no hay hardware conectado");
  }
  DLOGLN(F("[TEST] Iniciando loop() — reporte cada 5s"));
  DLOGLN(F("====================================\n"));
#endif

  // Forzar primera lectura de sensores en el primer ciclo del loop
  // (sin este offset, habría que esperar telemetryIntervalMs antes de tener datos)
  lastSlowSensorRead = millis() - telemetryIntervalMs;
  lastSensorRead      = lastSlowSensorRead;
}

// =============================================================================
// LOOP — Core 1: sensores + display (sin red)
// =============================================================================
void loop() {
  wdt_heartbeat("loopTask");
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
  // Debounce 400ms — evita doble avance por rebote y falsos flancos de GPIO35 flotante
  static unsigned long lastViewChange = 0;
  bool leftEdge  = (!curBtnLeft  && prevBtnLeft);
  bool rightEdge = (!curBtnRight && prevBtnRight);
  if (displayOn && (leftEdge || rightEdge) && (now - lastViewChange >= 400)) {
    displayView = (displayView + 1) % 4;
    lastViewChange = now;
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
    // Sin pantalla (no METEO): sensores de viento simulados
    windSpeed         = sim_windSpeed;
    windSpeedFiltered = sim_windSpeed;
    currentWindDirDeg = sim_windDir;
#endif

    accumulateWindVector(currentWindDirDeg);
    lastWindRead = now;
  }

  // ── 2. Sensores lentos I2C (cada telemetryIntervalMs): MCP9808, BMP280, HTU21, DHT, luz, suelo ──
  // El XDB401 se lee en el bloque 2b con su propio timer más rápido.
  if (now - lastSlowSensorRead >= telemetryIntervalMs) {

#if DEVICE_PROFILE == PROFILE_METEO
    // BMP280 — leer siempre para mandar sus datos explícitos por telemetría
    // Reintento de reinit si el sensor falló en ciclo anterior (cada telemetryIntervalMs = 20 s)
    if (!bmp_ok) {
      bmp_ok = beginBMP280();
      if (bmp_ok) DLOGLN("[BMP280] Reconectado tras fallo");
    }
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
    // Reintento de reinit si el sensor falló en ciclo anterior
    if (!mcp_ok) {
      mcp_ok = tempsensor.begin(0x19);
      if (mcp_ok) { tempsensor.setResolution(3); DLOGLN("[MCP9808] Reconectado tras fallo"); }
    }
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
    // Reintento de reinit si el sensor falló en ciclo anterior
    if (!micropressure_ok) {
      micropressure_ok = barometer.begin();
      if (micropressure_ok) DLOGLN("[MicroPressure] Reconectado tras fallo");
    }
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

#if DEVICE_PROFILE == PROFILE_METEO
    // HTU2x — omitido en IRRIGATION y AGROMETEO
    // Reintento de reinit si el sensor falló en ciclo anterior
    if (!htu_ok) {
      htu_ok = htu_begin();
      if (htu_ok) DLOGLN("[HTU2x] Reconectado tras fallo");
    }
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
#endif  // PROFILE_METEO

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

#if DEVICE_PROFILE == PROFILE_AQUALEAK
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

#endif  // PROFILE_AQUALEAK

#if DEVICE_PROFILE == PROFILE_IRRIGATION
    // ── IRRIGATION: BMP280 + AHT20 + INA219 ─────────────────────────────────

    // BMP280 — temperatura + presión atmosférica
    if (!bmp_ok) {
      bmp_ok = beginBMP280();
      if (bmp_ok) {
        bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                          Adafruit_BMP280::SAMPLING_X2,
                          Adafruit_BMP280::SAMPLING_X16,
                          Adafruit_BMP280::FILTER_X16,
                          Adafruit_BMP280::STANDBY_MS_500);
        DLOGLN("[BMP280] Reconectado tras fallo");
      }
    }
    bmp_temp_ok = bmp_pressure_ok = false;
    if (bmp_ok) {
      float tBmp = NAN, pBmp = NAN;
      if (readBMP280Temperature(tBmp)) { bmpTemperature = tBmp; bmp_temp_ok = true; }
      if (readBMP280PressureKPa(pBmp)) {
        bmpPressure = pBmp; bmp_pressure_ok = true;
        pressure = pBmp; bar_ok = true;
      }
      if (!bmp_temp_ok && !bmp_pressure_ok) {
        bmp_ok = false;
        DLOGLN("BMP280 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!bmp_pressure_ok) { bar_ok = false; pressure = sim_pressure; }

    // AHT20 — temperatura y humedad
    if (!aht20_ok) {
      aht20_ok = aht20_begin();
      if (aht20_ok) DLOGLN("[AHT20] Reconectado tras fallo");
    }
    if (aht20_ok) {
      float t = NAN, h = NAN;
      if (aht20_read(t, h)) {
        temperatureMCP = t;
        humidity       = h;
        temp_ok        = true;
      } else {
        aht20_ok = false;
        DLOGLN("AHT20 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!aht20_ok) {
      if (bmp_temp_ok) { temperatureMCP = bmpTemperature; temp_ok = true; }
      else             { temperatureMCP = sim_tempMCP; temp_ok = false; }
      humidity = sim_humidity;
    }

    // INA219 — voltaje / corriente / potencia
    if (!ina219_ok) {
      ina219_ok = ina219_begin();
      if (ina219_ok) DLOGLN("[INA219] Reconectado tras fallo");
    }
    if (ina219_ok) {
      float v = ina219_readBusVoltage();
      float c = ina219_readCurrent_mA();
      float p = ina219_readPower_mW();
      if (!isnan(v)) ina219BusVoltage = v;
      if (!isnan(c)) ina219Current    = c;
      if (!isnan(p)) ina219Power      = p;
      if (isnan(v) && isnan(c)) { ina219_ok = false; DLOGLN("INA219 fallo en lectura"); }
    }

    DLOGF("[IRRIG] AHT:T=%.1f H=%.1f%%  BMP:T=%.1f P=%.2fkPa  INA:V=%.2f I=%.1fmA P=%.1fmW\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure,
      ina219BusVoltage, ina219Current, ina219Power);
#endif  // PROFILE_IRRIGATION

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    // ── AQUA_SMART_REMOTE: BMP280 + AHT20 + INA219 ──────────────────────────

    if (!bmp_ok) {
      bmp_ok = beginBMP280();
      if (bmp_ok) {
        bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                          Adafruit_BMP280::SAMPLING_X2,
                          Adafruit_BMP280::SAMPLING_X16,
                          Adafruit_BMP280::FILTER_X16,
                          Adafruit_BMP280::STANDBY_MS_500);
        DLOGLN("[BMP280] Reconectado tras fallo");
      }
    }
    bmp_temp_ok = bmp_pressure_ok = false;
    if (bmp_ok) {
      float tBmp = NAN, pBmp = NAN;
      if (readBMP280Temperature(tBmp)) { bmpTemperature = tBmp; bmp_temp_ok = true; }
      if (readBMP280PressureKPa(pBmp)) {
        bmpPressure = pBmp; bmp_pressure_ok = true;
        pressure = pBmp; bar_ok = true;
      }
      if (!bmp_temp_ok && !bmp_pressure_ok) {
        bmp_ok = false;
        DLOGLN("BMP280 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!bmp_pressure_ok) { bar_ok = false; pressure = sim_pressure; }

    if (!aht20_ok) {
      aht20_ok = aht20_begin();
      if (aht20_ok) DLOGLN("[AHT20] Reconectado tras fallo");
    }
    if (aht20_ok) {
      float t = NAN, h = NAN;
      if (aht20_read(t, h)) {
        temperatureMCP = t;
        humidity       = h;
        temp_ok        = true;
      } else {
        aht20_ok = false;
        DLOGLN("AHT20 fallo en lectura — cambiando a simulacion");
      }
    }
    if (!aht20_ok) {
      if (bmp_temp_ok) { temperatureMCP = bmpTemperature; temp_ok = true; }
      else             { temperatureMCP = sim_tempMCP; temp_ok = false; }
      humidity = sim_humidity;
    }

    if (!ina219_ok) {
      ina219_ok = ina219_begin();
      if (ina219_ok) DLOGLN("[INA219] Reconectado tras fallo");
    }
    if (ina219_ok) {
      float v = ina219_readBusVoltage();
      float c = ina219_readCurrent_mA();
      float p = ina219_readPower_mW();
      if (!isnan(v)) ina219BusVoltage = v;
      if (!isnan(c)) ina219Current    = c;
      if (!isnan(p)) ina219Power      = p;
      if (isnan(v) && isnan(c)) { ina219_ok = false; DLOGLN("INA219 fallo en lectura"); }
    }

    DLOGF("[ASR] AHT:T=%.1f H=%.1f%%  BMP:T=%.1f P=%.2fkPa  INA:V=%.2f I=%.1fmA P=%.1fmW\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure,
      ina219BusVoltage, ina219Current, ina219Power);
#endif  // PROFILE_AQUA_SMART_REMOTE

#if DEVICE_PROFILE != PROFILE_AQUALEAK
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
#endif  // PROFILE_AQUALEAK (TSL guard)

// SoilSensor RS485 se lee en su propio bloque adaptativo (fuera del slow block).
    // soilMoisture y halisenseData ya están actualizados por ese bloque.
    // Aquí solo actualizamos soilMoisture desde YL-69 / sim si halisense no está disponible.
#if DEVICE_PROFILE == PROFILE_METEO
    if (!halisenseData.ok) {
#  if defined(SOIL_PIN)
      int raw = analogRead(SOIL_PIN);
      float filtRaw = filteredSoilADC(raw);
      soilMoisture = constrain(
        (float)(SOIL_RAW_DRY - filtRaw) / (SOIL_RAW_DRY - SOIL_RAW_WET) * 100.0f,
        0.0f, 100.0f
      );
#  else
      soilMoisture = sim_soilMoisture;
#  endif
    }
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
    // IRRIGATION: sin YL-69 analógico; soilMoisture viene del halisense o sim
    if (!halisenseData.ok)
      soilMoisture = sim_soilMoisture;
#else
#  if defined(SOIL_PIN)
    {
      int raw = analogRead(SOIL_PIN);
      float filtRaw = filteredSoilADC(raw);
      soilMoisture = constrain(
        (float)(SOIL_RAW_DRY - filtRaw) / (SOIL_RAW_DRY - SOIL_RAW_WET) * 100.0f,
        0.0f, 100.0f
      );
    }
#  else
    soilMoisture = sim_soilMoisture;
#  endif
#endif

    updateSimulatedValues();
    // updatePipelineValues() se llama en bloque 2b cuando hay sensor real.
    // Aquí solo actualizamos si estamos en modo simulación (sin XDB401 activo).
    if (strcmp(pipelineMode, "sim") == 0) updatePipelineValues();

    // Construir snapshot y publicar en la queue para Core 0 — sin mutex, sin bloqueo
    if (telemetryQueue) {
      TelemetrySnapshot snap = {};
      snap.tempMCP       = temperatureMCP;
      snap.pressure      = (float)pressure;
      snap.tempDHT       = temperatureDHT;
      snap.humidity      = humidity;
      snap.windSpeed     = windSpeed;
      snap.windDir       = currentWindDirDeg;
      snap.windSpeedFilt = windSpeedFiltered;
      snap.avgWindDir    = calcAndResetWindVector();
      snap.light         = lightLevel;
      snap.tempDHT11     = temperatureDHT11;
      snap.humDHT11      = humidityDHT11;
      snap.soil          = soilMoisture;
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      snap.halisenseOk   = halisenseData.ok;
      snap.soilTemp      = halisenseData.ok ? halisenseData.temperature : NAN;
      snap.soilEc        = halisenseData.ok ? halisenseData.ec          : NAN;
      snap.soilPh        = halisenseData.ok ? halisenseData.ph          : NAN;
      snap.soilTds       = halisenseData.ok ? halisenseData.tds         : NAN;
      snap.soilN         = halisenseData.ok ? halisenseData.n           : -1;
      snap.soilP         = halisenseData.ok ? halisenseData.p           : -1;
      snap.soilK         = halisenseData.ok ? halisenseData.k           : -1;
      snap.soilIrrigMode = anyRelayActive() ||
                           (soilPostIrrigEndMs != 0 &&
                            (millis() - soilPostIrrigEndMs) < SOIL_POST_IRRIG_MS);
#endif
      snap.bmpTemp       = bmp_temp_ok ? bmpTemperature : NAN;
      snap.bmpPressure   = bmp_pressure_ok ? bmpPressure : NAN;
#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      snap.inaVbus    = ina219_ok ? ina219BusVoltage : NAN;
      snap.inaCurrent = ina219_ok ? ina219Current    : NAN;
      snap.inaPower   = ina219_ok ? ina219Power      : NAN;
#endif
      snap.pipePressure  = sim_pipeline_pressure;
      snap.pipeFlow      = sim_pipeline_flow;
#if defined(FLOW_PIN)
      // Leer contadores con portENTER_CRITICAL para coherencia en dual-core.
      uint32_t totalPulses, sessionBase;
      portENTER_CRITICAL(&_flowMux);
      totalPulses = _flowPulseTotal;
      sessionBase = _flowSessionBase;
      portEXIT_CRITICAL(&_flowMux);
      uint32_t irrigPulses, leakPulses;
      portENTER_CRITICAL(&_flowMux);
      irrigPulses = _flowIrrigPulses;
      leakPulses  = _flowLeakPulses;
      portEXIT_CRITICAL(&_flowMux);
      snap.flowTotalL   = totalPulses / (float)FLOW_K_FACTOR;
      snap.flowSessionL = (totalPulses - sessionBase) / (float)FLOW_K_FACTOR;
      snap.flowIrrigL   = irrigPulses / (float)FLOW_K_FACTOR;
      snap.flowLeakL    = leakPulses  / (float)FLOW_K_FACTOR;
#else
      snap.flowTotalL   = 0.0f;
      snap.flowSessionL = 0.0f;
      snap.flowIrrigL   = 0.0f;
      snap.flowLeakL    = 0.0f;
#endif
      snap.xdb401Temp    = xdb401Temperature;
      snap.relayMask     = 0;
      // relayActive[] es escrito por Core 0 (MQTT callback) — mutex breve solo aquí
      if (dataMutex && xSemaphoreTake(dataMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int i = 0; i < RELAY_COUNT; i++)
          if (relayActive[i]) snap.relayMask |= (1 << i);
        xSemaphoreGive(dataMutex);
      }
#if DEVICE_PROFILE == PROFILE_AQUALEAK
      snap.dewPoint  = agroDewPoint;
      snap.heatIndex = agroHeatIndex;
      snap.absHum    = agroAbsHum;
#endif
      xQueueOverwrite(telemetryQueue, &snap);
    }

#if DEVICE_PROFILE == PROFILE_AQUALEAK
    DLOGF("[sensor] HDC:T=%.1f H=%.1f%% | BMP:T=%.1f P=%.2fkPa | BH:%.1flx | Dp=%.1f Hi=%.1f Ah=%.2f\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure,
      lightLevel, agroDewPoint, agroHeatIndex, agroAbsHum);
#else
    DLOGF("[sensor] T:%.1f Tb:%.1f H:%.1f D11T:%.1f D11H:%.1f P:%.2f W:%.2f D:%.0f Lux:%.1f Soil:%.1f%%\n",
      temperatureMCP, temperatureDHT, humidity,
      temperatureDHT11, humidityDHT11,
      (float)pressure, windSpeedFiltered, currentWindDirDeg, lightLevel, soilMoisture);
#endif

    lastSlowSensorRead = now;
    lastSensorRead      = now;
  }

  // ── 2c. SoilSensor RS485 — muestreo adaptativo según estado de riego ────────────
  // Rápido (3 s) cuando hay riego activo o en la ventana post-riego (2 min).
  // Lento (20 s) en reposo. Permite detectar si el suelo se está mojando al regar.
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  {
    static bool prevRelayOn = false;
    bool relayOn = anyRelayActive();

    // Detectar flanco OFF → iniciar ventana post-riego
    if (!relayOn && prevRelayOn) soilPostIrrigEndMs = now;
    // Resetear ventana si el riego vuelve a activarse
    if (relayOn) soilPostIrrigEndMs = 0;
    prevRelayOn = relayOn;

    unsigned long soilInterval = soilSlowIntervalMs;
    if (relayOn) {
      soilInterval = soilFastIntervalMs;
    } else if (soilPostIrrigEndMs != 0) {
      if (now - soilPostIrrigEndMs < SOIL_POST_IRRIG_MS)
        soilInterval = soilFastIntervalMs;
      else
        soilPostIrrigEndMs = 0;  // ventana expirada
    }

    if (now - lastSoilReadMs >= soilInterval) {
      lastSoilReadMs = now;
      wdt_heartbeat("loopTask", "soil_rs485");
      if (soilSensor.readAllVariables()) {
        halisenseData.ok          = true;
        halisenseData.moisture    = soilSensor.getHumidity();
        halisenseData.temperature = soilSensor.getTemperature();
        float ecRaw               = soilSensor.getEC();   // µS/cm
        halisenseData.ec          = ecRaw / 1000.0f;      // → dS/m
        halisenseData.tds         = ecRaw * 0.5f;         // → ppm
        halisenseData.ph          = soilSensor.getPH();
        halisenseData.n           = (int)soilSensor.getNitrogen();
        halisenseData.p           = (int)soilSensor.getPhosphorus();
        halisenseData.k           = (int)soilSensor.getPotassium();
        soilMoisture              = halisenseData.moisture;
        DLOGF("[SOIL] Hum=%.1f%% T=%.1f°C pH=%.1f N=%d P=%d K=%d [%s]\n",
              halisenseData.moisture, halisenseData.temperature, halisenseData.ph,
              halisenseData.n, halisenseData.p, halisenseData.k,
              relayOn ? "RIEGO" : (soilPostIrrigEndMs != 0 ? "POST-RIEGO" : "REPOSO"));
      } else {
        halisenseData.ok = false;
        DLOGLN("[SOIL] Sin respuesta del sensor RS485");
      }
    }
  }
#endif  // HAS_SOIL_SENSOR

  // ── 2b. XDB401 + caudalímetro — timer propio a PIPELINE_FAST_MS (200 ms) ────────
  // Independiente de los sensores lentos. En modo real actualiza display, LeakDetector
  // y genera alertas de fuga/rotura sin esperar el ciclo de telemetría completo.
  // La telemetría MQTT sigue al ritmo de telemetryIntervalMs (toma el último valor).
  if (strcmp(pipelineMode, "real") == 0 && (now - lastPipelineFastRead >= PIPELINE_FAST_MS)) {
    float fp = NAN, ff = 0.0f;
    if (readRealPipelineSensors(fp, ff)) {
      sim_pipeline_flow     = max(0.0f, ff);
      pipelineFlowOk        = true;
      if (!isnan(fp)) {
        sim_pipeline_pressure = fp;         // permitir negativos (vacío, golpe de ariete)
        pipelinePressureOk    = true;
        pipelineSource        = "real";
      } else {
        // Sin presión real — actualizar solo la sim de presión
        float savedFlow = sim_pipeline_flow;
        updatePipelineSimValues();
        sim_pipeline_flow  = savedFlow;
        pipelinePressureOk = false;
        pipelineSource     = "real_flow";
      }
      // Detección de fugas a 5 Hz — respuesta ~200 ms
      leakDetector.update(sim_pipeline_pressure, sim_pipeline_flow, anyRelayActive());
      strlcpy(pipelineScenario, leakDetector.scenario(), sizeof(pipelineScenario));
    } else {
      pipelineSource     = "fallback";
      pipelinePressureOk = false;
      pipelineFlowOk     = false;
    }
    lastPipelineFastRead = now;
  }

  // ── 3. Refresco de pantalla (cada SCREEN_MS = 1s, solo si hay display) ────────
#ifdef HAS_DISPLAY
  if (now - lastScreenTime >= SCREEN_MS) {
    if      (displayView == 1) drawPipelineScreen();
    else if (displayView == 2) drawInfoScreen();
    else if (displayView == 3) drawSueloScreen(
      halisenseData.ok,
      soilMoisture,
      halisenseData.ok ? halisenseData.temperature : NAN,
      halisenseData.ok ? halisenseData.ph          : NAN,
      halisenseData.ok ? halisenseData.n           : 0,
      halisenseData.ok ? halisenseData.p           : 0,
      halisenseData.ok ? halisenseData.k           : 0
    );
    else                       drawScreen();
    lastScreenTime = now;
  }
#endif

  // ── DEBUG: reporte completo de estado cada DEBUG_INTERVAL_MS ─────────────────
#ifdef DEBUG_MODE
  static unsigned long lastDebugReport = 0;
  if (now - lastDebugReport >= DEBUG_INTERVAL_MS) {
    lastDebugReport = now;

    // ── Datos variables ──────────────────────────────────────────────────────
    unsigned long up = millis() / 1000;
    DLOGF("[STATUS] %luh%02lum%02lus  Heap:%ld B  RSSI:%d dBm\n",
      up / 3600, (up % 3600) / 60, up % 60,
      (long)ESP.getFreeHeap(), WiFi.RSSI());

#if DEVICE_PROFILE == PROFILE_AQUALEAK
    DLOGF("[DATOS ] HDC:T=%.1f C  H=%.1f%%  BMP:T=%.1f C  P=%.2f kPa  BH:%.1f lx\n",
      temperatureMCP, humidity, bmpTemperature, (float)pressure, lightLevel);
    DLOGF("[AGROCALC] Dp=%.1f C  HI=%.1f C  AH=%.2f g/m3\n",
      agroDewPoint, agroHeatIndex, agroAbsHum);
#elif DEVICE_PROFILE == PROFILE_METEO
    DLOGF("[DATOS ] T:%.1f C  H:%.1f%%  P:%.2f kPa  T_DHT:%.1f C  H_DHT:%.1f%%  Lux:%.1f lx  Suelo:%.1f%%\n",
      temperatureMCP, humidity, (float)pressure, temperatureDHT11, humidityDHT11, lightLevel, soilMoisture);
    DLOGF("[VIENTO] %.1f m/s (filt:%.1f)  Dir:%.0f° (%s)\n",
      windSpeed, windSpeedFiltered, currentWindDirDeg, degToCompass(currentWindDirDeg));
#if defined(FLOW_PIN)
    DLOGF("[FLOW  ] %.2f L/min  Total:%lu p  Intervalo:%lu us\n",
      _flowLpm, (unsigned long)_flowPulseTotal, (unsigned long)_flowLastDtUs);
    if (_flowPulseTotal == 0)
      DLOGLN("[WARN  ] Caudalimetro: sin pulsos — verif. cableado GPIO" + String(FLOW_PIN));
#endif
#endif
    DLOGF("[PIPE  ] %.3f bar  %.2f L/min  fuente:%s  escenario:%s\n",
      sim_pipeline_pressure, sim_pipeline_flow, pipelineSource.c_str(), pipelineScenario);

    // ── Alertas de estado — solo imprime si hay algo anómalo ─────────────────
    if (WiFi.status() != WL_CONNECTED)
      DLOGLN("[WARN  ] WiFi DESCONECTADO");
#if defined(USE_MQTT)
    if (!mqttClient.connected())
      DLOGLN("[WARN  ] MQTT DESCONECTADO");
#endif
#if DEVICE_PROFILE == PROFILE_METEO
    if (!temp_ok)         DLOGLN("[WARN  ] Temperatura exterior: SIM (sin sensor real)");
    if (!bar_ok)          DLOGLN("[WARN  ] Barometro: SIM (sin sensor real)");
    if (!htu_ok)          DLOGLN("[WARN  ] HTU2x: SIM (sin sensor real)");
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
    if (!hdc_ok)          DLOGLN("[WARN  ] HDC1080: SIM (sin sensor real)");
    if (!bmp_ok)          DLOGLN("[WARN  ] BMP280: SIM (sin sensor real)");
    if (!bh1750_ok)       DLOGLN("[WARN  ] BH1750: SIM (sin sensor real)");
#endif
    if (!xdb401_ok)       DLOGLN("[WARN  ] XDB401: sin respuesta — presion tuberia no disponible");
    for (int i = 0; i < RELAY_COUNT; i++)
      if (relayActive[i]) DLOGF("[WARN  ] Relay R%d ACTIVO\n", i);
    if (strcmp(pipelineScenario, "normal") != 0)
      DLOGF("[WARN  ] Pipeline escenario: %s\n", pipelineScenario);
  }
#endif  // DEBUG_MODE
}
