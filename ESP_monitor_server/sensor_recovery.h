// sensor_recovery.h — Lógica de recuperación de sensores, helpers BMP280/XDB401
// y funciones de selección de fuente de temperatura/presión.
// Incluido desde ESP_monitor_server.ino después de declarar las flags de sensor,
// los contadores de recovery y los includes de pressure_sensor_i2c.h/hdc1080_driver.h.
#pragma once

// ── Constantes de recuperación genérica ──────────────────────────────────────
static constexpr uint8_t  SENSOR_RECOVERY_MAX_FAILURES    = 4;
static constexpr uint32_t SENSOR_RECOVERY_RETRY_INTERVAL  = 15000UL;
static constexpr uint32_t SENSOR_RECOVERY_COOLDOWN        = 300000UL;

static void sensorRecoveryMarkSuccess(uint8_t& recoveryFailures, unsigned long& retryAt) {
  recoveryFailures = 0;
  retryAt = 0;
}

static void sensorRecoveryMarkFailure(const char* sensorTag,
                                      uint8_t& recoveryFailures,
                                      unsigned long& retryAt) {
  if (recoveryFailures < 255) recoveryFailures++;

  if (recoveryFailures >= SENSOR_RECOVERY_MAX_FAILURES) {
    retryAt = millis() + SENSOR_RECOVERY_COOLDOWN;
    recoveryFailures = 0;
    DLOGF("[%s] Recuperacion fallida repetida — enfriamiento %lus antes de nuevo intento\n",
          sensorTag, (unsigned long)SENSOR_RECOVERY_COOLDOWN / 1000UL);
  } else {
    retryAt = millis() + SENSOR_RECOVERY_RETRY_INTERVAL;
  }
}

// ── XDB401 — escalado de cooldown tras recovery fallido ───────────────────────
static void xdb401_schedule_retry_after_recovery_fail() {
  if (xdb401_recovery_failures < 255) xdb401_recovery_failures++;

  if (xdb401_recovery_failures >= XDB401_MAX_RECOVERY_FAILURES) {
    xdb401_retry_at = millis() + XDB401_RECOVERY_COOLDOWN;
    xdb401_recovery_failures = 0;
    DLOGF("[XDB401] Recuperacion fallida repetida — enfriamiento %lus antes de nuevo intento\n",
          (unsigned long)XDB401_RECOVERY_COOLDOWN / 1000UL);
  } else {
    xdb401_retry_at = millis() + XDB401_RETRY_INTERVAL;
  }
}

// ── BMP280 — helpers de init y lectura ───────────────────────────────────────
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_AQUALEAK \
 || DEVICE_PROFILE == PROFILE_IRRIGATION || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
static bool beginBMP280() {
  if (bmp280.begin(0x76)) { bmp280_addr = 0x76; return true; }
  if (bmp280.begin(0x77)) { bmp280_addr = 0x77; return true; }
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
#endif

// ── XDB401 — init y lectura ──────────────────────────────────────────────────
// Detecta el sensor en el bus I2C ya inicializado por setup().
// En el 2.º y 3.er intento ejecuta bus recovery (9 pulsos SCL + STOP).
static bool xdb401_begin() {
  pressureSensor_init(I2C_SDA, I2C_SCL, 100000);

  for (int i = 0; i < 3; i++) {
    if (i > 0) {
      DLOGF("[XDB401] Reintento %d — recuperando bus I2C\n", i);
      pressureSensor_recover();
      delay(30);
    }
    if (pressureSensor_isPresent()) {
      DLOGF("[XDB401] Detectado en 0x%02X (intento %d)\n", PRESSURE_SENSOR_I2C_ADDR, i + 1);
      return true;
    }
  }
  return false;
}

static bool xdb401_read(float& pressureBar, float& temperatureC) {
  pressureBar  = NAN;
  temperatureC = NAN;

  PressureSensorData_t data;
  if (!pressureSensor_read(&data) || !data.valid) {
    DLOGLN("[XDB401] Read FAIL");
    return false;
  }

  float pb = data.pressure_kpa / 100.0f;
  float tc = data.temperature_c;

  static unsigned long _lastXdbPrint = 0;
  unsigned long _now = millis();
  if (_now - _lastXdbPrint >= DEBUG_INTERVAL_MS) {
    _lastXdbPrint = _now;
    DLOGF("[XDB401] Presion=%.3f bar  Temp=%.1f C\n", pb, tc);
  }

  float fs_bar = PRESSURE_SENSOR_FULLSCALE / 100.0f;
  bool ok = (pb >= -0.5f && pb <= fs_bar * 1.05f && tc > -10.0f && tc < 125.0f);
  if (!ok) {
    DLOGF("[XDB401] Validacion FAIL — pb=%.3f bar (max=%.1f) tc=%.1f C\n", pb, fs_bar, tc);
    return false;
  }

  pressureBar  = pb;
  temperatureC = tc;
  return true;
}

static float xdb401_readPressureBar() {
  float pb, tc;
  return xdb401_read(pb, tc) ? pb : NAN;
}

// ── Selección de fuente activa ────────────────────────────────────────────────
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
