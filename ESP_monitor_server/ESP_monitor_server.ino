// MeteoStation — Firmware v3
// ESP32 (con pantalla ST7789 240×135)
// Sensores: MCP9808, BMP280, HTU2x, SparkFun MicroPressure, TSL2584/APDS, anemómetro, veleta
// Tres temporizadores independientes: 100ms viento / 1s pantalla / 20s envío

// ── Versión del firmware ───────────────────────────────────────────────────────
// Incrementar según SemVer al crear un release. El backend almacena este valor
// en device_info.firmware_version para mostrar en el dashboard y detectar
// dispositivos desactualizados (comparado con app_settings.min_firmware_version).
#define FIRMWARE_VERSION "0.3.0-beta"

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
  // AQUA_SMART_REMOTE usa conectividad celular — sin WiFi.
  // SIM7000SSL no se usa: el stack SSL hardware del R1529 no soporta authmode/cacert.
  // TLS lo gestiona el ESP32 (BearSSL via SSLClient) sobre TCP plano.
  #define TINY_GSM_MODEM_SIM7000
  #include <TinyGsmClient.h>
  #include <SSLClient.h>
  #include "trust_anchors.h"
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
  #define FLOW_PIN         34   // Caudalímetro — GPIO34, ADC1_CH6, solo entrada; requiere pull-up externo
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
  #include "SoilProvisioner.h"
  #include "halisense_sensor.h"
#elif DEVICE_PROFILE == PROFILE_IRRIGATION
  // IRRIGATION: RS485 Helissense + INA219 + AHT20 + BMP280
  #include <Wire.h>
  #include <Adafruit_BMP280.h>
  #include "SoilSensor.h"
  #include "SoilProvisioner.h"
  #include "halisense_sensor.h"
  #include "aht20_driver.h"
  #include "ina219_driver.h"
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  // AQUA_SMART_REMOTE: mismo sensor suite que IRRIGATION + conectividad celular SIM7000G
  #include <Wire.h>
  #include <Adafruit_BMP280.h>
  #include "SoilSensor.h"
  #include "SoilProvisioner.h"
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
  #include "aht20_driver.h"   // AHT21 — fallback T+RH si HDC1080 no presente (protocolo idéntico)
#else
  // IRRIGATION: solo I2C básico (sin sensores meteo)
  #include <Wire.h>
#endif
#include <ArduinoJson.h>
#include <math.h>
// Nota de flasheo: la configuracion activa sale solo de "secrets.h".
// Archivos de respaldo (p. ej. "secrets - copia.h") no forman parte del build.
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
static TinyGsmClient  gsmTCPClient(modemSIM, 0);  // TCP puro (puerto 1883 / base para TLS)
static TinyGsmClient  _gsmTCPBase (modemSIM, 1);  // canal independiente para SSLClient
// BearSSL en el ESP32: TLS gestionado por el MCU, no por el SIM7000G.
// A0 = pin analógico flotante para semilla de entropía.
static SSLClient      gsmTLSClient(_gsmTCPBase, TAs, TAs_NUM, A0);
// ── Cache de estado GSM para lectura segura desde Core 1 ─────────────────────
// TinyGSM NO es thread-safe: NUNCA llamar a modemSIM.* desde loop() (Core 1)
// mientras NetworkTask (Core 0) pueda estar usando Serial1.
// NetworkTask actualiza estas variables; loop() solo las lee.
static volatile int  _simCsqCache       = 0;   // último CSQ conocido (0-31, 99=inválido)
static volatile bool _gprsConnectedFlag = false; // true cuando GPRS está activo
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
bool aht20_ok      = false;   // AHT21   — fallback T+RH si HDC1080 no detectado (0x38)
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
bool tsl_ok    = false;
bool ens160_ok = false;   // ENS160 — sensor IAQ (TVOC, eCO2, AQI)
bool xdb401_ok = false;   // XDB401 — sensor de presión de tubería I2C
static uint8_t       xdb401_failures  = 0;       // fallos consecutivos de lectura
static uint8_t       xdb401_recovery_failures = 0; // fallos consecutivos de recuperación
static unsigned long xdb401_retry_at  = 0;        // millis() cuando intentar reinit
static constexpr uint8_t  XDB401_MAX_FAILURES  = 8;    // más tolerante con cable largo (~1 m)
static constexpr uint32_t XDB401_RETRY_INTERVAL = 15000UL;  // reintento más rápido tras recovery
static constexpr uint8_t  XDB401_MAX_RECOVERY_FAILURES = 240; // 1 h de reintentos (240 × 15 s)
static constexpr uint32_t XDB401_RECOVERY_COOLDOWN = 86400000UL; // 24 h


