// http_client.h — Capa HTTP para perfiles WiFi (no disponible en AQUA_SMART_REMOTE).
// Incluido desde ESP_monitor_server.ino solo cuando DEVICE_PROFILE != PROFILE_AQUA_SMART_REMOTE.
// Requiere: WiFiClientSecure, HTTPClient, ArduinoJson, y los globales de pipeline/relay.
#pragma once

// ── Helpers de URL y TLS ──────────────────────────────────────────────────────
static bool serverUseTls() {
  return server_port == 443 || server_port == 8443;
}

static String serverBaseUrl() {
  String url = serverUseTls() ? "https://" : "http://";
  url += String(server_ip);
  if ((!serverUseTls() && server_port != 80) || (serverUseTls() && server_port != 443))
    url += ":" + String(server_port);
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

static void prepareSecureClient(WiFiClientSecure& client, int timeoutMs = 10000) {
  int handshakeSeconds = timeoutMs / 1000;
  if (handshakeSeconds < 1) handshakeSeconds = 1;
  client.stop();
  client.setHandshakeTimeout(handshakeSeconds);
  client.setCACert(MQTT_CA_CERT_PEM);
  if (!tlsClockReady(5000))
    DLOGLN("[TLS] Advertencia: reloj aun no sincronizado; reintentando handshake con la CA cargada");
}

// ── Validación de bitmask de relay ────────────────────────────────────────────
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

// ── Device info POST ──────────────────────────────────────────────────────────
void postDeviceInfo() {
  JsonDocument doc;
  doc["chip_model"]       = ESP.getChipModel();
  doc["chip_revision"]    = (int)ESP.getChipRevision();
  doc["cpu_freq_mhz"]     = ESP.getCpuFreqMHz();
  doc["flash_size_mb"]    = ESP.getFlashChipSize() / (1024 * 1024);
  doc["sdk_version"]      = ESP.getSdkVersion();
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

// ── GET genérico ──────────────────────────────────────────────────────────────
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

// ── POST genérico ─────────────────────────────────────────────────────────────
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

// ── Sincronización de configuración de pipeline desde el backend ──────────────
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

#if defined(FLOW_PIN)
      if (doc["reset_flow_counters"] | false) {
        portENTER_CRITICAL(&_flowMux);
        _flowIrrigPulses = 0;
        _flowLeakPulses  = 0;
        _flowSessionBase = _flowPulseTotal;
        portEXIT_CRITICAL(&_flowMux);
        DLOGLN("[FLOW] Contadores reseteados por el backend");
        httpPost(serverBaseUrl() + "/api/flow/reset-ack?mac=" + WiFi.macAddress(), "");
      }
#endif
      if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
      if (strcmp(nextScenario, "normal") == 0 || strcmp(nextScenario, "leak") == 0 ||
          strcmp(nextScenario, "burst")  == 0 || strcmp(nextScenario, "obstruction") == 0) {
        if (strcmp(nextScenario, pipelineScenario) != 0)
          DLOGF("[PIPE] Escenario → %s\n", nextScenario);
        strlcpy(pipelineScenario, nextScenario, sizeof(pipelineScenario));
      }
      if (strcmp(nextMode, "sim") == 0 || strcmp(nextMode, "real") == 0) {
        if (strcmp(nextMode, pipelineMode) != 0)
          DLOGF("[PIPE] Modo → %s\n", nextMode);
        strlcpy(pipelineMode, nextMode, sizeof(pipelineMode));
      }
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
        if (nextMs != telemetryIntervalMs) { telemetryIntervalMs = nextMs; DLOGF("[PIPE] Telemetría → %lds\n", nextTelemetry); }
      }
      if (nextSync >= 5 && nextSync <= 3600) {
        unsigned long nextMs = (unsigned long)nextSync * 1000UL;
        if (nextMs != configSyncIntervalMs) { configSyncIntervalMs = nextMs; DLOGF("[PIPE] Sync config → %lds\n", nextSync); }
      }
#if DEVICE_PROFILE == PROFILE_METEO || DEVICE_PROFILE == PROFILE_IRRIGATION \
 || DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE
      {
        long nextSoilFast = doc["soil_fast_interval_s"] | (long)(soilFastIntervalMs / 1000UL);
        long nextSoilSlow = doc["soil_slow_interval_s"] | (long)(soilSlowIntervalMs / 1000UL);
        if (nextSoilFast >= 3  && nextSoilFast <= 300)  soilFastIntervalMs = (unsigned long)nextSoilFast * 1000UL;
        if (nextSoilSlow >= 20 && nextSoilSlow <= 3600) soilSlowIntervalMs = (unsigned long)nextSoilSlow * 1000UL;
      }
#endif
#ifdef HAS_DISPLAY
      if (nextDisplay >= 0 && nextDisplay <= 3600) {
        unsigned long nextMs = (unsigned long)nextDisplay * 1000UL;
        if (nextMs != displayTimeoutMs) { displayTimeoutMs = nextMs; DLOGF("[PIPE] Pantalla timeout → %lds\n", nextDisplay); }
      }
#endif
      return;
    }

    body.trim();
    if (body == "normal" || body == "leak" || body == "burst" || body == "obstruction") {
      if (strcmp(body.c_str(), pipelineScenario) != 0)
        DLOGF("[PIPE] Escenario → %s\n", body.c_str());
      strlcpy(pipelineScenario, body.c_str(), sizeof(pipelineScenario));
    }
  }
}

// ── Polling de comandos de relay ──────────────────────────────────────────────
void checkRelayCommand() {
  String url = serverBaseUrl() + "/api/relay/command?mac=" + WiFi.macAddress();
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
      if (desired && !relayActive[i]) flowSessionReset();
#endif
      relayActive[i] = desired;
      digitalWrite(RELAY_PINS[i], desired ? HIGH : LOW);
      DLOGF("[Relay %d] %s\n", i, desired ? "ON" : "OFF");
      changed = true;
    }
  }
  if (changed) {
    int actualMask = 0;
    for (int i = 0; i < RELAY_COUNT; i++)
      if (relayActive[i]) actualMask |= (1 << i);
    setLedState(anyRelayActive() ? LED_RELAY_ON : LED_IDLE);
    httpPost(serverBaseUrl() + "/api/relay/ack", String(actualMask));
  }
}
