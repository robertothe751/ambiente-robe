#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <DNSServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

void inizializza_sistema_reti();

bool connetti_wifi_salvato();
void avvia_wifi_provisioning();
bool wifi_provisioning_attivo();
void disconnetti_e_cancella_wifi();
bool wifi_reset_in_corso();
String nome_wifi_salvato();

void avvia_wifi_ibrido();
void ferma_wifi_ibrido();
bool wifi_ibrido_attivo();
bool wifi_api_connessa();
int wifi_stato_api();
bool consuma_richiesta_ricarica_impostazioni();

void gestisci_wifi_prov();

void get_system_status_doc(JsonDocument &doc);