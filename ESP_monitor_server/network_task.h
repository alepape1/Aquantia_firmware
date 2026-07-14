// network_task.h — Snapshot de telemetría y tarea de red FreeRTOS (Core 0).
// Incluido desde ESP_monitor_server.ino después de mqtt_helpers.h.
// Requiere: todos los globales de sensores, relays, MQTT, GSM/WiFi declarados en .ino.
#pragma once

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
// true cuando WiFi es el transporte activo por fallo de SIM (Core 0 only — sin mutex).
static bool _wifiFallbackActive = false;
#endif

// Captura atómica de sensores para telemetría.
// Core 1 construye el snapshot y lo publica en telemetryQueue.
// Core 0 solo lo lee aquí — sin bloqueo, sin mutex, sin latencia.
void takeSnapshot() {
  if (telemetryQueue) {
    xQueuePeek(telemetryQueue, &_netSnap, 0);
  }
  _netSnap.heap   = (long)ESP.getFreeHeap();
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  _netSnap.rssi   = _wifiFallbackActive ? (int)WiFi.RSSI() : (int)modemSIM.getSignalQuality();
#else
  _netSnap.rssi   = WiFi.RSSI();
#endif
  _netSnap.uptime = (long)(millis() / 1000);
}

// =============================================================================
// TAREA DE RED — Core 0
// Gestiona OTA, relay polling y telemetría sin bloquear sensores/display (Core 1).
// =============================================================================
void networkTask(void* pvParameters) {
  {
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 120000, .idle_core_mask = 0, .trigger_panic = true };
#else
    esp_task_wdt_config_t wdt_cfg = { .timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true };
#endif
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
  static unsigned long wifiStableSince   = 0;
  static int           wifiFailCount     = 0;

#ifdef USE_MQTT
  static unsigned long mqttRetryDelayMs = 2000;
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  static int  mqttConnectFails = 0;
#endif

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  if (mqtt_port == 8883) {
    prepareGsmTLSClient();
    mqttClient.setClient(gsmTLSClient);
    DLOGF("[MQTT] Broker TLS (BearSSL): %s:%d\n", mqtt_server, mqtt_port);
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
  mqttClient.setBufferSize(2048);
#endif

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE && defined(DEV_MODE)
  // DEV_MODE: WiFi primario con WIFI_SSID/WIFI_PASSWORD de secrets.h.
  // Permite desarrollar sin depender de la SIM. Si no hay WiFi, el
  // dispositivo cae a SIM normalmente.
  DLOGLN("[NET] DEV_MODE ASR: probando WiFi primario (" WIFI_SSID ")...");
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_18_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  {
    unsigned long _wifiDevT0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - _wifiDevT0 < 15000UL) {
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(500));
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    DLOGF("[NET] WiFi DEV activo — IP: %s\n", WiFi.localIP().toString().c_str());
    _wifiFallbackActive = true;
    if (mqtt_port == 8883) {
      asr_wifiFallbackTLSClient.setCACert(MQTT_CA_CERT_PEM);
      mqttClient.setClient(asr_wifiFallbackTLSClient);
    } else {
      mqttClient.setClient(asr_wifiFallbackClient);
    }
  } else {
    DLOGLN("[NET] WiFi DEV no disponible — usando SIM");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif

  for (;;) {
    wdt_heartbeat("NetworkTask");
    esp_task_wdt_reset();
#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
    ArduinoOTA.handle();
    if (isUpdatingOTA) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
#endif

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    // ── Conectividad — SIM primario, WiFi como fallback ──────────────────────
    static unsigned long _lastCellPollMs = 0;
    static unsigned long _lastSimCheckMs = 0;   // comprobación periódica de recuperación SIM

    if (_wifiFallbackActive) {
      // ── Modo WiFi fallback ──────────────────────────────────────────────────
      if (WiFi.status() != WL_CONNECTED) {
#ifdef DEV_MODE
        // DEV_MODE: WiFi es primario — reconectar sin caer a SIM.
        DLOGLN("[NET] WiFi DEV caído — reconectando en 5 s...");
        mqttClient.disconnect();
        WiFi.reconnect();
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
#else
        DLOGLN("[NET] WiFi fallback caído — volviendo a modo SIM");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _wifiFallbackActive = false;
        _lastCellPollMs     = 0;
        if (mqtt_port == 8883) { prepareGsmTLSClient(); mqttClient.setClient(gsmTLSClient); }
        else                   { mqttClient.setClient(gsmTCPClient); }
        mqttClient.disconnect();
        continue;
#endif
      }
      // Intentar recuperar SIM cada 5 minutos (solo producción; DEV_MODE usa WiFi como primario)
#ifndef DEV_MODE
      if (millis() - _lastSimCheckMs >= 300000UL) {
        _lastSimCheckMs = millis();
        DLOGLN("[NET] Comprobando recuperación SIM (modo WiFi fallback)...");
        if (modemSIM.testAT()) {
          bool gprsOk = modemSIM.isGprsConnected();
          if (!gprsOk) gprsOk = modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS);
          if (gprsOk) {
            DLOGLN("[SIM] SIM recuperado — desactivando WiFi fallback");
            _wifiFallbackActive  = false;
            _gprsConnectedFlag   = true;
            _simCsqCache         = modemSIM.getSignalQuality();
            wifiFailCount        = 0;
            wifiRetryDelayMs     = 500;
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            if (mqtt_port == 8883) { prepareGsmTLSClient(); mqttClient.setClient(gsmTLSClient); }
            else                   { mqttClient.setClient(gsmTCPClient); }
            mqttClient.disconnect();
          } else {
            DLOGLN("[NET] SIM no disponible — manteniendo WiFi fallback");
          }
        } else {
          DLOGLN("[NET] Modem sin respuesta — manteniendo WiFi fallback");
        }
      }
#endif  // !DEV_MODE

    } else {
      // ── Modo SIM normal ─────────────────────────────────────────────────────
      bool doCellPoll = (millis() - _lastCellPollMs >= 2000UL) || !_gprsConnectedFlag;
      if (doCellPoll) {
        _lastCellPollMs = millis();

        if (!modemSIM.testAT()) {
          wdt_heartbeat("NetworkTask", "gsm_at_check");
          setLedState(LED_WIFI_CONNECTING);
          wifiFailCount++;
          _gprsConnectedFlag = false;
          _simCsqCache = 0;
          DLOGF("[SIM] Modem no responde a AT — failCount:%d\n", wifiFailCount);
          if (wifiFailCount >= 10) {
            DLOGLN("[SIM] 10 fallos AT consecutivos — reiniciando ESP");
            esp_restart();
          }
          vTaskDelay(pdMS_TO_TICKS(5000));
          continue;
        }

        _gprsConnectedFlag = modemSIM.isGprsConnected();
        if (!_gprsConnectedFlag) {
          wdt_heartbeat("NetworkTask", "gprs_connect");
          setLedState(LED_WIFI_CONNECTING);
          wifiFailCount++;
          int csq  = modemSIM.getSignalQuality();
          int creg = modemSIM.getRegistrationStatus();
          _simCsqCache = csq;
          DLOGF("[SIM] GPRS inactivo — CSQ:%d  CREG:%d  failCount:%d  APN:%s\n",
                csq, creg, wifiFailCount, GSM_APN);

#ifndef DEV_MODE
          // Intentar WiFi fallback tras 5 fallos consecutivos de GPRS.
          // Cada ciclo de fallo tarda ~60-90s (gprsConnect timeout + backoff),
          // así que 5 fallos ≈ 5-8 min. prov_ssid solo existe fuera de DEV_MODE.
          if (wifiFailCount == 5 && strlen(prov_ssid) > 0) {
            DLOGF("[NET] %d fallos GPRS — intentando WiFi fallback (%s)...\n",
                  wifiFailCount, prov_ssid);
            WiFi.mode(WIFI_STA);
            WiFi.setTxPower(WIFI_POWER_18_5dBm);
            WiFi.begin(prov_ssid, prov_password);
            unsigned long _wt0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - _wt0 < 15000UL) {
              esp_task_wdt_reset();
              vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (WiFi.status() == WL_CONNECTED) {
              DLOGF("[NET] WiFi fallback activo — IP: %s\n",
                    WiFi.localIP().toString().c_str());
              _wifiFallbackActive = true;
              wifiFailCount       = 0;
              wifiRetryDelayMs    = 500;
              _lastSimCheckMs     = millis();
              if (mqtt_port == 8883) {
                asr_wifiFallbackTLSClient.setCACert(MQTT_CA_CERT_PEM);
                mqttClient.setClient(asr_wifiFallbackTLSClient);
              } else {
                mqttClient.setClient(asr_wifiFallbackClient);
              }
              mqttClient.disconnect();
            } else {
              DLOGLN("[NET] WiFi fallback FAIL — red WiFi no disponible");
              WiFi.disconnect(true);
              WiFi.mode(WIFI_OFF);
            }
          }
#endif  // !DEV_MODE

          if (wifiFailCount >= 15) {
            DLOGLN("[SIM] 15 fallos consecutivos sin GPRS ni WiFi — reiniciando ESP");
            esp_restart();
          }

          if (!_wifiFallbackActive) {
            bool gprsOk = modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS);
            _gprsConnectedFlag = gprsOk;
            _simCsqCache = modemSIM.getSignalQuality();
          #ifdef USE_MQTT
            if (gprsOk) mqttConnectFails = 0;
          #endif
            DLOGF("[SIM] gprsConnect → %s  CSQ:%d\n",
                  gprsOk ? "OK" : "FAIL", _simCsqCache);
            if (!gprsOk) {
              vTaskDelay(pdMS_TO_TICKS(wifiRetryDelayMs));
              if (wifiRetryDelayMs < 30000) wifiRetryDelayMs = min(wifiRetryDelayMs * 2UL, 30000UL);
              continue;
            }
            DLOGF("[SIM] GPRS reconectado — IP: %s\n", modemSIM.localIP().toString().c_str());
          }
        } else {
          _simCsqCache = modemSIM.getSignalQuality();
        }
      }
#ifdef DEBUG_MODE
      static unsigned long _lastGprsReport = 0;
      if (_gprsConnectedFlag && millis() - _lastGprsReport >= 30000) {
        _lastGprsReport = millis();
        DLOGF("[SIM] GPRS OK — IP:%s  CSQ:%d  Op:%s\n",
              modemSIM.localIP().toString().c_str(),
              _simCsqCache,
              modemSIM.getOperator().c_str());
      }
#endif
      wifiFailCount = 0;
      wifiRetryDelayMs = 500;
    }
#else
    // ── Conectividad WiFi ───────────────────────────────────────────────────
    if (WiFi.status() != WL_CONNECTED) {
      wdt_heartbeat("NetworkTask", "wifi_reconnect");
      setLedState(LED_WIFI_CONNECTING);
      wifiStableSince = 0;
      wifiFailCount++;

      if (wifiFailCount >= 60) {
#ifndef DEV_MODE
        // Señalizar al siguiente boot que abra el portal SoftAP para que el
        // usuario pueda actualizar las credenciales si el router cambió.
        // No se borran credenciales: si el router estaba caído temporalmente
        // el usuario simplemente cierra el portal y el dispositivo reconecta.
        provisioning_set_ap_forced();
#endif
        esp_restart();
      } else if (wifiFailCount % 10 == 0) {
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
    if (wifiStableSince == 0) wifiStableSince = millis();
    if (millis() - wifiStableSince > 10000) wifiRetryDelayMs = 500;
#endif

    unsigned long now = millis();

#ifdef USE_MQTT
#if DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE
    if (time(nullptr) < 1000000000L && (now - lastNtpRetry > 60000 || lastNtpRetry == 0)) {
      DLOGLN("[NTP] Reintentando sincronización...");
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      lastNtpRetry = now;
    }
#elif DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    if (_wifiFallbackActive && time(nullptr) < 1000000000L
        && (now - lastNtpRetry > 60000 || lastNtpRetry == 0)) {
      DLOGLN("[NTP] WiFi fallback — sincronizando NTP...");
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
#ifdef DEBUG_MODE
      DLOGF("[MQTT] Desconectado — state=%d  retryMs=%lu\n",
            mqttClient.state(), mqttRetryDelayMs);
  #if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
    DLOGF("[MQTT] Estado red antes de conectar: isGPRS=%d  CSQ=%d\n",
      (int)_gprsConnectedFlag,
      _simCsqCache);
  #endif
#endif
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      if (mqtt_port == 8883) {
        if (_wifiFallbackActive) asr_wifiFallbackTLSClient.setCACert(MQTT_CA_CERT_PEM);
        else                     prepareGsmTLSClient();
      }
#else
      if (mqtt_port == 8883) prepareSecureClient(mqttTLSClient, 10000);
#endif
      esp_task_wdt_reset();
      if (!mqttConnect()) {
#ifdef DEBUG_MODE
        DLOGF("[MQTT] Connect FAIL — state=%d  broker=%s:%d  user=%s\n",
              mqttClient.state(), mqtt_server, mqtt_port, mqtt_user);
  #if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      DLOGF("[MQTT] Estado red tras fallo: isGPRS=%d  CSQ=%d\n",
        (int)_gprsConnectedFlag,
        _simCsqCache);
  #endif
#endif
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
        mqttConnectFails++;
        if (mqtt_port == 8883 && (mqttClient.state() == -4 || mqttClient.state() == -1)) {
          mqttConnectFails = 0;
          if (_gprsConnectedFlag) {
            DLOGLN("[SIM] Refresh PDP tras timeout MQTT (gprsDisconnect/gprsConnect)");
            modemSIM.gprsDisconnect();
            vTaskDelay(pdMS_TO_TICKS(600));
            bool gprsOk = modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS);
            _gprsConnectedFlag = gprsOk;
            _simCsqCache = modemSIM.getSignalQuality();
            DLOGF("[SIM] PDP refresh → %s  CSQ:%d\n", gprsOk ? "OK" : "FAIL", _simCsqCache);
          }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(mqttRetryDelayMs));
        if (mqttRetryDelayMs < 15000) mqttRetryDelayMs += 2000;
        continue;
      }
      setLedState(LED_IDLE);
      mqttRetryDelayMs = 2000;
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      mqttConnectFails = 0;
#endif
      if (!deviceInfoSent) {
        mqttPublishRegister();
        deviceInfoSent = true;
        time_t t = time(nullptr);
        if (t > 1000000000L) {
          struct tm tm_info;
          localtime_r(&t, &tm_info);
          strftime(g_lastConnectStr, sizeof(g_lastConnectStr),
                   "%H:%M:%S %d/%m/%y", &tm_info);
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Dispositivo reiniciado: %s", g_rebootReason);
        mqttPublishAlert("device_reboot", "info", msg);
      } else {
        static unsigned long _mqttReconnectAlertMs = 0;
        if (millis() - _mqttReconnectAlertMs >= 3600000UL) {
          mqttPublishAlert("mqtt_reconnect", "info", "Dispositivo reconectado al broker MQTT");
          _mqttReconnectAlertMs = millis();
        }
      }
    }
    mqttClient.loop();

#ifdef DEBUG_MODE
    if (!mqttClient.connected()) {
      static unsigned long _lastMqttDropLog = 0;
      if (millis() - _lastMqttDropLog >= 2000) {
        _lastMqttDropLog = millis();
        DLOGF("[MQTT] Detectada desconexión tras loop() — state=%d  uptime=%lus\n",
              mqttClient.state(), millis() / 1000);
  #if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      DLOGF("[SIM] Estado tras caída MQTT: isGPRS=%d  CSQ=%d  IP=%s\n",
        (int)_gprsConnectedFlag,
        _simCsqCache,
        modemSIM.localIP().toString().c_str());
  #endif
      }
    }