#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK \
 || DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static uint8_t       bmp_recovery_failures = 0;
static unsigned long bmp_retry_at = 0;
#endif

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK
static uint8_t       micropressure_recovery_failures = 0;
static unsigned long micropressure_retry_at = 0;
#endif

#if DEVICE_PROFILE == PROFILE_METEO
static uint8_t       mcp_recovery_failures = 0;
static unsigned long mcp_retry_at = 0;
static uint8_t       htu_recovery_failures = 0;
static unsigned long htu_retry_at = 0;
#endif

#if DEVICE_PROFILE == PROFILE_AQUALEAK
static uint8_t       hdc_recovery_failures = 0;
static unsigned long hdc_retry_at = 0;
static uint8_t       bh1750_recovery_failures = 0;
static unsigned long bh1750_retry_at = 0;
#endif

#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE \
 || DEVICE_PROFILE == PROFILE_AQUALEAK
static uint8_t       aht20_recovery_failures = 0;
static unsigned long aht20_retry_at = 0;
#endif
#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static uint8_t       ina219_recovery_failures = 0;
static unsigned long ina219_retry_at = 0;
#endif

#if DEVICE_PROFILE != PROFILE_AQUALEAK
static uint8_t       tsl_recovery_failures = 0;
static unsigned long tsl_retry_at = 0;
#endif

// ENS160 — todos los perfiles (detección en runtime)
static uint8_t       ens160_recovery_failures = 0;
static unsigned long ens160_retry_at = 0;

#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static uint8_t       soil_rs485_recovery_failures = 0;
static unsigned long soil_rs485_retry_at = 0;
#endif



#include "hdc1080_driver.h"

#include "pressure_sensor_i2c.h"
#include "sensor_recovery.h"


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
uint8_t  ens160Aqi  = 0;    // UBA Air Quality Index (1-5), 0 = no disponible
uint16_t ens160Tvoc = 0;    // TVOC en ppb
uint16_t ens160Eco2 = 0;    // eCO2 en ppm eq.
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

#include "pipeline_core.h"

#include "htu2x_driver.h"
#include "light_sensor.h"
#include "ens160_driver.h"


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
  // ENS160 — calidad de aire (todos los perfiles, NaN/0 si no detectado)
  uint8_t  ensAqi;    // UBA Air Quality Index 1-5 (0 = no disponible)
  uint16_t ensTvoc;   // TVOC en ppb
  uint16_t ensEco2;   // eCO2 en ppm eq.
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
#if DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  float inaVbus, inaCurrent, inaPower;  // INA219
#endif
} _netSnap;

#include "wind_sensor.h"

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

