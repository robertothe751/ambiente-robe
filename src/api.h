#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <vector>

#include "interfaccia_ui.h"

struct ApiEvent {
  long id;
  String name;
  String qr;
  int reservations;
  int participants;
};

void inizializza_api(WebServer &server);
String get_current_firmware_version();
bool gestisci_aggiornamento_api_periodico();
void richiedi_aggiornamento_api();
void gestisci_websocket_periodico();

std::vector<ApiEvent> api_get_cached_events();
int api_get_rotation_interval();

const std::vector<uint8_t> &api_get_organization_logo_bytes();
bool api_logo_aggiorna();
bool api_logo_scaricato();
void api_logo_elimina();