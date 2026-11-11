#include "api.h"
#include "display_config.h"
#include "interfaccia_ui.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <vector>

// ============================================================================
// CONFIGURAZIONE
// ============================================================================
static const char *HEARTBEAT_URL =
    "https://demo.relaicrm.com/api/v1/devices/heartbeat/";
static const char *CHANNEL_AUTH_URL =
    "https://demo.relaicrm.com/broadcasting/auth";

static const char *API_TOKEN =
    "18|9piZoRET32QPy8OCOU8rzhKd2gmvyFJjEop5Tdsk38648aaa";

static const char *DEVICE_MAC = "C4:DE:E2:12:34:56";
static const char *DEVICE_TYPE = "esp_display";

static const int WS_TLS_MODE = -1;

static const size_t MAX_EVENTS = 10;
static const uint32_t HEARTBEAT_DEFAULT_MS = 30000;
static const uint32_t HEARTBEAT_RETRY_MS = 15000;
static const uint32_t SUBSCRIBE_RETRY_MS = 10000;

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.2.0"
#endif

String get_current_firmware_version() { return String(FIRMWARE_VERSION); }

// ============================================================================
// STATO
// ============================================================================
static std::vector<ApiEvent> events_cache;
static int cached_rotation_interval_ms = 30000; // Default a 30 secondi

static WebSocketsClient webSocket;
static bool ws_initialized = false;
static bool ws_connected = false;
static String ws_socket_id;
static uint32_t ws_next_subscribe_ms = 0;

// Gestione dei due canali richiesti (dispositivo e organizzazione)
static String ws_device_channel;
static String ws_org_channel;
static bool ws_device_subscribed = false;
static bool ws_org_subscribed = false;

static bool heartbeat_scheduled = false;
static bool force_sync = false;
static uint32_t next_heartbeat_ms = 0;
static uint32_t heartbeat_interval_ms = HEARTBEAT_DEFAULT_MS;

static bool scaduto(uint32_t now, uint32_t t) {
  return (int32_t)(now - t) >= 0;
}

// Unico client TLS riusato (keep-alive) da heartbeat e auth
static WiFiClientSecure httpsClient;
static bool https_client_init = false;
static uint8_t heartbeat_failures = 0;

static WiFiClientSecure &https_client() {
  if (!https_client_init) {
    httpsClient.setInsecure();
    https_client_init = true;
  }
  return httpsClient;
}

// ============================================================================
// CACHE EVENTI E IMPOSTAZIONI (RAM + NVS)
// ============================================================================
static bool eventi_uguali(const ApiEvent &a, const ApiEvent &b) {
  return a.id == b.id && a.name == b.name && a.qr == b.qr &&
         a.reservations == b.reservations && a.participants == b.participants;
}

static void salva_eventi(JsonArray events, const char *origine) {
  std::vector<ApiEvent> nuovi;
  for (JsonObject ev : events) {
    if (nuovi.size() >= MAX_EVENTS)
      break;
    ApiEvent c;
    c.id = ev["id"] | 0L;
    c.name = ev["name"] | "";
    c.qr = ev["qr"] | "";
    c.reservations = ev["reservations"] | 0;
    c.participants = ev["participants"] | 0;
    nuovi.push_back(c);
  }

  bool uguale = (nuovi.size() == events_cache.size());
  for (size_t i = 0; uguale && i < nuovi.size(); ++i) {
    uguale = eventi_uguali(nuovi[i], events_cache[i]);
  }
  if (uguale)
    return;

  events_cache = nuovi;

  Preferences prefs;
  prefs.begin("api-cache", false);
  for (size_t i = 0; i < MAX_EVENTS; ++i) {
    char kId[16], kName[16], kQr[16], kPart[16], kPartic[16];
    snprintf(kId, sizeof(kId), "ev_id_%u", (unsigned)i);
    snprintf(kName, sizeof(kName), "ev_name_%u", (unsigned)i);
    snprintf(kQr, sizeof(kQr), "ev_qr_%u", (unsigned)i);
    snprintf(kPart, sizeof(kPart), "ev_part_%u", (unsigned)i);
    snprintf(kPartic, sizeof(kPartic), "ev_partic_%u", (unsigned)i);

    if (i < events_cache.size()) {
      prefs.putLong(kId, events_cache[i].id);
      prefs.putString(kName, events_cache[i].name);
      prefs.putString(kQr, events_cache[i].qr);
      prefs.putInt(kPart, events_cache[i].reservations);
      prefs.putInt(kPartic, events_cache[i].participants);
    } else {
      if (prefs.isKey(kId))
        prefs.remove(kId);
      if (prefs.isKey(kName))
        prefs.remove(kName);
      if (prefs.isKey(kQr))
        prefs.remove(kQr);
      if (prefs.isKey(kPart))
        prefs.remove(kPart);
      if (prefs.isKey(kPartic))
        prefs.remove(kPartic);
    }
  }
  prefs.end();

  Serial.printf("[Cache] %s: %u eventi aggiornati.\n", origine,
                (unsigned)events_cache.size());

  ui_richiedi_ricarica_tileview();
}