#endif

    // ── Alarmas MQTT — solo al cambio de estado ──────────────────────────────
    {
      static const unsigned long SENSOR_ALERT_COOLDOWN = 43200000UL;

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
      {
        static const float SOIL_DRY_THRESHOLD = 30.0f;
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

      bool heapWarn = (ESP.getFreeHeap() < 30000);
      if (heapWarn && !_lastHeapWarn)
        mqttPublishAlert("low_heap", "warning", "Heap libre < 30 KB — posible memory leak");
      _lastHeapWarn = heapWarn;
    }

    // Publicar telemetría cada telemetryIntervalMs
    if (now - lastSendTime >= telemetryIntervalMs) {
      wdt_heartbeat("NetworkTask", "mqtt_publish");
      takeSnapshot();
      const TelemetrySnapshot& snap = _netSnap;

      auto r2 = [](float x){ return roundf(x * 100.0f) / 100.0f; };
      auto r1 = [](float x){ return roundf(x * 10.0f)  / 10.0f;  };

      char topic[64];
      snprintf(topic, sizeof(topic), "aquantia/%s/telemetry", finca_id);
      size_t payload_len;
      bool ok;

#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      // ── GSM slim payload ─────────────────────────────────────────────────
      {
        StaticJsonDocument<640> doc;
        doc["temperature"]       = r2(snap.tempMCP);
        doc["humidity"]          = r2(snap.humidity);
        doc["pressure"]          = r2(snap.pressure);
        doc["windSpeed"]         = r2(snap.windSpeed);
        doc["windDirection"]     = r1(snap.windDir);
        doc["light"]             = r1(snap.light);
        doc["soil_moisture"]     = r1(snap.soil);
        doc["pipeline_pressure"] = r2(snap.pipePressure);
        doc["pipeline_flow"]     = r2(snap.pipeFlow);
        doc["flow_total_l"]      = roundf(snap.flowTotalL * 10.0f) / 10.0f;
        doc["relay_active"]      = snap.relayMask;
        doc["rssi"]              = snap.rssi;
        doc["free_heap"]         = snap.heap;
        doc["uptime_s"]          = snap.uptime;
        doc["mac_address"]       = getDeviceMacAddress();
        if (snap.halisenseOk) {
          doc["soil_temperature"] = r1(snap.soilTemp);
          doc["soil_ec"]          = r2(snap.soilEc);
          doc["soil_ph"]          = r1(snap.soilPh);
          doc["soil_n"]           = snap.soilN;
          doc["soil_p"]           = snap.soilP;
          doc["soil_k"]           = snap.soilK;
        }
        if (!isnan(snap.inaPower))   doc["ina219_power_mw"]    = r1(snap.inaPower);
        if (!isnan(snap.inaVbus))    doc["ina219_bus_voltage"]  = r2(snap.inaVbus);
        if (!isnan(snap.inaCurrent)) doc["ina219_current_ma"]   = r1(snap.inaCurrent);
        if (snap.ensAqi > 0) {
          doc["ens160_aqi"]  = snap.ensAqi;
          doc["ens160_tvoc"] = snap.ensTvoc;
          doc["ens160_eco2"] = snap.ensEco2;
        }
        { time_t _ts = time(nullptr); if (_ts > 1000000000L) doc["ts"] = (long)_ts; }
        char buf[640];
        payload_len = serializeJson(doc, buf, sizeof(buf));
        if (payload_len >= sizeof(buf))
          DLOGF("[MQTT] WARN payload truncado (%u >= %u)\n", (unsigned)payload_len, (unsigned)sizeof(buf));
        ok = mqttClient.publish(topic, buf, false);
      }
#else
      // ── Full payload (WiFi / debug) ───────────────────────────────────────
      {
        StaticJsonDocument<1280> doc;
        doc["temperature"]           = r2(snap.tempMCP);
        doc["pressure"]              = r2(snap.pressure);
        doc["humidity"]              = r2(snap.humidity);
        doc["bmp280_ok"]             = bmp_ok && (bmp_temp_ok || bmp_pressure_ok);
        if (!isnan(snap.bmpTemp))     doc["bmp280_temperature"] = r2(snap.bmpTemp);
        if (!isnan(snap.bmpPressure)) doc["bmp280_pressure"]    = r2(snap.bmpPressure);
        doc["windSpeed"]             = r2(snap.windSpeed);
        doc["windDirection"]         = r1(snap.windDir);
        doc["light"]                 = r1(snap.light);
        doc["rssi"]                  = snap.rssi;
        doc["free_heap"]             = snap.heap;
        doc["uptime_s"]              = snap.uptime;
        doc["relay_active"]          = snap.relayMask;
        doc["soil_moisture"]         = r1(snap.soil);
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION
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
        doc["flow_total_l"]          = roundf(snap.flowTotalL   * 100.0f) / 100.0f;
        doc["flow_session_l"]        = roundf(snap.flowSessionL * 100.0f) / 100.0f;
        doc["flow_irrig_l"]          = roundf(snap.flowIrrigL   * 100.0f) / 100.0f;
        doc["flow_leak_l"]           = roundf(snap.flowLeakL    * 100.0f) / 100.0f;
        doc["pipeline_scenario"]     = pipelineScenario;
        doc["pipeline_mode"]         = pipelineMode;
        doc["pipeline_source"]       = pipelineSource;
        doc["irrigation_type"]       = irrigTypeToStr(irrigationType);
        doc["leak_detect_trained"]   = leakDetector.hasBaseline();
        doc["pipeline_pressure_ok"]  = pipelinePressureOk;
        doc["pipeline_flow_ok"]      = pipelineFlowOk;
        doc["leak_baseline_pressure"] = r2(leakDetector.baselinePressure());
        doc["leak_baseline_flow"]     = r2(leakDetector.baselineFlow());
        doc["xdb401_ok"]             = xdb401_ok;
        if (!isnan(snap.xdb401Temp)) doc["xdb401_temperature"] = r2(snap.xdb401Temp);
        doc["mac_address"]           = getDeviceMacAddress();
        doc["ip_address"]            = WiFi.localIP().toString();
        doc["relay_count"]           = RELAY_COUNT;
        doc["firmware_version"]      = FIRMWARE_VERSION;
#if DEVICE_PROFILE == PROFILE_AQUALEAK
        if (!isnan(snap.dewPoint))   doc["dew_point"] = r1(snap.dewPoint);
#endif
#if DEVICE_PROFILE == PROFILE_IRRIGATION
        doc["aht20_ok"]  = aht20_ok;
        doc["ina219_ok"] = ina219_ok;
        if (!isnan(snap.inaPower))   doc["ina219_power_mw"]   = r1(snap.inaPower);
        if (!isnan(snap.inaVbus))    doc["ina219_bus_voltage"] = r2(snap.inaVbus);
        if (!isnan(snap.inaCurrent)) doc["ina219_current_ma"]  = r1(snap.inaCurrent);
#endif
        doc["ens160_ok"] = ens160_ok;
        if (snap.ensAqi > 0) {
          doc["ens160_aqi"]  = snap.ensAqi;
          doc["ens160_tvoc"] = snap.ensTvoc;
          doc["ens160_eco2"] = snap.ensEco2;
        }
        { time_t _ts = time(nullptr); if (_ts > 1000000000L) doc["ts"] = (long)_ts; }
        char buf[1280];
        payload_len = serializeJson(doc, buf, sizeof(buf));
        if (payload_len >= sizeof(buf))
          DLOGF("[MQTT] WARN payload truncado (%u >= %u)\n", (unsigned)payload_len, (unsigned)sizeof(buf));
        ok = mqttClient.publish(topic, buf, false);
      }
#endif // DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE

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
    if (!deviceInfoSent) {
      postDeviceInfo();
      deviceInfoSent = true;
    }

    if (now - lastRelayCheck >= RELAY_MS) {
      checkRelayCommand();
      lastRelayCheck = now;
    }

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
        snap.flowTotalL
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
