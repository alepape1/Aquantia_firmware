// gsm_modem.h — Inicialización y configuración TLS del modem SIM7000G.
// Incluido desde ESP_monitor_server.ino solo cuando DEVICE_PROFILE == PROFILE_AQUA_SMART_REMOTE.
// Requiere: TinyGsmClient, SSLClient, modemSIM, gsmTLSClient (declarados en .ino).
#pragma once

// Escribe el CA cert bundle en el sistema de ficheros interno del SIM7000G.
// Solo se ejecuta si el fichero no existe ya (AT+CFSGFIS lo detecta).
static void sim7000g_uploadCACert() {
  const char* fname = "mqtt_ca.pem";
  size_t certLen = strlen(MQTT_CA_CERT_PEM);

  modemSIM.sendAT(GF("+CFSGFIS=3,\"mqtt_ca.pem\""));
  String resp = "";
  if (modemSIM.waitResponse(3000L, resp) == 1 && resp.indexOf(String(certLen)) != -1) {
    DLOGLN("[TLS] CA cert ya existe en SIM7000G — omitiendo subida");
    return;
  }

  DLOGF("[TLS] Subiendo CA cert al SIM7000G (%u B)...\n", (unsigned)certLen);
  modemSIM.sendAT(GF("+CFSINIT"));
  modemSIM.waitResponse(3000L);

  char cmd[64];
  snprintf(cmd, sizeof(cmd), "+CFSWFILE=3,\"%s\",0,%u,5000", fname, (unsigned)certLen);
  modemSIM.sendAT(cmd);

  if (modemSIM.waitResponse(5000L, GF("DOWNLOAD")) != 1) {
    DLOGLN("[TLS] ERROR: SIM7000G no aceptó CFSWFILE");
    modemSIM.sendAT(GF("+CFSTERM"));
    modemSIM.waitResponse(3000L);
    return;
  }

  modemSIM.stream.print(MQTT_CA_CERT_PEM);
  if (modemSIM.waitResponse(10000L) != 1)
    DLOGLN("[TLS] ERROR: fallo al escribir CA cert");
  else
    DLOGLN("[TLS] CA cert subido OK");

  modemSIM.sendAT(GF("+CFSTERM"));
  modemSIM.waitResponse(3000L);
}

// Configura el SSLClient (BearSSL en el ESP32) para la conexión MQTT sobre GSM.
// El SIM7000G R1529 no soporta AT+CSSLCFG — TLS gestionado enteramente por el MCU.
static void prepareGsmTLSClient(uint8_t /*unused*/ = 1) {
  gsmTLSClient.setTimeout(75000);
  DLOGF("[TLS] SSLClient configurado (BearSSL ESP32, SNI=auto, trust anchors=%d)\n", TAs_NUM);
}

// Enciende el modem SIM7000G y establece conexión GPRS con el APN configurado.
// Retorna true si GPRS queda operativo.
static bool sim7000g_powerOn() {
  pinMode(MODEM_DTR_PIN, OUTPUT);
  digitalWrite(MODEM_DTR_PIN, LOW);

  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH); delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);

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

  // Esperar a que la SIM esté realmente lista antes de abrir contexto PDP.
  // Evita gprsConnect tempranos que parecen OK pero quedan inestables.
  {
    unsigned long simT0 = millis();
    int simStatus = modemSIM.getSimStatus();
    while (simStatus != SIM_READY && (millis() - simT0) < 30000UL) {
      DLOGF("[SIM] Esperando SIM READY... status=%d\n", simStatus);
      delay(1000);
      simStatus = modemSIM.getSimStatus();
    }
    if (simStatus == SIM_READY) {
      DLOGLN("[SIM] SIM READY");
    } else {
      DLOGF("[SIM] WARN: SIM no lista tras 30s (status=%d) — continuando\n", simStatus);
    }
  }

  // Onomondo es una SIM data-only: AT+CREG (red voz) puede quedarse en
  // RegStat=2 indefinidamente aunque GPRS funcione (AT+CGREG=1/5 OK).
  // Solución: ignorar CREG y conectar GPRS directamente con reintentos.
  DLOGLN("[SIM] SIM data-only — intentando GPRS directo (sin esperar CREG)...");
  delay(5000);

  bool gprsOk = false;
  for (int attempt = 1; attempt <= 3 && !gprsOk; attempt++) {
    int csq    = modemSIM.getSignalQuality();
    int regCs  = modemSIM.getRegistrationStatus();
    DLOGF("[SIM] Intento GPRS %d/3 — CSQ:%d  CREG:%d  APN:%s\n",
          attempt, csq, regCs, GSM_APN);
    gprsOk = modemSIM.gprsConnect(GSM_APN, GSM_USER, GSM_PASS);
    DLOGF("[SIM] gprsConnect %d/3 → %s\n", attempt, gprsOk ? "OK" : "FAIL");
    if (!gprsOk) delay(5000);
  }

  if (!gprsOk) {
    DLOGLN("[SIM] ERROR: no se pudo activar contexto GPRS en 3 intentos");
    return false;
  }
  DLOGF("[SIM] GPRS OK — IP local: %s  CSQ:%d\n",
        modemSIM.localIP().toString().c_str(), modemSIM.getSignalQuality());
  return true;
}