std::vector<ApiEvent> api_get_cached_events() {
  if (!events_cache.empty()) {
    return events_cache;
  }

  std::vector<ApiEvent> result;
  Preferences prefs;
  prefs.begin("api-cache", true);

  for (size_t i = 0; i < MAX_EVENTS; i++) {
    char kId[16], kName[16], kQr[16], kPart[16], kPartic[16];
    snprintf(kId, sizeof(kId), "ev_id_%u", (unsigned)i);
    snprintf(kName, sizeof(kName), "ev_name_%u", (unsigned)i);
    snprintf(kQr, sizeof(kQr), "ev_qr_%u", (unsigned)i);
    snprintf(kPart, sizeof(kPart), "ev_part_%u", (unsigned)i);
    snprintf(kPartic, sizeof(kPartic), "ev_partic_%u", (unsigned)i);

    if (prefs.isKey(kName)) {
      ApiEvent ev;
      ev.id = prefs.getLong(kId, 2126 + i);
      ev.name = prefs.getString(kName, "");
      ev.qr = prefs.getString(kQr, "");
      ev.reservations = prefs.getInt(kPart, 0);
      ev.participants = prefs.getInt(kPartic, 0);
      result.push_back(ev);
    }
  }
  prefs.end();
  return result;
}

int api_get_rotation_interval() { return cached_rotation_interval_ms; }

// ============================================================================
// WEBSOCKET
// ============================================================================
static bool estrai_data(JsonDocument &doc, JsonDocument &out) {
  JsonVariant data = doc["data"];
  if (data.isNull())
    return false;
  if (data.is<const char *>()) {
    return deserializeJson(out, data.as<const char *>()) ==
           DeserializationError::Ok;
  }
  out.set(data);
  return true;
}

static String estrai_azione(JsonVariantConst payload, uint8_t depth = 0) {
  if (depth > 2)
    return "";

  String action = payload["action"] | "";
  if (action.isEmpty())
    action = payload["action_name"] | "";
  if (action.isEmpty())
    action = payload["command_name"] | "";
  if (action.isEmpty())
    action = payload["type"] | "";
  if (action.isEmpty())
    action = payload["name"] | "";

  JsonVariantConst command = payload["command"];
  if (action.isEmpty() && command.is<const char *>())
    action = command.as<const char *>();
  if (action.isEmpty() && command.is<JsonObjectConst>()) {
    action = command["action"] | "";
    if (action.isEmpty())
      action = command["name"] | "";
    if (action.isEmpty())
      action = command["type"] | "";
  }
  if (!action.isEmpty())
    return action;

  JsonVariantConst nested = payload["data"];
  if (nested.is<JsonObjectConst>())
    return estrai_azione(nested, depth + 1);
  return "";
}

// parte ota
static void log_progress(int cur, int total) {
  static int last = -1;
  int pct = total > 0 ? (cur * 100) / total : 0;
  if (pct / 10 != last) {
    last = pct / 10;
    Serial.printf("[OTA] Download %d%% (%d/%d)\n", pct, cur, total);
  }
}

static void esegui_aggiornamento_ota(const char *firmware_url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "[OTA] Connessione Wi-Fi assente, impossibile procedere con l'OTA.");
    return;
  }

  Serial.printf("[OTA] Avvio aggiornamento OTA da URL: %s\n", firmware_url);

  bool locked = xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(5000));

  WiFiClient updateClient;
  updateClient.setTimeout(15);
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpUpdate.onProgress(log_progress);

  t_httpUpdate_return ret = httpUpdate.update(updateClient, firmware_url);

  if (locked) {
    xSemaphoreGive(lvgl_mutex);
  }

  switch (ret) {
  case HTTP_UPDATE_FAILED:
    Serial.printf("[OTA] Aggiornamento FALLITO. Errore (%d): %s\n",
                  httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
    break;

  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("[OTA] Nessun aggiornamento trovato.");
    break;

  case HTTP_UPDATE_OK:
    Serial.println("[OTA] Aggiornamento RIUSCITO! Riavvio del dispositivo...");
    delay(200);
    ESP.restart();
    break;
  }
}

static bool esegui_comando_remoto(JsonVariantConst payload,
                                  const char *origine) {
  String action = estrai_azione(payload);
  action.trim();
  action.toLowerCase();
  action.replace('_', '-');

  bool isRestart = (action == "restart");
  bool isReset = (action == "reset");
  bool isShutdown = (action == "shutdown");
  bool isOta = (action == "ota");

  if (!isRestart && !isReset && !isShutdown && !isOta)
    return false;

  String cmdId = payload["id"] | "";
  if (cmdId.isEmpty())
    cmdId = payload["command"]["id"] | "";

  Serial.printf("[API] Comando remoto via %s: %s (ID: %s)\n", origine,
                action.c_str(), cmdId.c_str());

  if (isRestart) {
    Serial.println("[API] Esecuzione riavvio remoto...");
    delay(200);
    ESP.restart();
  }

  if (isReset) {
    Serial.println("[API] Esecuzione reset di fabbrica remoto...");
    Preferences prefsWifi;
    prefsWifi.begin("wifi-config", false);
    prefsWifi.clear();
    prefsWifi.end();

    Preferences prefsSettings;
    prefsSettings.begin("esp-settings", false);
    prefsSettings.clear();
    prefsSettings.end();

    Preferences prefsCache;
    prefsCache.begin("api-cache", false);
    prefsCache.clear();
    prefsCache.end();

    delay(200);
    ESP.restart();
  }

  if (isShutdown) {
    Serial.println("[API] Esecuzione spegnimento remoto (Deep Sleep)...");
    spegni_display();
    delay(200);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_deep_sleep_start();
  }

  if (isOta) {
    String fwUrl = payload["payload"]["firmware_url"] | "";
    if (fwUrl.isEmpty()) {
      fwUrl = payload["firmware_url"] | "";
    }

    if (fwUrl.length() > 0) {
      esegui_aggiornamento_ota(fwUrl.c_str());
    } else {
      Serial.println("[API] Errore: URL firmware mancante nel comando OTA.");
    }
  }

  return true;
}

static String richiedi_auth_canale(const String &socketId,
                                   const String &channel) {
  WiFiClientSecure &client = https_client();

  HTTPClient http;
  http.setReuse(true);
  if (!http.begin(client, CHANNEL_AUTH_URL)) {
    Serial.println("[WebSocket] Auth: HTTP begin fallita");
    return "";
  }
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "1");

  int code = http.POST("socket_id=" + socketId + "&channel_name=" + channel);

  String auth;
  if (code == 200) {
    JsonDocument d;
    if (deserializeJson(d, http.getString()) == DeserializationError::Ok) {
      auth = d["auth"] | "";
    }
  } else {
    String body = http.getString();
    Serial.printf("[WebSocket] Auth canale fallita, HTTP %d: %.150s\n", code,
                  body.c_str());
  }
  http.end();
  return auth;
}

