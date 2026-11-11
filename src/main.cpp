#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "api.h"
#include "display_config.h"
#include "interfaccia_ui.h"
#include "wifi_conessione.h"

#include <esp_log.h>

TaskHandle_t NetworkTaskHandle = NULL;
TaskHandle_t DisplayTaskHandle = NULL;

// ----------------------------------------------------
// Tasks FreeRTOS (Dual-Core)
// ----------------------------------------------------

static void log_heap_status(void) {
  size_t free = esp_get_free_heap_size();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  float frag = (free > 0) ? (1.0f - ((float)largest / (float)free)) : 0.0f;

  // Usiamo Serial.printf per garantire la stampa visibile sul monitor seriale
  // Arduino
  Serial.printf(
      "[HEAP] Free: %u bytes | Largest block: %u bytes | Frag: %.1f%%\n",
      (unsigned)free, (unsigned)largest, frag * 100.0f);
}

// Core 0: Gestione Rete, Server Web permanente e Stato
void networkTask(void *pvParameters) {
  // Avvia permanentemente AP (configurazione) + STA (router) contemporaneamente
  avvia_wifi_provisioning();

  uint32_t disconnect_start_time = 0;
  uint32_t last_reconnect_attempt = 0;
  uint32_t last_status_refresh = 0;
  uint32_t last_ota_check = 0; // Timer per il controllo periodico OTA
  bool wifi_error_box_active = false;
  bool wifi_connected = false;
  String saved_ssid = nome_wifi_salvato();
  uint32_t last_logo_attempt = 0;

  while (true) {
    // Download logo fuori dal mutex LVGL: la UI non si blocca.
    if (!api_logo_scaricato() && WiFi.status() == WL_CONNECTED &&
        (last_logo_attempt == 0 || millis() - last_logo_attempt > 30000)) {
      last_logo_attempt = millis();
      if (api_logo_aggiorna())
        ui_richiedi_ricarica_tileview();
    }

    if (millis() - last_status_refresh >= 1000) {
      // log_heap_status();

      saved_ssid = nome_wifi_salvato();
      if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100))) {
        ui_set_connection_status(wifi_stato_api(), wifi_connected, saved_ssid);
        ui_set_wifi_setup_state(wifi_provisioning_attivo(), saved_ssid);
        xSemaphoreGive(lvgl_mutex);
      }
      last_status_refresh = millis();
    }

    gestisci_wifi_prov();

    wifi_connected = (WiFi.status() == WL_CONNECTED);

    if (!wifi_connected) {
      if (saved_ssid.isEmpty()) {
        disconnect_start_time = 0;
        if (wifi_error_box_active &&
            xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100))) {
          close_wifi_error_msgbox();
          xSemaphoreGive(lvgl_mutex);
          wifi_error_box_active = false;
        }
      } else if (disconnect_start_time == 0) {
        disconnect_start_time = millis();
      } else if ((millis() - disconnect_start_time > 15000) &&
                 !wifi_error_box_active) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000))) {
          show_wifi_error_msgbox();
          xSemaphoreGive(lvgl_mutex);
          wifi_error_box_active = true;
          Serial.println(
              "[UI] Mostrata Message Box: Connessione router assente da > 15s");
        }
      }

      if (millis() - last_reconnect_attempt >= 5000) {
        saved_ssid = nome_wifi_salvato();
        last_reconnect_attempt = millis();
      }
    } else {
      if (wifi_error_box_active) {
        if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000))) {
          close_wifi_error_msgbox();
          xSemaphoreGive(lvgl_mutex);
          wifi_error_box_active = false;
          Serial.println("[UI] Connessione router ripristinata.");
        }
      }
      disconnect_start_time = 0;

      /*
      // Esegue il controllo OTA ogni 60 secondi quando il Wi-Fi è connesso
      if (millis() - last_ota_check >= 6 * 10000) {
        controlla_aggiornamento_ota();
        last_ota_check = millis();
      }
      */
    }

    if (millis() - last_status_refresh >= 1000) {
      if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(100))) {
        saved_ssid = nome_wifi_salvato();
        ui_set_connection_status(wifi_stato_api(), wifi_connected, saved_ssid);
        ui_set_wifi_setup_state(wifi_provisioning_attivo(), saved_ssid);
        xSemaphoreGive(lvgl_mutex);
      }
      last_status_refresh = millis();
    }

    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000))) {
      if (ui_consuma_richiesta_ricarica()) {
        create_tileview();
      }
      xSemaphoreGive(lvgl_mutex);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Core 1: Gestione Display, Touch e Rendering LVGL
void displayTask(void *pvParameters) {
  if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY)) {
    create_tileview();
    xSemaphoreGive(lvgl_mutex);
  }

  while (true) {
    uint32_t time_till_next = 5;

    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(10))) {
      time_till_next = lv_timer_handler();
      xSemaphoreGive(lvgl_mutex);
    }

    if (time_till_next < 1)
      time_till_next = 1;
    if (time_till_next > 10)
      time_till_next = 10;

    vTaskDelay(pdMS_TO_TICKS(time_till_next));
  }
}

// ----------------------------------------------------
// Setup & Loop
// ----------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n--- Avvio ESP32-S3 ---");

  inizializza_sistema_reti();

  init_display_touch_lvgl();
  ui_load_cached_data();

  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 24576, NULL, 1,
                          &NetworkTaskHandle, 0);
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 16384, NULL, 2,
                          &DisplayTaskHandle, 1);

  Serial.println("[SYSTEM] Avvio completato.");
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }