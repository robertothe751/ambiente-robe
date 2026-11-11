#include "wifi_conessione.h"
#include "api.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

static WebServer server(80);
static DNSServer dnsServer;
static bool provisioning_started = false;
static volatile bool ibrido_mode_requested = false;
static volatile bool wifi_reset_requested = false;
static volatile bool settings_reload_requested = false;
// Incrementato a ogni cambio di stato Wi-Fi, le pagine web lo usano per
// riallinearsi
static volatile uint32_t wifi_state_epoch = 0;

static std::vector<String> scan_cache;
static unsigned long scan_started_millis = 0;
static bool scan_paused_sta = false;

const byte DNS_PORT = 53;

const char *AP_SSID = "ESP32-Config";
const char *AP_PASS = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);

// Variabili per il calcolo del tempo di disconnessione
static unsigned long wifi_disconnect_start_millis = 0;
static bool wifi_was_connected = true;

// ============================================================================
// GESTIONE STATO WI-FI E PREFERENCES
// ============================================================================
String nome_wifi_salvato() {
  Preferences preferences;
  // RW crea il namespace se manca, evitando NOT_FOUND
  preferences.begin("wifi-config", false);
  String ssid =
      preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  preferences.end();
  return ssid;
}

bool connetti_wifi_salvato() { return WiFi.status() == WL_CONNECTED; }

bool wifi_provisioning_attivo() { return provisioning_started; }

bool wifi_api_connessa() { return WiFi.status() == WL_CONNECTED; }

int wifi_stato_api() { return wifi_api_connessa() ? 1 : 0; }

bool consuma_richiesta_ricarica_impostazioni() {
  if (!settings_reload_requested)
    return false;
  settings_reload_requested = false;
  return true;
}

// ============================================================================
// GESTIONE CAPTIVE PORTAL E FILE SYSTEM (LITTLEFS)
// ============================================================================
static bool isCaptivePortal() {
  String hostHeader = server.hostHeader();
  return hostHeader != "192.168.4.1" && hostHeader != "192.168.4.1:80";
}

static bool handleFileRead(String path) {
  if (path.endsWith("/")) {
    path += "index.html";
  }

  String possiblePaths[2];
  possiblePaths[0] = path;
  possiblePaths[1] = path.startsWith("/siti_web") ? path : ("/siti_web" + path);

  for (int i = 0; i < 2; i++) {
    String currentPath = possiblePaths[i];
    String pathWithGz = currentPath + ".gz";

    if (LittleFS.exists(pathWithGz)) {
      File file = LittleFS.open(pathWithGz, "r");
      if (file) {
        server.sendHeader("Content-Encoding", "gzip");
        server.sendHeader("Cache-Control", "no-cache");

        String contentType = "text/plain";
        if (currentPath.endsWith(".html"))
          contentType = "text/html";
        else if (currentPath.endsWith(".css"))
          contentType = "text/css";
        else if (currentPath.endsWith(".js"))
          contentType = "text/javascript";

        server.streamFile(file, contentType);
        file.close();
        return true;
      }
    }

    if (LittleFS.exists(currentPath)) {
      File file = LittleFS.open(currentPath, "r");
      if (file) {
        server.sendHeader("Cache-Control", "no-cache");

        String contentType = "text/plain";
        if (currentPath.endsWith(".html"))
          contentType = "text/html";
        else if (currentPath.endsWith(".css"))
          contentType = "text/css";
        else if (currentPath.endsWith(".js"))
          contentType = "text/javascript";

        server.streamFile(file, contentType);
        file.close();
        return true;
      }
    }
  }

  return false;
}

static void handle_root() {
  if (provisioning_started && isCaptivePortal()) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
    return;
  }
  handleFileRead("/index.html");
}

static void handle_not_found() {
  String uri = server.uri();

  if (uri.indexOf("generate_204") != -1 || uri.indexOf("ncsi.txt") != -1 ||
      uri.indexOf("connecttest.txt") != -1) {
    server.send(204, "text/plain", "");
    return;
  }

  if (provisioning_started && isCaptivePortal()) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
    return;
  }

  if (handleFileRead(uri))
    return;

  server.send(404, "text/plain", "404: File Not Found");
}

// ============================================================================
// ENDPOINT SCANSIONE RETI E STATO
// ============================================================================
static void riprendi_sta_dopo_scan() {
  if (!scan_paused_sta)
    return;
  scan_paused_sta = false;
  Preferences preferences;
  preferences.begin("wifi-config", false);
  String ssid =
      preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  String password = preferences.isKey("password")
                        ? preferences.getString("password", "")
                        : "";
  preferences.end();
  if (!ssid.isEmpty() && WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
}

static void avvia_scansione() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    scan_paused_sta = true;
  }
  WiFi.scanNetworks(true);
  scan_started_millis = millis();
}

static void handle_scan() {
  int16_t n = WiFi.scanComplete();

  if (n >= 0) {
    scan_cache.clear();
    for (int i = 0; i < n; ++i) {
      String netName = WiFi.SSID(i);
      if (netName.isEmpty())
        continue;
      bool duplicate = false;
      for (const String &existing : scan_cache) {
        if (existing == netName) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate)
        scan_cache.push_back(netName);
    }
    WiFi.scanDelete();
    riprendi_sta_dopo_scan();
  } else if (n == WIFI_SCAN_RUNNING) {
    if (millis() - scan_started_millis > 15000) {
      WiFi.scanDelete();
      riprendi_sta_dopo_scan();
      avvia_scansione();
    }
  } else {
    if (server.hasArg("refresh") || scan_cache.empty()) {
      avvia_scansione();
    }
  }

  bool scanning = (WiFi.scanComplete() == WIFI_SCAN_RUNNING);
  JsonDocument doc;
  doc["scanning"] = scanning;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (const String &network : scan_cache) {
    networks.add(network);
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// Funzione centrale unica per la generazione del JSON di stato del sistema
void get_system_status_doc(JsonDocument &doc) {
  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    wifi_was_connected = true;
    wifi_disconnect_start_millis = millis();
  } else {
    if (wifi_was_connected) {
      wifi_was_connected = false;
      wifi_disconnect_start_millis = millis();
    }
  }

  unsigned long elapsed =
      connected ? 0 : (millis() - wifi_disconnect_start_millis) / 1000;

  doc["network_connected"] = connected;
  doc["ssid"] = connected ? WiFi.SSID() : nome_wifi_salvato();
  doc["firmware"] = get_current_firmware_version();
  doc["elapsed_seconds"] = elapsed;
  doc["wifi_configured"] = !nome_wifi_salvato().isEmpty();
  doc["state_epoch"] = wifi_state_epoch;
}

static void handle_get_status() {
  JsonDocument doc;
  get_system_status_doc(doc);

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void handle_connect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) {
    server.send(400, "application/json",
                "{\"status\":\"error\", \"message\":\"Dati non validi.\"}");
    return;
  }

  Serial.println("[Wi-Fi Prov] Tentativo di connessione STA a: " + ssid);

  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING)
    WiFi.scanDelete();
  scan_paused_sta = false;
  WiFi.disconnect(false, true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  const unsigned long start = millis();
  wl_status_t st = WiFi.status();
  while (st != WL_CONNECTED && millis() - start < 10000) {
    if (st == WL_CONNECT_FAILED && millis() - start > 500)
      break;
    if (st == WL_NO_SSID_AVAIL && millis() - start > 3000)
      break;
    delay(100);
    st = WiFi.status();
  }

  if (st == WL_CONNECTED) {
    Serial.println("[Wi-Fi Prov] Connessione STA RIUSCITA!");
    Preferences preferences;
    preferences.begin("wifi-config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", pass);
    preferences.end();
    wifi_state_epoch += 1;
    IPAddress ip = WiFi.localIP();
    String jsonRes = "{\"status\":\"ok\", \"ip\":\"" + ip.toString() + "\"}";
    server.send(200, "application/json", jsonRes);
  } else {
    Serial.println("[Wi-Fi Prov] Connessione STA fallita, AP sempre attivo.");
    WiFi.disconnect(false, true);
    const char *msg = (st == WL_NO_SSID_AVAIL)
                          ? "Rete non raggiungibile."
                          : "Password errata o rete non raggiungibile.";
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = msg;
    String jsonRes;
    serializeJson(doc, jsonRes);
    server.send(200, "application/json", jsonRes);
    scan_paused_sta = true;
    riprendi_sta_dopo_scan();
  }
}