static void tenta_subscribe_canale(const String &channel, bool &is_subscribed) {
  if (!ws_connected || is_subscribed || ws_socket_id.isEmpty() ||
      channel.isEmpty())
    return;

  bool privato =
      channel.startsWith("private-") || channel.startsWith("presence-");

  String auth;
  if (privato) {
    auth = richiedi_auth_canale(ws_socket_id, channel);
    if (auth.isEmpty())
      return;
  }

  JsonDocument sub;
  sub["event"] = "pusher:subscribe";
  sub["data"]["channel"] = channel;
  if (!auth.isEmpty())
    sub["data"]["auth"] = auth;

  String msg;
  serializeJson(sub, msg);
  webSocket.sendTXT(msg);
  Serial.printf("[WebSocket] Iscrizione inviata al canale: %s\n",
                channel.c_str());
}

static void tenta_subscribe() {
  uint32_t now = millis();
  if (!scaduto(now, ws_next_subscribe_ms))
    return;
  ws_next_subscribe_ms = now + SUBSCRIBE_RETRY_MS;

  tenta_subscribe_canale(ws_device_channel, ws_device_subscribed);
  tenta_subscribe_canale(ws_org_channel, ws_org_subscribed);
}

static void gestisci_messaggio_ws(const char *payload, size_t length) {
  Serial.printf("[WebSocket] Messaggio: %.200s%s\n", payload,
                length > 200 ? "..." : "");

  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok)
    return;

  String eventName = doc["event"] | "";

  if (eventName == "pusher:connection_established") {
    JsonDocument d;
    if (!estrai_data(doc, d))
      return;
    ws_socket_id = d["socket_id"] | "";
    ws_device_subscribed = false;
    ws_org_subscribed = false;
    ws_next_subscribe_ms = millis();
    Serial.printf("[WebSocket] Handshake OK (socket_id %s)\n",
                  ws_socket_id.c_str());
    return;
  }

  if (eventName == "pusher:ping") {
    webSocket.sendTXT("{\"event\":\"pusher:pong\",\"data\":{}}");
    return;
  }

  if (eventName == "pusher_internal:subscription_succeeded") {
    String subChannel = doc["channel"] | "";
    if (subChannel.isEmpty())
      subChannel = doc["data"]["channel"] | "";
    if (subChannel == ws_device_channel) {
      ws_device_subscribed = true;
    } else if (subChannel == ws_org_channel) {
      ws_org_subscribed = true;
    }
    Serial.printf("[WebSocket] Iscrizione confermata per: %s\n",
                  subChannel.c_str());
    return;
  }

  if (eventName.startsWith("pusher"))
    return;

  JsonDocument d;
  if (!estrai_data(doc, d)) {
    d = doc;
  }

  // 1. GESTIONE COMANDI REMOTI
  if (esegui_comando_remoto(d.as<JsonVariantConst>(), "WebSocket") ||
      esegui_comando_remoto(doc.as<JsonVariantConst>(), "WebSocket")) {
    return;
  }

  // 2. GESTIONE IMPOSTAZIONI E EVENTI
  JsonObject settings = d["settings"].as<JsonObject>();
  if (!settings.isNull() && settings["rotation_interval_seconds"].is<int>()) {
    int rot_sec = settings["rotation_interval_seconds"] | 30;
    if (rot_sec >= 5) {
      cached_rotation_interval_ms = rot_sec * 1000;
      Serial.printf("[API] Intervallo rotazione aggiornato da WS: %d s\n",
                    rot_sec);
    }
  }

  JsonArray events = settings["events"].as<JsonArray>();
  if (events.isNull())
    events = d["events"].as<JsonArray>();
  if (!events.isNull()) {
    salva_eventi(events, "WebSocket");
  }
}

