// secrets.h — NO subir al repositorio (está en .gitignore)

// ══════════════════════════════════════════════════════════════════════════════
// MODO DE ARRANQUE
//   DEV_MODE  → usa las credenciales de este archivo directamente.
//               Sin NVS, sin portal captivo, sin provisioning.
//               Ideal para desarrollo y pruebas rápidas.
//
//   (comentar) → modo PRODUCCIÓN: NVS + portal SoftAP + token de fábrica.
//               El dispositivo pedirá configuración al cliente en el primer arranque.
// ══════════════════════════════════════════════════════════════════════════════
#define DEV_MODE

// ── Perfil de hardware ────────────────────────────────────────────────────────
// Descomentar para la placa de riego (4 relays, sin pantalla):
// #define DEVICE_PROFILE PROFILE_IRRIGATION

// ── Credenciales (solo se usan en DEV_MODE) ───────────────────────────────────
#define WIFI_SSID     "TechCave_guess"
#define WIFI_PASSWORD "7yfnPgvSiwCL"
#define SERVER_IP     "192.168.1.7"
#define SERVER_PORT   7000

#define USE_MQTT
#define MQTT_SERVER  "192.168.1.7"
#define MQTT_PORT    1883
#define FINCA_ID     "finca-ale"
#define MQTT_USER    "backend"
#define MQTT_PASS    "aquantia_159"