// ============================================================================
// INIZIALIZZAZIONE E GESTIONE AP+STA PERMANENTE (E BOTTONI DI SISTEMA)
// ============================================================================
static void esegui_reset_wifi() {
  Preferences preferences;
  preferences.begin("wifi-config", false);
  preferences.clear();
  preferences.end();

  scan_paused_sta = false;
  WiFi.disconnect(false, true);
  wifi_was_connected = false;
  wifi_disconnect_start_millis = millis();
  wifi_state_epoch += 1;
  Serial.println("[Wi-Fi] Reset credenziali completato.");
}

static void handle_reset_wifi() {
  esegui_reset_wifi();
  JsonDocument doc;
  get_system_status_doc(doc);
  doc["status"] = "ok";
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void handle_reboot() {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "Dispositivo in riavvio...";
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
  delay(200);
  ESP.restart();
}

static void handle_reset_factory() {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["message"] = "Ripristino di fabbrica in corso...";
  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
  delay(200);

  esegui_reset_wifi();

  Preferences prefsSettings;
  prefsSettings.begin("esp-settings", false);
  prefsSettings.clear();
  prefsSettings.end();

  Preferences prefsCache;
  prefsCache.begin("api-cache", false);
  prefsCache.clear();
  prefsCache.end();

  api_logo_elimina();

  ESP.restart();
}

void inizializza_sistema_reti() {
  if (!LittleFS.begin(true)) {
    Serial.println("[LittleFS] ERRORE: Montaggio fallito!");
  } else {
    Serial.println("[LittleFS] File system montato correttamente.");
  }

  server.on("/", HTTP_GET, handle_root);
  server.on("/scan", HTTP_GET, handle_scan);

  // Eventuali rotte API aggiuntive se necessarie
  inizializza_api(server);

  server.on("/status", HTTP_GET, handle_get_status);
  server.on("/connect", HTTP_POST, handle_connect);
  server.on("/reset_wifi", HTTP_POST, handle_reset_wifi);
  server.on("/reboot", HTTP_POST, handle_reboot);
  server.on("/reset_factory", HTTP_POST, handle_reset_factory);
  server.onNotFound(handle_not_found);

  Serial.println("[Wi-Fi] Sistema di routing HTTP configurato con successo.");
}

void avvia_wifi_provisioning() {
  server.stop();
  dnsServer.stop();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(AP_SSID, AP_PASS)) {
    Serial.println("[Wi-Fi AP] ERRORE: avvio Access Point fallito!");
  }
  dnsServer.start(DNS_PORT, "*", AP_IP);

  Preferences preferences;
  preferences.begin("wifi-config", false);
  String ssid =
      preferences.isKey("ssid") ? preferences.getString("ssid", "") : "";
  String password = preferences.isKey("password")
                        ? preferences.getString("password", "")
                        : "";
  preferences.end();

  if (!ssid.isEmpty()) {
    Serial.println("[Wi-Fi] Connessione STA in background a: " + ssid);
    WiFi.begin(ssid.c_str(), password.c_str());
  } else {
    Serial.println("[Wi-Fi] Nessuna rete salvata. Solo AP attivo.");
  }

  provisioning_started = true;
  ibrido_mode_requested = false;
  server.begin();

  Serial.println("[Wi-Fi AP] Access Point permanente avviato (" +
                 String(AP_SSID) + ").");
}

void avvia_wifi_ibrido() { ibrido_mode_requested = true; }

void ferma_wifi_ibrido() { ibrido_mode_requested = false; }

bool wifi_ibrido_attivo() { return true; }

void gestisci_wifi_prov() {
  if (wifi_reset_requested) {
    wifi_reset_requested = false;
    esegui_reset_wifi();
  }

  // Gestione periodica del loop e della sincronizzazione WebSocket
  gestisci_websocket_periodico();

  dnsServer.processNextRequest();
  server.handleClient();
}

bool wifi_reset_in_corso() { return wifi_reset_requested; }

void disconnetti_e_cancella_wifi() {
  Serial.println("[UI] Richiesta utente: Disconnessione e reset Wi-Fi.");
  wifi_reset_requested = true;
}