static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WebSocket] Disconnesso dal server.");
    ws_connected = false;
    ws_device_subscribed = false;
    ws_org_subscribed = false;
    ws_socket_id = "";
    break;
  case WStype_CONNECTED:
    Serial.printf("[WebSocket] Connesso al server! Path: %s\n",
                  payload ? (const char *)payload : "");
    ws_connected = true;
    break;
  case WStype_TEXT:
    if (payload)
      gestisci_messaggio_ws((const char *)payload, length);
    break;
  case WStype_ERROR:
    Serial.printf("[WebSocket] Errore: %.*s\n", (int)length,
                  payload ? (const char *)payload : "");
    break;
  default:
    break;
  }
}

static void avvia_websocket(String host, uint16_t port, const String &appKey,
                            const String &channel, const String &orgChannel,
                            const String &scheme) {
  if (ws_initialized)
    return;
  ws_device_channel = channel;
  ws_org_channel = orgChannel;

  bool useTls = (WS_TLS_MODE < 0) ? (scheme == "https" || scheme == "wss")
                                  : (WS_TLS_MODE == 1);
  String wsPath =
      "/ws/app/" + appKey + "?protocol=7&client=esp32&version=7.4.0";
  Serial.printf("[WebSocket] %s verso %s:%u path %s\n", useTls ? "WSS" : "WS",
                host.c_str(), (unsigned)port, wsPath.c_str());

  if (useTls) {
    webSocket.beginSSL(host.c_str(), port, wsPath.c_str());
  } else {
    webSocket.begin(host.c_str(), port, wsPath.c_str());
  }
  webSocket.setExtraHeaders(
      (String("Origin: http") + (useTls ? "s" : "") + "://" + host).c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 5000, 2);
  ws_initialized = true;
}

// ============================================================================
// HEARTBEAT HTTP
// ============================================================================
static bool esegui_heartbeat() {
  WiFiClientSecure &client = https_client();
  HTTPClient http;
  http.setReuse(true);

  if (!http.begin(client, HEARTBEAT_URL)) {
    Serial.println("[API] HTTP begin fallita per l'endpoint heartbeat.");
    return false;
  }

  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "1");

  JsonDocument doc;
  doc["mac_address"] = DEVICE_MAC;
  doc["type"] = DEVICE_TYPE;
  doc["software_version"] = get_current_firmware_version();
  doc["connection_type"] = "wifi";

  JsonObject telemetry = doc["telemetry"].to<JsonObject>();
  telemetry["uptime_seconds"] = millis() / 1000;
  telemetry["free_heap_bytes"] = ESP.getFreeHeap();
  telemetry["wifi_rssi"] = WiFi.RSSI();

  String requestBody;
  serializeJson(doc, requestBody);

  int code = http.POST(requestBody);
  if (code < 200 || code >= 300) {
    String error = http.errorToString(code);
    String response = http.getString();
    Serial.printf("[API] Heartbeat fallito: HTTP %d (%s), RSSI %d dBm.\n", code,
                  error.c_str(), WiFi.RSSI());

    JsonDocument errorDoc;
    if (deserializeJson(errorDoc, response) == DeserializationError::Ok) {
      String message = errorDoc["message"] | "";
      if (message.isEmpty())
        message = errorDoc["error"] | "";
      if (!message.isEmpty())
        Serial.printf("[API] Risposta CRM: %.160s\n", message.c_str());
    }
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  JsonDocument res;
  DeserializationError parseError = deserializeJson(res, response);
  if (parseError) {
    Serial.printf("[API] Risposta heartbeat non valida: %s (%u byte).\n",
                  parseError.c_str(), (unsigned)response.length());
    return false;
  }

  JsonArray commands = res["commands"].as<JsonArray>();
  if (!commands.isNull()) {
    for (JsonVariant command : commands) {
      if (esegui_comando_remoto(command.as<JsonVariantConst>(), "heartbeat"))
        return true;
    }
  }

  uint32_t sec = res["heartbeat_interval_seconds"] | 30;
  if (sec < 10)
    sec = 10;
  if (sec > 300)
    sec = 300;
  heartbeat_interval_ms = sec * 1000UL;

  JsonObject settings = res["settings"].as<JsonObject>();
  if (!settings.isNull() && settings["rotation_interval_seconds"].is<int>()) {
    int rot_sec = settings["rotation_interval_seconds"] | 30;
    if (rot_sec >= 5) {
      cached_rotation_interval_ms = rot_sec * 1000;
    }
  }

  JsonArray events = settings["events"].as<JsonArray>();
  if (!events.isNull()) {
    salva_eventi(events, "Heartbeat");
  }

  JsonObject wsConfig = res["websocket"].as<JsonObject>();
  if (!wsConfig.isNull() && !ws_initialized) {
    if (wsConfig["enabled"] | false) {
      String host = wsConfig["host"] | "";
      uint16_t port = wsConfig["port"] | 443;
      String appKey = wsConfig["app_key"] | "";
      String channel = wsConfig["channel"] | "";
      String orgChannel = wsConfig["org_channel"] | "";
      String scheme = wsConfig["scheme"] | "https";
      if (host.length() > 0 && appKey.length() > 0) {
        avvia_websocket(host, port, appKey, channel, orgChannel, scheme);
      }
    }
  }
  return true;
}

void gestisci_websocket_periodico() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (ws_initialized) {
    webSocket.loop();
    tenta_subscribe();
  }

  uint32_t now = millis();
  if (!force_sync && heartbeat_scheduled && !scaduto(now, next_heartbeat_ms))
    return;
  force_sync = false;

  bool ok = esegui_heartbeat();
  heartbeat_scheduled = true;
  if (ok) {
    heartbeat_failures = 0;
  } else {
    heartbeat_failures++;
    Serial.println("[API] Fallimento heartbeat: Server temporaneamente "
                   "offline o timeout.");
  }

  next_heartbeat_ms =
      millis() + (ok ? heartbeat_interval_ms : HEARTBEAT_RETRY_MS);
}

void richiedi_aggiornamento_api() { force_sync = true; }

bool gestisci_aggiornamento_api_periodico() { return false; }

void inizializza_api(WebServer &server) {
  server.on("/api/events", HTTP_GET, [&server]() {
    JsonDocument doc;
    JsonArray data = doc["data"].to<JsonArray>();
    for (const ApiEvent &e : events_cache) {
      JsonObject o = data.add<JsonObject>();
      o["id"] = e.id;
      o["name"] = e.name;
      o["qr"] = e.qr;
      o["reservations"] = e.reservations;
      o["participants"] = e.participants;
    }
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
}

// ============================================================================
// Logo
// ============================================================================

static const char *ORGANIZATION_LOGO_URL =
    "https://demo.relaicrm.com/api/organization-logo";
static std::vector<uint8_t> organization_logo_cache;
static bool organization_logo_loaded = false;

static int valore_hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool firma_png_valida(const uint8_t *data, size_t size) {
  static const uint8_t signature[] = {0x89, 0x50, 0x4e, 0x47,
                                      0x0d, 0x0a, 0x1a, 0x0a};
  if (data == nullptr || size < 24 ||
      memcmp(data, signature, sizeof(signature)) != 0)
    return false;

  uint32_t width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                   ((uint32_t)data[18] << 8) | data[19];
  uint32_t height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                    ((uint32_t)data[22] << 8) | data[23];
  return width > 0 && height > 0 && width <= 1024 && height <= 1024;
}

static bool decodifica_hex_png(const String &hex,
                               std::vector<uint8_t> &png_bytes) {
  if (hex.isEmpty() || hex.length() > 1024 * 1024)
    return false;

  png_bytes.clear();
  png_bytes.reserve(hex.length() / 2);
  int high_nibble = -1;

  for (size_t i = 0; i < hex.length(); ++i) {
    char c = hex[i];
    if (isspace((unsigned char)c) || c == ':' || c == ',' || c == '-')
      continue;
    if (c == '0' && i + 1 < hex.length() &&
        (hex[i + 1] == 'x' || hex[i + 1] == 'X')) {
      ++i;
      continue;
    }

    int nibble = valore_hex(c);
    if (nibble < 0)
      return false;
    if (high_nibble < 0) {
      high_nibble = nibble;
    } else {
      png_bytes.push_back((uint8_t)((high_nibble << 4) | nibble));
      high_nibble = -1;
    }
  }

  return high_nibble < 0 &&
         firma_png_valida(png_bytes.data(), png_bytes.size());
}

static String estrai_hex_logo(const String &body) {
  JsonDocument json;
  if (deserializeJson(json, body) != DeserializationError::Ok)
    return body;

  if (json.is<const char *>())
    return json.as<String>();

  const char *fields[] = {"logo_hex", "hex", "logo", "image", "data"};
  for (const char *field : fields) {
    JsonVariantConst value = json[field];
    if (value.is<const char *>())
      return value.as<String>();
    if (value.is<JsonObjectConst>()) {
      String nested = value["hex"] | "";
      if (nested.isEmpty())
        nested = value["data"] | "";
      if (!nested.isEmpty())
        return nested;
    }
  }
  return "";
}

static const char *LOGO_FILE_PATH = "/logo.png";

static void logo_carica_da_flash() {
  if (!organization_logo_cache.empty() || !LittleFS.exists(LOGO_FILE_PATH))
    return;
  File f = LittleFS.open(LOGO_FILE_PATH, "r");
  if (!f)
    return;
  size_t len = f.size();
  if (len >= 24 && len <= 1024 * 1024) {
    std::vector<uint8_t> buf(len);
    if (f.read(buf.data(), len) == len &&
        firma_png_valida(buf.data(), buf.size())) {
      organization_logo_cache.swap(buf);
    }
  }
  f.close();
}

static void logo_salva_su_flash(const std::vector<uint8_t> &data) {
  File f = LittleFS.open(LOGO_FILE_PATH, "w");
  if (!f)
    return;
  f.write(data.data(), data.size());
  f.close();
}

void api_logo_elimina() {
  if (LittleFS.exists(LOGO_FILE_PATH))
    LittleFS.remove(LOGO_FILE_PATH);
  organization_logo_cache.clear();
  organization_logo_loaded = false;
}

// Non bloccante: restituisce cache RAM o copia in flash.
const std::vector<uint8_t> &api_get_organization_logo_bytes() {
  logo_carica_da_flash();
  return organization_logo_cache;
}

// Scarica il logo dal server ritorna true se il logo e' cambiato.
bool api_logo_aggiorna() {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure &client = https_client();
  HTTPClient http;
  http.setReuse(false);

  int code = -1;
  String body = "";

  if (!http.begin(client, ORGANIZATION_LOGO_URL))
    return false;

  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  http.addHeader("Accept", "image/png, text/plain, application/json");
  http.addHeader("ngrok-skip-browser-warning", "1");

  code = http.GET();
  if (code >= 200 && code < 300) {
    int len = http.getSize();
    if (len > 0 && len <= 1024 * 1024)
      body = http.getString();
  }
  http.end();

  if (code < 200 || code >= 300 || body.isEmpty()) {
    Serial.printf("[API] Richiesta logo fallita: HTTP %d\n", code);
    return false;
  }

  std::vector<uint8_t> logo_bytes;
  if (body.length() >= 24 &&
      firma_png_valida((const uint8_t *)body.c_str(), body.length())) {
    logo_bytes.assign((const uint8_t *)body.c_str(),
                      (const uint8_t *)body.c_str() + body.length());
  } else {
    String hex = estrai_hex_logo(body);
    if (!decodifica_hex_png(hex, logo_bytes)) {
      Serial.println("[API] Logo ricevuto ma non è un PNG hex valido.");
      return false;
    }
  }

  organization_logo_loaded = true;
  logo_carica_da_flash();
  if (logo_bytes == organization_logo_cache)
    return false;

  organization_logo_cache = logo_bytes;
  logo_salva_su_flash(logo_bytes);
  Serial.printf("[API] Logo PNG caricato: %u byte.\n",
                (unsigned)organization_logo_cache.size());
  return true;
}

bool api_logo_scaricato() { return organization_logo_loaded; }