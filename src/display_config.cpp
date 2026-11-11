#include "display_config.h"
#include "pin_config.h"

// ----------------------------------------------------
// Variabili Globali e Risorse Hardware
// ----------------------------------------------------
SemaphoreHandle_t lvgl_mutex = NULL;

// Bus QSPI e Display CO5300
Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0,
                                             LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *gfx = new Arduino_CO5300(bus, LCD_RESET, 0 /* Rotazione */);

// Touch Driver
TouchDrvCSTXXX touch;

// Variabili di stato LVGL
static uint16_t screenWidth = LCD_WIDTH;
static uint16_t screenHeight = LCD_HEIGHT;
lv_display_t *display = NULL;
static uint16_t *buf1 = NULL;
static uint16_t *buf2 = NULL;

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2
#define TOUCH_INVERT_X true
#define TOUCH_INVERT_Y true
#define TOUCH_SWAP_XY false

// ----------------------------------------------------
// Callbacks Interne LVGL
// ----------------------------------------------------

// Tick Timer per incrementare il tempo interno di LVGL
static void lvgl_tick_cb(void *arg) {
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

// Inviatore di pixel da LVGL verso Arduino_GFX (Flush)
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area,
                        uint8_t *px_map) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
  lv_display_flush_ready(disp);
}

// Lettura delle coordinate dal chip Touch
static void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
  auto touch_points = touch.getTouchPoints();

  if (touch_points.hasPoints()) {
    const auto &point = touch_points.getPoint(0);
    int16_t raw_x = point.x;
    int16_t raw_y = point.y;

    if (TOUCH_SWAP_XY) {
      int16_t temp = raw_x;
      raw_x = raw_y;
      raw_y = temp;
    }

    if (TOUCH_INVERT_X) {
      raw_x = LCD_WIDTH - 1 - raw_x;
    }

    if (TOUCH_INVERT_Y) {
      raw_y = LCD_HEIGHT - 1 - raw_y;
    }

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = raw_x;
    data->point.y = raw_y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// Arrotondamento aree per la gestione corretta dei pixel pari/dispari del
// display
static void rounder_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);

  area->x1 = area->x1 & ~1;
  area->y1 = area->y1 & ~1;

  area->x2 = area->x2 | 1;
  area->y2 = area->y2 | 1;
}

// ----------------------------------------------------
// Inizializzazione Touch Hardware
// ----------------------------------------------------
static void init_touch() {
  // Wire.begin lo esegue la libreria touch: farlo qui causa errori setPins().
  bool ok = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  if (!ok) {
    // Serial.println("[TOUCH] Inizializzazione fallita!");
    while (1) {
      delay(1000);
    }
  }

  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  // Serial.println("[TOUCH] Inizializzato con successo.");
}

// ----------------------------------------------------
// Funzione Principale di Inizializzazione
// ----------------------------------------------------
void init_display_touch_lvgl() {
#ifdef AMOLED_PWR_EN
  pinMode(AMOLED_PWR_EN, OUTPUT);
  digitalWrite(AMOLED_PWR_EN, HIGH);
#endif

  // 1. Avvio Display GFX
  gfx->begin(80000000);
  ((Arduino_CO5300 *)gfx)->setBrightness(255);
  gfx->fillScreen(RGB565_BLACK);

  // 2. Avvio Touch Hardware
  init_touch();

  // 3. Inizializzazione Core LVGL
  lv_init();

  // Allocazione diretta dei buffer di rendering in PSRAM (più grandi per
  // performance ottimali)
  uint32_t buffer_pixels = LCD_WIDTH * (LCD_HEIGHT / 2);

  buf1 =
      (uint16_t *)heap_caps_aligned_alloc(64, buffer_pixels * sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  buf2 =
      (uint16_t *)heap_caps_aligned_alloc(64, buffer_pixels * sizeof(uint16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!buf1 || !buf2) {
    if (buf1)
      free(buf1);
    if (buf2)
      free(buf2);

    while (1) {
      delay(1000); // Errore critico: PSRAM non disponibile o insufficiente
    }
  }

  // Creazione e Configurazione del Display LVGL 9
  display = lv_display_create(screenWidth, screenHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display, buf1, buf2, buffer_pixels * sizeof(uint16_t),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_timer_t *refr_timer = lv_display_get_refr_timer(display);
  if (refr_timer) {
    lv_timer_set_period(refr_timer, 16); // ~60 FPS
  }

  lv_display_set_flush_cb(display, my_flush_cb);
  lv_display_add_event_cb(display, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  // 4. Configurazione Driver Touch per LVGL
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touchpad_read);

  // 5. Timer Hardware Tick LVGL
  const esp_timer_create_args_t lvgl_tick_timer_args = {
      .callback = &lvgl_tick_cb, .name = "lvgl_tick"};
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

  // 6. Creazione del Mutex per la sincronizzazione FreeRTOS
  lvgl_mutex = xSemaphoreCreateMutex();

  // Serial.println("[LVGL] Inizializzazione completata.");
}

void imposta_luminosita_display(uint8_t percentuale) {
  if (!gfx)
    return;

  if (percentuale > 100)
    percentuale = 100;

  bool mutex_locked = false;
  if (lvgl_mutex) {
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return;
    mutex_locked = true;
  }

  uint8_t brightness = (uint16_t)percentuale * 255 / 100;
  ((Arduino_CO5300 *)gfx)->setBrightness(brightness);

  if (mutex_locked)
    xSemaphoreGive(lvgl_mutex);
}

void spegni_display() {
  if (!gfx)
    return;

  bool mutex_locked = false;
  if (lvgl_mutex) {
    if (xSemaphoreTake(lvgl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return;
    mutex_locked = true;
  }

  gfx->fillScreen(RGB565_BLACK);
  ((Arduino_CO5300 *)gfx)->setBrightness(0);

  if (mutex_locked)
    xSemaphoreGive(lvgl_mutex);
}