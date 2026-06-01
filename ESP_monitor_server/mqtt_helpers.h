#pragma once
// =============================================================================
// MQTT — Funciones auxiliares
// Solo se incluye dentro de #if defined(USE_MQTT) en el sketch principal.
// Requiere: mqttClient, finca_id, mqtt_user, mqtt_pass, relayActive[], RELAY_PINS[],
//           RELAY_COUNT, pipelineScenario, pipelineMode, telemetryIntervalMs,
//           configSyncIntervalMs, irrigationType, leakDetector — globals del sketch.
// =============================================================================

// Declaración anticipada para evitar errores de visibilidad
void mqttPublishAlert(const char* type, const char* severity, const char* message);

// Callback para comandos entrantes en aquantia/<finca_id>/cmd
// Payload esperado: {"relay": 0, "state": true} o {"type":"pipeline_config", ...}
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, payload, length)) return;

  String targetMac = doc["mac"] | "";
  targetMac.trim();
  targetMac.toUpperCase();
  String selfMac = getDeviceMacAddress();
  selfMac.toUpperCase();
  if (targetMac.length() > 0 && targetMac != selfMac) return;

  if (doc.containsKey("relay")) {
    int  relay = doc["relay"] | 0;
    bool state = doc["state"] | false;
    if (relay >= 0 && relay < RELAY_COUNT) {
      bool wasActive = relayActive[relay];
      relayActive[relay] = state;
      digitalWrite(RELAY_PINS[relay], state ? HIGH : LOW);  // activo-HIGH
      DLOGF("[MQTT] Relay %d → %s\n", relay, state ? "ON" : "OFF");
      // Al abrir la válvula (OFF→ON) reiniciar contador de sesión para medir litros
      // exactamente desde esta apertura. Sólo aplica si hay caudalímetro.
#if defined(FLOW_PIN)
      if (state && !wasActive) flowSessionReset();
#endif
      setLedState(anyRelayActive() ? LED_RELAY_ON : LED_IDLE);
    }
  }

  bool updatedPipe = false;
  const char* nextScenario = doc["pipeline_scenario"] | (const char*)pipelineScenario;
  const char* nextMode     = doc["pipeline_mode"] | (const char*)pipelineMode;
  long nextTelemetry  = doc["telemetry_interval_s"] | (long)(telemetryIntervalMs / 1000UL);
  long nextSync       = doc["config_sync_interval_s"] | (long)(configSyncIntervalMs / 1000UL);
#ifdef HAS_DISPLAY
  long nextDisplay    = doc["display_timeout_s"] | (long)(displayTimeoutMs / 1000UL);
#endif

  // Proteger escrituras con mutex (leídas desde Core 1 eventual-consistent)
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  if (strcmp(nextScenario, "normal") == 0 || strcmp(nextScenario, "leak") == 0 ||
      strcmp(nextScenario, "burst")  == 0 || strcmp(nextScenario, "obstruction") == 0) {
    if (strcmp(nextScenario, pipelineScenario) != 0) {
      strlcpy(pipelineScenario, nextScenario, sizeof(pipelineScenario));
      updatedPipe = true;
    }
  }
  if (strcmp(nextMode, "sim") == 0 || strcmp(nextMode, "real") == 0) {
    if (strcmp(nextMode, pipelineMode) != 0) {
      strlcpy(pipelineMode, nextMode, sizeof(pipelineMode));
      updatedPipe = true;
    }
  }
  // Tipo de riego (afecta umbrales del LeakDetector)
  const char* nextIrrigStr = doc["irrigation_type"] | irrigTypeToStr(irrigationType);
  IrrigationType nextIrrig = irrigStrToType(nextIrrigStr);
  if (nextIrrig != irrigationType) {
    irrigationType = nextIrrig;
    leakDetector.begin(irrigationType);
    DLOGF("[MQTT] Tipo riego → %s\n", irrigTypeToStr(irrigationType));
  }
  if (dataMutex) xSemaphoreGive(dataMutex);
  if (nextTelemetry >= 5 && nextTelemetry <= 3600) {
    telemetryIntervalMs = (unsigned long)nextTelemetry * 1000UL;
  }
  if (nextSync >= 5 && nextSync <= 3600) {
    configSyncIntervalMs = (unsigned long)nextSync * 1000UL;
  }
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION
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
    displayTimeoutMs = (unsigned long)nextDisplay * 1000UL;
  }
