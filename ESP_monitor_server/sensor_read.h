// sensor_read.h — Lectura periódica de sensores lentos I2C y sensor de suelo RS485.
// Incluido desde ESP_monitor_server.ino después de network_task.h.
// Requiere: todos los globales de sensores, snapshot y FreeRTOS declarados en .ino.
#pragma once

// ── Sensores lentos I2C: MCP9808, BMP280, HTU21, DHT, luz ────────────────────
// Llamado desde loop() cada telemetryIntervalMs.
// Construye el TelemetrySnapshot y lo publica en la queue para Core 0.
void readSlowSensors(unsigned long now) {
  if (now - lastSlowSensorRead < telemetryIntervalMs) return;

#if DEVICE_PROFILE == PROFILE_METEO
  // BMP280 — temperatura + presión barométrica
  if (!bmp_ok && now >= bmp_retry_at) {
    bmp_ok = beginBMP280();
    if (bmp_ok) {
      sensorRecoveryMarkSuccess(bmp_recovery_failures, bmp_retry_at);
      DLOGLN("[BMP280] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("BMP280", bmp_recovery_failures, bmp_retry_at);
    }
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

  // MCP9808 — temperatura exterior (fallback BMP280)
  if (!mcp_ok && now >= mcp_retry_at) {
    mcp_ok = tempsensor.begin(0x19);
    if (mcp_ok) {
      sensorRecoveryMarkSuccess(mcp_recovery_failures, mcp_retry_at);
      tempsensor.setResolution(3);
      DLOGLN("[MCP9808] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("MCP9808", mcp_recovery_failures, mcp_retry_at);
    }
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
  if (!micropressure_ok && now >= micropressure_retry_at) {
    micropressure_ok = barometer.begin();
    if (micropressure_ok) {
      sensorRecoveryMarkSuccess(micropressure_recovery_failures, micropressure_retry_at);
      DLOGLN("[MicroPressure] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("MicroPressure", micropressure_recovery_failures, micropressure_retry_at);
    }
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
#endif  // PROFILE_METEO

#if DEVICE_PROFILE == PROFILE_METEO
  // HTU2x — temperatura/humedad interior
  if (!htu_ok && now >= htu_retry_at) {
    htu_ok = htu_begin();
    if (htu_ok) {
      sensorRecoveryMarkSuccess(htu_recovery_failures, htu_retry_at);
      DLOGLN("[HTU2x] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("HTU2x", htu_recovery_failures, htu_retry_at);
    }
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
#endif

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
  // ── AQUALEAK: MicroPressure + BMP280 + HDC1080 + BH1750 ──────────────────
  if (qwiic_ps_ok) {
    if (!micropressure_ok && now >= micropressure_retry_at) {
      micropressure_ok = barometer.begin();
      if (micropressure_ok) {
        sensorRecoveryMarkSuccess(micropressure_recovery_failures, micropressure_retry_at);
      } else {
        sensorRecoveryMarkFailure("MicroPressure", micropressure_recovery_failures, micropressure_retry_at);
      }
    }
    if (!bmp_ok && now >= bmp_retry_at) {
      bmp_ok = beginBMP280();
      if (bmp_ok) {
        sensorRecoveryMarkSuccess(bmp_recovery_failures, bmp_retry_at);
        bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                           Adafruit_BMP280::SAMPLING_X2,
                           Adafruit_BMP280::SAMPLING_X16,
                           Adafruit_BMP280::FILTER_X16,
                           Adafruit_BMP280::STANDBY_MS_500);
      } else {
        sensorRecoveryMarkFailure("BMP280", bmp_recovery_failures, bmp_retry_at);
      }
    }
    if (!hdc_ok && now >= hdc_retry_at) {
      hdc_ok = hdc1080_init();
      if (hdc_ok) {
        sensorRecoveryMarkSuccess(hdc_recovery_failures, hdc_retry_at);
      } else {
        sensorRecoveryMarkFailure("HDC1080", hdc_recovery_failures, hdc_retry_at);
      }
    }
    if (!bh1750_ok && now >= bh1750_retry_at) {
      bh1750_ok = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
      if (bh1750_ok) {
        sensorRecoveryMarkSuccess(bh1750_recovery_failures, bh1750_retry_at);
        delay(180);
      } else {
        sensorRecoveryMarkFailure("BH1750", bh1750_recovery_failures, bh1750_retry_at);
      }
    }
  }

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

  if (bh1750_ok) {
    float lux = bh1750.readLightLevel();
    if (lux < 0.0f) {
      delay(50);
      lux = bh1750.readLightLevel();
    }
    if (lux >= 0.0f) {
      lightLevel = lux;
    } else {
      bh1750_ok = false;
      DLOGLN("BH1750 fallo en lectura — cambiando a simulacion");
    }
  }
  if (!bh1750_ok) lightLevel = sim_light;

  {
    float T = temperatureMCP;
    float H = humidity;
    float Tavg = (hdc_ok && bmp_temp_ok) ? (T + bmpTemperature) / 2.0f : T;
    agroTempAvg = Tavg;
    if (!isnan(Tavg) && !isnan(H) && H > 0.0f) {
      agroDewPoint  = agro_calcDewPoint(Tavg, H);
      agroAbsHum    = agro_calcAbsHumidity(Tavg, H);
      agroHeatIndex = (Tavg > 27.0f && H > 40.0f)
                      ? agro_calcHeatIndex(Tavg, H) : NAN;
    }
  }
#endif  // PROFILE_AQUALEAK

#if DEVICE_PROFILE == PROFILE_IRRIGATION
  // ── IRRIGATION: BMP280 + AHT20 + INA219 ──────────────────────────────────
  if (!bmp_ok && now >= bmp_retry_at) {
    bmp_ok = beginBMP280();
    if (bmp_ok) {
      sensorRecoveryMarkSuccess(bmp_recovery_failures, bmp_retry_at);
      bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16,
                        Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
      DLOGLN("[BMP280] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("BMP280", bmp_recovery_failures, bmp_retry_at);
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

  if (!aht20_ok && now >= aht20_retry_at) {
    aht20_ok = aht20_begin();
    if (aht20_ok) {
      sensorRecoveryMarkSuccess(aht20_recovery_failures, aht20_retry_at);
      DLOGLN("[AHT20] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("AHT20", aht20_recovery_failures, aht20_retry_at);
    }
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

  if (!ina219_ok && now >= ina219_retry_at) {
    ina219_ok = ina219_begin();
    if (ina219_ok) {
      sensorRecoveryMarkSuccess(ina219_recovery_failures, ina219_retry_at);
      DLOGLN("[INA219] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("INA219", ina219_recovery_failures, ina219_retry_at);
    }
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
  // ── AQUA_SMART_REMOTE: BMP280 + AHT20 + INA219 ───────────────────────────
  if (!bmp_ok && now >= bmp_retry_at) {
    bmp_ok = beginBMP280();
    if (bmp_ok) {
      sensorRecoveryMarkSuccess(bmp_recovery_failures, bmp_retry_at);
      bmp280.setSampling(Adafruit_BMP280::MODE_NORMAL,
                        Adafruit_BMP280::SAMPLING_X2,
                        Adafruit_BMP280::SAMPLING_X16,
                        Adafruit_BMP280::FILTER_X16,
                        Adafruit_BMP280::STANDBY_MS_500);
      DLOGLN("[BMP280] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("BMP280", bmp_recovery_failures, bmp_retry_at);
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

  if (!aht20_ok && now >= aht20_retry_at) {
    aht20_ok = aht20_begin();
    if (aht20_ok) {
      sensorRecoveryMarkSuccess(aht20_recovery_failures, aht20_retry_at);
      DLOGLN("[AHT20] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("AHT20", aht20_recovery_failures, aht20_retry_at);
    }
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

  if (!ina219_ok && now >= ina219_retry_at) {
    ina219_ok = ina219_begin();
    if (ina219_ok) {
      sensorRecoveryMarkSuccess(ina219_recovery_failures, ina219_retry_at);
      DLOGLN("[INA219] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("INA219", ina219_recovery_failures, ina219_retry_at);
    }
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
  // TSL2584/APDS-9930 — luz ambiental
  if (!tsl_ok && now >= tsl_retry_at) {
    tsl_ok = tsl_begin();
    if (tsl_ok) {
      sensorRecoveryMarkSuccess(tsl_recovery_failures, tsl_retry_at);
      DLOGLN("[TSL2584] Reconectado tras fallo");
    } else {
      sensorRecoveryMarkFailure("TSL2584", tsl_recovery_failures, tsl_retry_at);
    }
  }
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
#endif

  // soilMoisture — YL-69 analógico o simulación (halisense se lee por separado)
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
  if (strcmp(pipelineMode, "sim") == 0) updatePipelineValues();

  // Construir snapshot y publicar en la queue para Core 0
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
  lastSensorRead     = now;
}

// ── Sensor de suelo RS485 — muestreo adaptativo ───────────────────────────────
// Rápido (soilFastIntervalMs) durante riego o ventana post-riego.
// Lento (soilSlowIntervalMs) en reposo.
void readSoilSensor(unsigned long now) {
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  static bool prevRelayOn = false;
  bool relayOn = anyRelayActive();

  if (!relayOn && prevRelayOn) soilPostIrrigEndMs = now;
  if (relayOn) soilPostIrrigEndMs = 0;
  prevRelayOn = relayOn;

  unsigned long soilInterval = soilSlowIntervalMs;
  if (relayOn) {
    soilInterval = soilFastIntervalMs;
  } else if (soilPostIrrigEndMs != 0) {
    if (now - soilPostIrrigEndMs < SOIL_POST_IRRIG_MS)
      soilInterval = soilFastIntervalMs;
    else
      soilPostIrrigEndMs = 0;
  }

  if (now - lastSoilReadMs >= soilInterval) {
    lastSoilReadMs = now;
    wdt_heartbeat("loopTask", "soil_rs485");
    if (now >= soil_rs485_retry_at) {
      if (soilSensor.readAllVariables()) {
        sensorRecoveryMarkSuccess(soil_rs485_recovery_failures, soil_rs485_retry_at);
        halisenseData.ok          = true;
        halisenseData.moisture    = soilSensor.getHumidity();
        halisenseData.temperature = soilSensor.getTemperature();
        float ecRaw               = soilSensor.getEC();
        halisenseData.ec          = ecRaw / 1000.0f;
        halisenseData.tds         = ecRaw * 0.5f;
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
        sensorRecoveryMarkFailure("SOIL", soil_rs485_recovery_failures, soil_rs485_retry_at);
        DLOGLN("[SOIL] Sin respuesta del sensor RS485");
      }
    } else {
      halisenseData.ok = false;
    }
  }
#endif
}
