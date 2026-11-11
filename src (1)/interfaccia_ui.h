#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <WiFi.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include <vector>

void create_tileview(void);
void ui_load_cached_data(void);
void ui_set_rotation_interval(int interval_ms);
void ui_set_connection_status(int api_status, bool network_connected,
                              const String &saved_ssid);
void ui_set_wifi_setup_state(bool provisioning, const String &saved_ssid);
void show_wifi_error_msgbox(void);
void close_wifi_error_msgbox(void);
void show_qr_modal(const char *title_text, const char *qr_data);

int get_num_dynamic_pages(void);
long get_event_id_for_page(int page_idx);

void ui_richiedi_ricarica_tileview();
bool ui_consuma_richiesta_ricarica();