#endif

  if (updatedPipe) {
    updatePipelineValues();
    DLOGF("[MQTT] Pipeline mode=%s scenario=%s\n", pipelineMode, pipelineScenario);
  }

#if defined(FLOW_PIN)
  if (doc["reset_flow_counters"] | false) {
    portENTER_CRITICAL(&_flowMux);
    _flowIrrigPulses  = 0;
    _flowLeakPulses   = 0;
    _flowSessionBase  = _flowPulseTotal;
    portEXIT_CRITICAL(&_flowMux);
    DLOGLN("[FLOW] Contadores reseteados por MQTT");
    mqttPublishAlert("flow_counters_reset", "info", "Contadores de flujo reseteados");
  }
#endif
}

// Conectar al broker y suscribirse al topic de comandos
bool mqttConnect() {
  char client_id[48];
  // Client ID = "aquantia-{MAC sin colons}" — idéntico en WiFi y cellular.
  // getDeviceMacAddress() lee el eFuse: funciona sin WiFi inicializado.
  String mac_no_colon = getDeviceMacAddress();
  mac_no_colon.replace(":", "");
  snprintf(client_id, sizeof(client_id), "aquantia-%s", mac_no_colon.c_str());

  mqttClient.setKeepAlive(60);
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
  doc["device_serial"]    = device_serial_get();   // AQ-{MAC} — identidad hardware
  doc["mac_address"]      = getDeviceMacAddress(); // "FC:B4:67:F3:77:48" — mismo en WiFi y cellular
#if DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
  doc["ip_address"]       = modemSIM.localIP().toString();
  doc["network"]          = "cellular";
#else
  doc["ip_address"]       = WiFi.localIP().toString();
#endif
  doc["chip_model"]       = ESP.getChipModel();
  doc["chip_revision"]    = (int)ESP.getChipRevision();
  doc["cpu_freq_mhz"]     = (int)ESP.getCpuFreqMHz();
  doc["flash_size_mb"]    = (int)(ESP.getFlashChipSize() / 1048576);
  doc["sdk_version"]      = ESP.getSdkVersion();
  doc["relay_count"]      = RELAY_COUNT;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["device_profile"]   =
    (DEVICE_PROFILE == PROFILE_METEO)             ? "METEO" :
    (DEVICE_PROFILE == PROFILE_IRRIGATION)        ? "IRRIGATION" :
    (DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE) ? "AQUA_SMART_REMOTE" : "AQUALEAK";

  char topic[64], buf[768];
  snprintf(topic, sizeof(topic), "aquantia/%s/register", finca_id);
  size_t payload_len = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqttClient.publish(topic, buf, false);
  DLOGF("[MQTT] Register %s (%u B)\n",
                ok ? "publicado" : "ERROR",
                (unsigned)payload_len);
}

// Publica una alerta puntual en aquantia/{finca_id}/alerts.
// El backend escucha este topic e inserta en la tabla alerts.
// payload: { "device_mac", "type", "severity", "message" }
// severity: "info" | "warning" | "critical"
void mqttPublishAlert(const char* type, const char* severity, const char* message) {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<256> doc;
  doc["device_mac"] = getDeviceMacAddress();
  doc["type"]       = type;
  doc["severity"]   = severity;
  doc["message"]    = message;
  char topic[64], buf[256];
  snprintf(topic, sizeof(topic), "aquantia/%s/alerts", finca_id);
  size_t len = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqttClient.publish(topic, buf, false);
  DLOGF("[MQTT] Alert %s type=%s sev=%s (%u B)\n",
        ok ? "OK" : "ERROR", type, severity, (unsigned)len);
}
