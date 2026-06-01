// wind_sensor.h — Filtros ADC del anemómetro, conversión a m/s y grados,
// y promedio vectorial de dirección de viento.
// Incluido desde ESP_monitor_server.ino después de los driver includes y
// después de declarar windMux (portMUX_TYPE, en la sección FreeRTOS del .ino).
#pragma once

// ── Filtro media móvil ────────────────────────────────────────────────────────
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

// ── Conversión ADC → velocidad y dirección ────────────────────────────────────
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

// ── Promedio vectorial de dirección de viento ─────────────────────────────────
// windMux declarado en el .ino (sección FreeRTOS) — portMUX_TYPE global.
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
