#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <TouchDrvCST.hpp>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lvgl.h>

// ----------------------------------------------------
// Dichiarazioni esterne
// ----------------------------------------------------
extern SemaphoreHandle_t lvgl_mutex;
extern Arduino_GFX *gfx;
extern lv_display_t *display;
extern TouchDrvCSTXXX touch;

// ----------------------------------------------------
// Prototipi delle Funzioni
// ----------------------------------------------------
void init_display_touch_lvgl();
void imposta_luminosita_display(uint8_t percentuale);
void spegni_display();