#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
#include "http_client.h"
#endif
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
#include "gsm_modem.h"
#endif

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
#include "network_task.h"
#include "sensor_read.h"

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
    (DEVICE_PROFILE == PROFILE_AQUALEAK) ? "AQUALEAK" :
    (DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE) ? "AQUA_SMART_REMOTE" : "IRRIGATION", DEVICE_PROFILE);
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

  if (provisioning_check_ap_forced() || !provisioning_has_credentials()) {
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
  // GPIO 34-39 son input-only en ESP32 — sin pull-up interno; el circuito BC547 provee el pull-up externo.
  // METEO (GPIO32) y AQUALEAK (GPIO17) sí soportan pull-up interno.
  #if FLOW_PIN >= 34
  pinMode(FLOW_PIN, INPUT);
  #else
  pinMode(FLOW_PIN, INPUT_PULLUP);
  #endif
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
    DLOGLN("HDC1080 no detectado — probando AHT21 en 0x38");
    aht20_ok = aht20_begin();
    if (aht20_ok) {
      float t = NAN, h = NAN;
      if (aht20_read(t, h)) {
        temperatureMCP = t;
        humidity       = h;
        temp_ok        = true;
        DLOGF("AHT21 OK — T:%.1f C  H:%.1f %%\n", t, h);
      } else {
        aht20_ok = false;
      }
    }
    if (!aht20_ok) {
      DLOGLN("AHT21 no detectado — modo simulacion");
      temperatureMCP = sim_tempMCP;
      humidity       = sim_humidity;
    }
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
  {
    uint8_t savedSoilAddr = soilBusLoadAddress();
    if (savedSoilAddr > 0) {
      soilSensor.setSlaveAddress(savedSoilAddr);
      DLOGF("[SOIL] Dirección cargada de NVS: 0x%02X\n", savedSoilAddr);
    }
  }
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

  // ── ENS160 — sensor IAQ (todos los perfiles, detección en runtime) ──────────
  // Placa ENS160+AHT21 (AliExpress): ENS160 en 0x52, AHT21 en 0x38.
  // AHT21 @ 0x38 es protocolo-compatible con AHT20 — detectado automáticamente
  // en perfiles IRRIGATION/AQUA_SMART_REMOTE por aht20_driver.h.
  ens160_ok = ens160_begin();
  if (ens160_ok) {
    // Primera compensación con los valores de T/HR ya leídos en este boot
    ens160_set_compensation(temperatureMCP, humidity);
    uint8_t aqi = 0; uint16_t tvoc = 0, eco2 = 0;
    if (ens160_read(aqi, tvoc, eco2)) {
      ens160Aqi  = aqi;
      ens160Tvoc = tvoc;
      ens160Eco2 = eco2;
      DLOGF("ENS160 OK — AQI:%d  TVOC:%d ppb  eCO2:%d ppm\n", aqi, tvoc, eco2);
    } else {
      DLOGLN("ENS160 OK (calentando — lecturas iniciales no disponibles aun)");
    }
  } else {
    DLOGLN("ENS160 no detectado");
  }

  // ── Resumen de sensores detectados ──────────────────────────────────────────
  DLOGLN("\n=== Resumen de sensores ===");
  // Temperatura
#if DEVICE_PROFILE == PROFILE_METEO
  DLOGF("  Temperatura  : %s%s%s\n",
    mcp_ok      ? "[MCP9808] " : "",
    bmp_temp_ok ? "[BMP280]  " : "",
    (!mcp_ok && !bmp_temp_ok) ? "[SIM]" : "");
  DLOGF("  -> Usando    : %s\n", temperatureSourceName());
  DLOGF("  Humedad      : %s%s\n",
    htu_ok ? "[HTU2x]" : "",
    !htu_ok ? "[SIM]"  : "");
  DLOGF("  Presion atm  : %s%s%s\n",
    micropressure_ok ? "[MicroPressure] " : "",
    bmp_pressure_ok  ? "[BMP280] "        : "",
    (!micropressure_ok && !bmp_pressure_ok) ? "[SIM]" : "");
  DLOGF("  -> Usando    : %s\n", pressureSourceName());
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
  DLOGF("  Temperatura  : %s%s%s\n",
    hdc_ok      ? "[HDC1080] " : "",
    aht20_ok    ? "[AHT21]   " : "",
    bmp_temp_ok ? "[BMP280]  " : "");
  if (!hdc_ok && !aht20_ok && !bmp_temp_ok) DLOGLN("  Temperatura  : [SIM]");
  DLOGF("  -> Usando    : %s\n", temperatureSourceName());
  DLOGF("  Humedad      : %s\n", hdc_ok ? "[HDC1080]" : aht20_ok ? "[AHT21]" : "[SIM]");
  DLOGF("  Presion atm  : %s%s%s\n",
    micropressure_ok ? "[MicroPressure] " : "",
    bmp_pressure_ok  ? "[BMP280] "        : "",
    (!micropressure_ok && !bmp_pressure_ok) ? "[SIM]" : "");
  DLOGF("  -> Usando    : %s\n", pressureSourceName());
  DLOGF("  Luz          : %s\n", bh1750_ok ? "[BH1750]" : "[SIM]");
#elif DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  DLOGF("  Temperatura  : %s%s\n",
    aht20_ok    ? "[AHT20/21] " : "",
    bmp_temp_ok ? "[BMP280]  "  : "");
  if (!aht20_ok && !bmp_temp_ok) DLOGLN("  Temperatura  : [SIM]");
  DLOGF("  -> Usando    : %s\n", temperatureSourceName());
  DLOGF("  Humedad      : %s\n", aht20_ok ? "[AHT20/21]" : "[SIM]");
  DLOGF("  Presion atm  : %s%s\n",
    bmp_pressure_ok ? "[BMP280]" : "",
    !bmp_pressure_ok ? "[SIM]"   : "");
  DLOGF("  -> Usando    : %s\n", pressureSourceName());
  DLOGF("  Corriente    : %s\n", ina219_ok ? "[INA219]" : "[no]");
#endif
  DLOGF("  Presion tubo : %s\n", xdb401_ok ? "[XDB401]" : "[SIM]");
  DLOGF("  Calidad aire : %s\n", ens160_ok ? "[ENS160]" : "[no detectado]");
  DLOGLN("===========================\n");

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
#ifdef USE_MQTT
  // Inicializar cache de estado celular antes de arrancar NetworkTask para que
  // los logs de Core 1 no muestren CSQ=0/GPRS=DOWN durante el setup inicial TLS.
  _gprsConnectedFlag = simOk;
  _simCsqCache = simOk ? modemSIM.getSignalQuality() : 0;
#endif

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
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
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
    DLOGLN("OTA listo — hostname: " DEVICE_HOSTNAME);
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
  DLOGF("[TEST] GSM/GPRS : %s (CSQ:%d)\n",
    _gprsConnectedFlag ? "OK" : "SIN CONEXION", (int)_simCsqCache);
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
  DLOGF("[TEST] HDC1080  : %s\n", hdc_ok    ? "REAL" : "SIM (sin sensor)");
  DLOGF("[TEST] AHT21    : %s\n", aht20_ok  ? "REAL (fallback T+RH)" : (hdc_ok ? "no usado" : "SIM (sin sensor)"));
  DLOGF("[TEST] BH1750   : %s\n", bh1750_ok ? "REAL" : "SIM (sin sensor)");
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
  readSlowSensors(now);

  readSoilSensor(now);

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
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    // Usar cache — NO llamar a modemSIM.* desde Core 1 (race condition con NetworkTask)
    DLOGF("[STATUS] %luh%02lum%02lus  Heap:%ld B  CSQ:%d  GPRS:%s\n",
      up / 3600, (up % 3600) / 60, up % 60,
      (long)ESP.getFreeHeap(), (int)_simCsqCache,
      _gprsConnectedFlag ? "OK" : "DOWN");
#else
    DLOGF("[STATUS] %luh%02lum%02lus  Heap:%ld B  RSSI:%d dBm\n",
      up / 3600, (up % 3600) / 60, up % 60,
      (long)ESP.getFreeHeap(), WiFi.RSSI());
#endif

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
    noInterrupts();
    uint32_t _dbgFlowTotal = _flowPulseTotal;
    interrupts();
    DLOGF("[FLOW  ] %.2f L/min  Total:%lu p  Intervalo:%lu us\n",
      _flowLpm, (unsigned long)_dbgFlowTotal, (unsigned long)_flowLastDtUs);
    if (_dbgFlowTotal == 0)
      DLOGLN("[WARN  ] Caudalimetro: sin pulsos — verif. cableado GPIO" + String(FLOW_PIN));
#endif
#endif
    DLOGF("[PIPE  ] %.3f bar  %.2f L/min  fuente:%s  escenario:%s\n",
      sim_pipeline_pressure, sim_pipeline_flow, pipelineSource.c_str(), pipelineScenario);

    // ── Alertas de estado — solo imprime si hay algo anómalo ─────────────────
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    // Usar flag cache — NO llamar a modemSIM.* desde Core 1
    if (!_gprsConnectedFlag)
      DLOGLN("[WARN  ] GPRS DESCONECTADO");
#else
    if (WiFi.status() != WL_CONNECTED)
      DLOGLN("[WARN  ] WiFi DESCONECTADO");
#endif
#if defined(USE_MQTT)
    if (!mqttClient.connected())
      DLOGLN("[WARN  ] MQTT DESCONECTADO");
#endif
#if DEVICE_PROFILE == PROFILE_METEO
    if (!temp_ok)         DLOGLN("[WARN  ] Temperatura exterior: SIM (sin sensor real)");
    if (!bar_ok)          DLOGLN("[WARN  ] Barometro: SIM (sin sensor real)");
    if (!htu_ok)          DLOGLN("[WARN  ] HTU2x: SIM (sin sensor real)");
#elif DEVICE_PROFILE == PROFILE_AQUALEAK
    if (!hdc_ok && !aht20_ok) DLOGLN("[WARN  ] HDC1080/AHT21: SIM (sin sensor T+RH real)");
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
