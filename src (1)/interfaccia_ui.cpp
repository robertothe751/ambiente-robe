#include "interfaccia_ui.h"
#include "api.h"
#include "wifi_conessione.h"

#include <lodepng.h>

// ============================================================================
// STRUTTURE E VARIABILI GLOBALI
// ============================================================================
struct PageDataUI {
  lv_obj_t *scr;
  lv_obj_t *lbl_name;
  lv_obj_t *lbl_reservations;
  lv_obj_t *lbl_participants;
  long event_id;
  String cached_name;
  int cached_reservations;
  int cached_participants;
};

static std::vector<PageDataUI> dynamic_pages;
static lv_image_dsc_t organization_logo_image_dsc = {};
static lv_obj_t *scr_logo =
    NULL; // Schermata fissa Logo inserita prima del menu
static lv_obj_t *scr_menu = NULL;
static lv_obj_t *global_sidebar = NULL;
static int total_screens_count = 1;

static int current_screen_idx = 0;
static bool is_animating = false;
static lv_timer_t *auto_rotate_timer = NULL;
static lv_obj_t *lbl_wifi_ssid = NULL;
static lv_obj_t *lbl_network_status = NULL;
static lv_obj_t *lbl_last_sync = NULL;
static lv_obj_t *lbl_firmware = NULL;

static lv_obj_t *qr_modal = NULL;
static lv_obj_t *status_modal = NULL;
static std::vector<lv_obj_t *> all_nav_buttons;
static bool rotation_enabled = true;

static volatile bool ui_reload_requested = false;

void ui_richiedi_ricarica_tileview() { ui_reload_requested = true; }

bool ui_consuma_richiesta_ricarica() {
  if (!ui_reload_requested)
    return false;
  ui_reload_requested = false;
  return true;
}

// ============================================================================
// GESTIONE EFFETTO LUMINOSO PULSANTI DI NAVIGAZIONE
// ============================================================================
static void update_nav_buttons_glow(bool running) {
  lv_color_t glow_color =
      running ? lv_color_hex(0x57E389) : lv_color_hex(0xD60429);

  for (lv_obj_t *btn : all_nav_buttons) {
    if (btn != NULL) {
      lv_obj_set_style_shadow_width(btn, 12, LV_PART_MAIN);
      lv_obj_set_style_shadow_spread(btn, 2, LV_PART_MAIN);
      lv_obj_set_style_shadow_color(btn, glow_color, LV_PART_MAIN);
      lv_obj_set_style_shadow_opa(btn, LV_OPA_80, LV_PART_MAIN);
    }
  }
}

// ============================================================================
// GESTIONE MESSAGE BOX E MODALI DI SISTEMA
// ============================================================================
static lv_obj_t *wifi_error_msgbox = NULL;

void close_wifi_error_msgbox(void) {
  if (wifi_error_msgbox != NULL) {
    lv_obj_del(wifi_error_msgbox);
    wifi_error_msgbox = NULL;

    if (auto_rotate_timer != NULL && rotation_enabled) {
      lv_timer_resume(auto_rotate_timer);
      lv_timer_reset(auto_rotate_timer);
    }
    update_nav_buttons_glow(rotation_enabled);
  }
}

static void wifi_error_close_cb(lv_event_t *e) {
  (void)e;
  close_wifi_error_msgbox();
}

void show_wifi_error_msgbox(void) {
  if (wifi_error_msgbox != NULL)
    return;

  if (auto_rotate_timer != NULL) {
    lv_timer_pause(auto_rotate_timer);
  }
  update_nav_buttons_glow(false);

  wifi_error_msgbox = lv_msgbox_create(lv_layer_top());
  lv_msgbox_add_title(wifi_error_msgbox, "Avviso di Rete");
  lv_msgbox_add_text(wifi_error_msgbox, "errore, perdita di connesione");

  lv_obj_t *close_btn =
      lv_msgbox_add_header_button(wifi_error_msgbox, LV_SYMBOL_CLOSE);
  if (close_btn != NULL) {
    lv_obj_add_event_cb(close_btn, wifi_error_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(close_btn, 34, 34);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xD60429), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_outline_width(close_btn, 0, 0);
  }

  lv_obj_set_size(wifi_error_msgbox, 330, 150);
  lv_obj_align(wifi_error_msgbox, LV_ALIGN_CENTER, 6, 0);
  lv_obj_set_style_bg_color(wifi_error_msgbox, lv_color_hex(0x111111), 0);
  lv_obj_set_style_text_color(wifi_error_msgbox, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_color(wifi_error_msgbox, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(wifi_error_msgbox, 1, 0);
  lv_obj_set_style_radius(wifi_error_msgbox, 16, 0);
  lv_obj_set_style_pad_all(wifi_error_msgbox, 12, 0);

  lv_obj_t *header = lv_msgbox_get_header(wifi_error_msgbox);
  if (header) {
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *title_label = lv_obj_get_child(header, 0);
    if (title_label) {
      lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    }
  }
}

static void close_qr_modal_cb(lv_event_t *e) {
  (void)e;
  if (qr_modal != NULL) {
    lv_obj_del(qr_modal);
    qr_modal = NULL;

    if (auto_rotate_timer != NULL && rotation_enabled) {
      lv_timer_resume(auto_rotate_timer);
      lv_timer_reset(auto_rotate_timer);
    }
    update_nav_buttons_glow(rotation_enabled);
  }
}

static void close_settings_qr_modal_cb(lv_event_t *e) {
  (void)e;
  ferma_wifi_ibrido();
  close_qr_modal_cb(e);
}

void show_qr_modal(const char *title_text, const char *qr_data,
                   lv_event_cb_t close_cb) {
  if (qr_modal != NULL)
    return;

  if (auto_rotate_timer != NULL) {
    lv_timer_pause(auto_rotate_timer);
  }
  update_nav_buttons_glow(false);

  qr_modal = lv_msgbox_create(lv_layer_top());
  lv_msgbox_add_title(qr_modal, title_text);

  lv_obj_t *close_btn = lv_msgbox_add_header_button(qr_modal, LV_SYMBOL_CLOSE);
  if (close_btn != NULL) {
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(close_btn, 34, 34);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xD60429), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_outline_width(close_btn, 0, 0);
  }

  lv_obj_set_size(qr_modal, 330, 345);
  lv_obj_align(qr_modal, LV_ALIGN_CENTER, 6, 0);
  lv_obj_set_style_bg_color(qr_modal, lv_color_hex(0x111111), 0);
  lv_obj_set_style_text_color(qr_modal, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_color(qr_modal, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(qr_modal, 1, 0);
  lv_obj_set_style_radius(qr_modal, 16, 0);
  lv_obj_set_style_pad_all(qr_modal, 12, 0);

  lv_obj_t *header = lv_msgbox_get_header(qr_modal);
  if (header) {
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *title_label = lv_obj_get_child(header, 0);
    if (title_label) {
      lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    }
  }

  lv_obj_t *content_area = lv_msgbox_get_content(qr_modal);
  if (content_area) {
    lv_obj_set_flex_flow(content_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_area, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *qrcode = lv_qrcode_create(content_area);
    lv_qrcode_set_size(qrcode, 200);
    lv_qrcode_set_dark_color(qrcode, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qrcode, lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qrcode, qr_data, strlen(qr_data));
  }
}

void show_qr_modal(const char *title_text, const char *qr_data) {
  show_qr_modal(title_text, qr_data, close_qr_modal_cb);
}

static void close_status_modal_cb(lv_event_t *e) {
  (void)e;
  if (status_modal != NULL) {
    lv_obj_del(status_modal);
    status_modal = NULL;
    lbl_wifi_ssid = NULL;
    lbl_network_status = NULL;
    lbl_last_sync = NULL;
    lbl_firmware = NULL;

    if (auto_rotate_timer != NULL && rotation_enabled) {
      lv_timer_resume(auto_rotate_timer);
      lv_timer_reset(auto_rotate_timer);
    }
    update_nav_buttons_glow(rotation_enabled);
  }
}

static void btn_show_status_modal_cb(lv_event_t *e) {
  (void)e;
  if (status_modal != NULL)
    return;

  if (auto_rotate_timer != NULL) {
    lv_timer_pause(auto_rotate_timer);
  }
  update_nav_buttons_glow(false);

  status_modal = lv_msgbox_create(lv_layer_top());
  lv_msgbox_add_title(status_modal, "Stato Sistema");

  lv_obj_t *close_btn =
      lv_msgbox_add_header_button(status_modal, LV_SYMBOL_CLOSE);
  if (close_btn != NULL) {
    lv_obj_add_event_cb(close_btn, close_status_modal_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_set_size(close_btn, 34, 34);
    lv_obj_set_style_radius(close_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xD60429), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_set_style_outline_width(close_btn, 0, 0);
  }

  lv_obj_set_size(status_modal, 330, 255);
  lv_obj_align(status_modal, LV_ALIGN_CENTER, 6, 0);
  lv_obj_set_style_bg_color(status_modal, lv_color_hex(0x111111), 0);
  lv_obj_set_style_text_color(status_modal, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_color(status_modal, lv_color_hex(0x444444), 0);
  lv_obj_set_style_border_width(status_modal, 1, 0);
  lv_obj_set_style_radius(status_modal, 16, 0);
  lv_obj_set_style_pad_all(status_modal, 12, 0);

  lv_obj_t *header = lv_msgbox_get_header(status_modal);
  if (header) {
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *title_label = lv_obj_get_child(header, 0);
    if (title_label) {
      lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    }
  }

  lv_obj_t *content_area = lv_msgbox_get_content(status_modal);
  if (content_area) {
    lv_obj_set_flex_flow(content_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_area, LV_FLEX_ALIGN_SPACE_AROUND,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(content_area, 8, 0);

    lbl_wifi_ssid = lv_label_create(content_area);
    lv_label_set_text(lbl_wifi_ssid, "Rete: ---");
    lv_obj_set_style_text_color(lbl_wifi_ssid, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_wifi_ssid, &lv_font_montserrat_14, 0);

    lbl_network_status = lv_label_create(content_area);
    lv_label_set_text(lbl_network_status, "Stato Rete: ---");
    lv_obj_set_style_text_color(lbl_network_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_network_status, &lv_font_montserrat_14, 0);

    lbl_last_sync = lv_label_create(content_area);
    lv_label_set_text(lbl_last_sync, "Ultima Sincronizzazione: ---");
    lv_obj_set_style_text_color(lbl_last_sync, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_last_sync, &lv_font_montserrat_14, 0);

    lbl_firmware = lv_label_create(content_area);
    lv_label_set_text(lbl_firmware, "Firmware: ---");
    lv_obj_set_style_text_color(lbl_firmware, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_firmware, &lv_font_montserrat_14, 0);
  }
}

// ============================================================================
// INTERAZIONE UTENTE E NAVIGAZIONE TRA SCHERMATE
// ============================================================================
static int tap_count = 0;
static uint32_t first_tap_tick = 0;

static void user_interaction_cb(lv_event_t *e) {
  (void)e;
  uint32_t now = lv_tick_get();

  if (tap_count == 0 || (now - first_tap_tick) > 1500) {
    tap_count = 1;
    first_tap_tick = now;
  } else {
    tap_count++;
    if (tap_count == 3) {
      rotation_enabled = !rotation_enabled;
      tap_count = 0;
      update_nav_buttons_glow(rotation_enabled);
    }
  }
}

static lv_obj_t *pending_screen = NULL;
static lv_scr_load_anim_t pending_anim = LV_SCR_LOAD_ANIM_NONE;

static void unlock_animation_cb(lv_timer_t *t) {
  (void)t;
  is_animating = false;
}

static void do_screen_change_cb(lv_timer_t *t) {
  (void)t;
  lv_obj_t *target = pending_screen;
  pending_screen = NULL;
  if (target == NULL) {
    is_animating = false;
    return;
  }

  int target_index = -1;
  if (target == scr_logo) {
    target_index = dynamic_pages.size();
  } else if (target == scr_menu) {
    target_index = dynamic_pages.size() + 1;
  } else {
    for (size_t i = 0; i < dynamic_pages.size(); ++i) {
      if (dynamic_pages[i].scr == target) {
        target_index = i;
        break;
      }
    }
  }

  if (target_index < 0) {
    is_animating = false;
    return;
  }

  lv_indev_reset(NULL, NULL);
  current_screen_idx = target_index;
  lv_screen_load_anim(target, pending_anim, 300, 0, false);

  if (auto_rotate_timer != NULL) {
    lv_timer_reset(auto_rotate_timer);
  }

  lv_timer_t *unlock = lv_timer_create(unlock_animation_cb, 350, NULL);
  lv_timer_set_repeat_count(unlock, 1);
}

static void load_screen_and_sync_index(lv_obj_t *target_screen,
                                       lv_scr_load_anim_t anim_type) {
  if (target_screen == NULL || is_animating)
    return;

  is_animating = true;
  pending_screen = target_screen;
  pending_anim = anim_type;

  lv_timer_t *timer_chg = lv_timer_create(do_screen_change_cb, 1, NULL);
  lv_timer_set_repeat_count(timer_chg, 1);
}

static lv_obj_t *get_screen_obj(int idx) {
  if (idx >= 0 && idx < (int)dynamic_pages.size()) {
    return dynamic_pages[idx].scr;
  } else if (idx == (int)dynamic_pages.size()) {
    return scr_logo;
  }
  return scr_menu;
}

static void auto_rotate_timer_cb(lv_timer_t *timer) {
  (void)timer;
  if (!rotation_enabled || is_animating)
    return;

  int next_screen_idx = (current_screen_idx + 1) % total_screens_count;
  load_screen_and_sync_index(get_screen_obj(next_screen_idx),
                             LV_SCR_LOAD_ANIM_OVER_TOP);
}

static void global_nav_up_cb(lv_event_t *e) {
  (void)e;
  if (is_animating)
    return;
  int prev_idx =
      (current_screen_idx - 1 + total_screens_count) % total_screens_count;
  load_screen_and_sync_index(get_screen_obj(prev_idx),
                             LV_SCR_LOAD_ANIM_OVER_BOTTOM);
}

static void global_nav_down_cb(lv_event_t *e) {
  (void)e;
  if (is_animating)
    return;
  int next_idx = (current_screen_idx + 1) % total_screens_count;
  load_screen_and_sync_index(get_screen_obj(next_idx),
                             LV_SCR_LOAD_ANIM_OVER_TOP);
}

static void create_global_fixed_sidebar(void) {
  global_sidebar = lv_obj_create(lv_layer_top());
  lv_obj_set_size(global_sidebar, 100, 466);
  lv_obj_align(global_sidebar, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_opa(global_sidebar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(global_sidebar, 0, 0);
  lv_obj_set_scrollbar_mode(global_sidebar, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *btn_up = lv_button_create(global_sidebar);
  lv_obj_set_size(btn_up, 55, 55);
  lv_obj_set_style_radius(btn_up, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x689CD9), LV_PART_MAIN);
  lv_obj_align(btn_up, LV_ALIGN_RIGHT_MID, -20, -50);

  lv_obj_t *lbl_up = lv_label_create(btn_up);
  lv_label_set_text(lbl_up, LV_SYMBOL_UP);
  lv_obj_set_style_text_color(lbl_up, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_center(lbl_up);
  lv_obj_add_event_cb(btn_up, global_nav_up_cb, LV_EVENT_CLICKED, NULL);
  all_nav_buttons.push_back(btn_up);

  lv_obj_t *btn_down = lv_button_create(global_sidebar);
  lv_obj_set_size(btn_down, 55, 55);
  lv_obj_set_style_radius(btn_down, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(btn_down, lv_color_hex(0x689CD9), LV_PART_MAIN);
  lv_obj_align(btn_down, LV_ALIGN_RIGHT_MID, -20, 50);

  lv_obj_t *lbl_down = lv_label_create(btn_down);
  lv_label_set_text(lbl_down, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(lbl_down, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_center(lbl_down);
  lv_obj_add_event_cb(btn_down, global_nav_down_cb, LV_EVENT_CLICKED, NULL);
  all_nav_buttons.push_back(btn_down);
}

// ============================================================================
// PROTOTIPO PAGINA EVENTO
// ============================================================================
int get_num_dynamic_pages(void) { return dynamic_pages.size(); }

long get_event_id_for_page(int page_idx) {
  if (page_idx >= 0 && page_idx < (int)dynamic_pages.size()) {
    return dynamic_pages[page_idx].event_id;
  }
  return 0;
}

static void create_prototype_event_ui(lv_obj_t *parent, int page_num,
                                      long ev_id, String cached_name,
                                      int cached_parts, int cached_participants,
                                      String qr_link) {
  lv_obj_t *tv = lv_tileview_create(parent);
  lv_obj_set_size(tv, 275, 320);
  lv_obj_align(tv, LV_ALIGN_CENTER, -20, 0);
  lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(tv, 0, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *container = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
  lv_obj_t *qr_tile = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(container, 1, LV_PART_MAIN);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(container, 12, 0);
  lv_obj_set_style_pad_top(container, 20, 0);
  lv_obj_set_style_pad_left(container, 20, 0);
  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

  lv_obj_set_style_bg_color(container, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
  lv_obj_set_style_margin_top(container, 6, 0);

  lv_obj_add_event_cb(container, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scrollbar_mode(qr_tile, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(qr_tile, 0, 0);
  lv_obj_set_style_border_width(qr_tile, 0, 0);
  lv_obj_set_style_bg_opa(qr_tile, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(qr_tile, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  if (qr_link.length() > 0) {
    lv_obj_set_style_bg_color(qr_tile, lv_color_hex(0x1E1E1E), 0);
    lv_obj_t *qrcode = lv_qrcode_create(qr_tile);
    lv_qrcode_set_size(qrcode, 200);
    lv_qrcode_set_dark_color(qrcode, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qrcode, lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qrcode, qr_link.c_str(), qr_link.length());
    lv_obj_center(qrcode);
  } else {
    lv_obj_set_style_bg_color(qr_tile, lv_color_hex(0x1E1E1E), 0);
    lv_obj_t *lbl_fallback = lv_label_create(qr_tile);
    lv_label_set_long_mode(lbl_fallback, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_fallback, 240);
    lv_label_set_text(lbl_fallback, "QR non disponibile");
    lv_obj_set_style_text_color(lbl_fallback, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_fallback, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(lbl_fallback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl_fallback);
  }

  // 1. ID Evento
  lv_obj_t *lbl_id = lv_label_create(container);
  lv_label_set_text_fmt(lbl_id, "ID : %ld", ev_id);
  lv_obj_set_style_text_color(lbl_id, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_id, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(lbl_id, LV_TEXT_ALIGN_LEFT, 0);

  // 2. Nome Evento
  lv_obj_t *lbl_name = lv_label_create(container);
  lv_label_set_long_mode(lbl_name, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl_name, 250);
  if (cached_name.length() > 0) {
    lv_label_set_text_fmt(lbl_name, "Nome : %s", cached_name.c_str());
  } else {
    lv_label_set_text_fmt(lbl_name, "Nome : ID %ld", ev_id);
  }
  lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_align(lbl_name, LV_TEXT_ALIGN_LEFT, 0);

  // 3. Reservations & Participants
  lv_obj_t *lbl_reservations = lv_label_create(container);
  lv_label_set_text_fmt(lbl_reservations, "Prenotazioni : %d", cached_parts);
  lv_obj_set_style_text_color(lbl_reservations, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_reservations, &lv_font_montserrat_16, 0);

  lv_obj_t *lbl_participants = lv_label_create(container);
  lv_label_set_text_fmt(lbl_participants, "Partecipanti : %d",
                        cached_participants);
  lv_obj_set_style_text_color(lbl_participants, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl_participants, &lv_font_montserrat_16, 0);

  dynamic_pages.push_back({parent, lbl_name, lbl_reservations, lbl_participants,
                           ev_id, cached_name, cached_parts,
                           cached_participants});
}

// ============================================================================
// SCHERMATA LOGO
// ============================================================================

static lv_img_dsc_t cached_logo_dsc = {0};
static bool logo_dsc_inizializzato = false;
static std::vector<uint16_t> rgb565_buffer;

static void create_logo_ui(lv_obj_t *parent) {
  lv_obj_add_event_cb(parent, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  lv_obj_t *container = lv_obj_create(parent);

  lv_obj_set_size(container, 225, 335);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);

  lv_obj_set_x(container, lv_obj_get_x(container) - 20);

  lv_obj_set_y(container, lv_obj_get_y(container) - 15);

  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);

  lv_obj_set_style_border_width(container, 0, 0);

  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);

  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

  lv_obj_set_scrollable(container, false);

  lv_obj_add_event_cb(container, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  if (!logo_dsc_inizializzato && LittleFS.exists("/logo.png")) {
    File f = LittleFS.open("/logo.png", "r");
    if (f) {
      size_t fsize = f.size();
      std::vector<uint8_t> file_bytes(fsize);
      if (f.read(file_bytes.data(), fsize) == fsize) {
        std::vector<unsigned char> rgba;
        unsigned width = 0;
        unsigned height = 0;

        unsigned err = lodepng::decode(rgba, width, height, file_bytes.data(),
                                       file_bytes.size());

        if (err == 0 && width > 0 && height > 0) {
          rgb565_buffer.resize(width * height);

          for (size_t i = 0; i < width * height; i++) {
            uint8_t r = rgba[i * 4 + 0];
            uint8_t g = rgba[i * 4 + 1];
            uint8_t b = rgba[i * 4 + 2];

            rgb565_buffer[i] =
                ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
          }

          cached_logo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
          cached_logo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
          cached_logo_dsc.header.w = width;
          cached_logo_dsc.header.h = height;
          cached_logo_dsc.header.stride = width * 2;
          cached_logo_dsc.data_size = rgb565_buffer.size() * sizeof(uint16_t);
          cached_logo_dsc.data = (const uint8_t *)rgb565_buffer.data();

          logo_dsc_inizializzato = true;
        }
      }
      f.close();
    }
  }

  if (logo_dsc_inizializzato) {
    lv_obj_t *image = lv_image_create(container);

    lv_image_set_src(image, &cached_logo_dsc);

    lv_obj_set_size(image, 225, 300);

    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);

    lv_obj_set_style_opa(image, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_center(image);

    lv_obj_add_event_cb(image, user_interaction_cb, LV_EVENT_PRESSED, NULL);
  } else {
    lv_obj_t *lbl = lv_label_create(container);

    lv_label_set_text(lbl, "LOGO NON DISPONIBILE");

    lv_obj_set_style_text_color(lbl, lv_color_hex(0xD60429), 0);

    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  }
}

// ============================================================================
// SCHERMATA MENU E DIAGNOSTICA
// ============================================================================
static void btn_show_wifi_share_cb(lv_event_t *e) {
  (void)e;
  if (WiFi.status() == WL_CONNECTED) {
    char qr_data[128];
    String ssid = WiFi.SSID();
    String pass = WiFi.psk();
    snprintf(qr_data, sizeof(qr_data), "WIFI:S:%s;T:WPA;P:%s;;", ssid.c_str(),
             pass.c_str());
    show_qr_modal("Condivisione Rete", qr_data);
  } else {
    show_qr_modal("Condivisione Rete", "WIFI:S:Disconnesso;T:NOPASS;;");
  }
}

static void btn_show_settings_qr_cb(lv_event_t *e) {
  (void)e;
  avvia_wifi_ibrido();
  show_qr_modal("QR Setting ESP32", "WIFI:S:ESP32-Config;T:WPA;P:12345678;;",
                close_settings_qr_modal_cb);
}

static void btn_disconnect_wifi_cb(lv_event_t *e) {
  (void)e;
  disconnetti_e_cancella_wifi();
}

static void restart_timer_cb(lv_timer_t *t) {
  (void)t;
  ESP.restart();
}

static void btn_restart_esp32_cb(lv_event_t *e) {
  (void)e;
  lv_timer_t *t = lv_timer_create(restart_timer_cb, 300, NULL);
  lv_timer_set_repeat_count(t, 1);
}

static void reset_esp_timer_cb(lv_timer_t *t) {
  (void)t;
  Preferences prefsWifi;
  prefsWifi.begin("wifi-config", false);
  prefsWifi.clear();
  prefsWifi.end();
  WiFi.disconnect(false, true);

  Preferences prefsCache;
  prefsCache.begin("api-cache", false);
  prefsCache.clear();
  prefsCache.end();

  ESP.restart();
}

static void btn_reset_esp32_cb(lv_event_t *e) {
  (void)e;
  lv_timer_t *t = lv_timer_create(reset_esp_timer_cb, 300, NULL);
  lv_timer_set_repeat_count(t, 1);
}

static void create_menu_diagnostica_ui(lv_obj_t *parent) {
  lv_obj_t *container = lv_obj_create(parent);
  lv_obj_set_size(container, 285, 355);
  lv_obj_align(container, LV_ALIGN_CENTER, -20, -15);
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(container, 0, 0);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(container, 9, 0);
  lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *title = lv_label_create(container);
  lv_label_set_text(title, "MENU");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_margin_top(title, 4, 0);

  lv_obj_t *btn_status_open = lv_button_create(container);
  lv_obj_set_size(btn_status_open, 230, 38);
  lv_obj_set_style_bg_color(btn_status_open, lv_color_hex(0x2C2C2C),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_status_open, lv_color_hex(0x444444),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_status_open, 1, LV_PART_MAIN);
  lv_obj_set_style_margin_top(btn_status_open, 6, 0);

  lv_obj_t *lbl_status_btn = lv_label_create(btn_status_open);
  lv_label_set_text(lbl_status_btn, "Stato Sistema");
  lv_obj_set_style_text_color(lbl_status_btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_status_btn, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_status_btn);
  lv_obj_add_event_cb(btn_status_open, btn_show_status_modal_cb,
                      LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_settings = lv_button_create(container);
  lv_obj_set_size(btn_settings, 230, 38);
  lv_obj_set_style_bg_color(btn_settings, lv_color_hex(0x2C2C2C), LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_settings, lv_color_hex(0x444444),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_settings, 1, LV_PART_MAIN);

  lv_obj_t *lbl_set = lv_label_create(btn_settings);
  lv_label_set_text(lbl_set, "QR Setting ESP32");
  lv_obj_set_style_text_color(lbl_set, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_set, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_set);
  lv_obj_add_event_cb(btn_settings, btn_show_settings_qr_cb, LV_EVENT_CLICKED,
                      NULL);

  lv_obj_t *btn_qr2 = lv_button_create(container);
  lv_obj_set_size(btn_qr2, 230, 38);
  lv_obj_set_style_bg_color(btn_qr2, lv_color_hex(0x2C2C2C), LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_qr2, lv_color_hex(0x444444), LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_qr2, 1, LV_PART_MAIN);

  lv_obj_t *lbl_q2 = lv_label_create(btn_qr2);
  lv_label_set_text(lbl_q2, "QR Condivisione Rete");
  lv_obj_set_style_text_color(lbl_q2, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_q2, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_q2);
  lv_obj_add_event_cb(btn_qr2, btn_show_wifi_share_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_reset_wifi = lv_button_create(container);
  lv_obj_set_size(btn_reset_wifi, 230, 38);
  lv_obj_set_style_bg_color(btn_reset_wifi, lv_color_hex(0x2C2C2C),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_reset_wifi, lv_color_hex(0xD60429),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_reset_wifi, 1, LV_PART_MAIN);

  lv_obj_t *lbl_btn_reset_wifi = lv_label_create(btn_reset_wifi);
  lv_label_set_text(lbl_btn_reset_wifi, "Reset Wi-Fi");
  lv_obj_set_style_text_color(lbl_btn_reset_wifi, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_btn_reset_wifi, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_btn_reset_wifi);
  lv_obj_add_event_cb(btn_reset_wifi, btn_disconnect_wifi_cb, LV_EVENT_CLICKED,
                      NULL);

  lv_obj_t *btn_riavvio_esp = lv_button_create(container);
  lv_obj_set_size(btn_riavvio_esp, 230, 38);
  lv_obj_set_style_bg_color(btn_riavvio_esp, lv_color_hex(0x2C2C2C),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_riavvio_esp, lv_color_hex(0xD60429),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_riavvio_esp, 1, LV_PART_MAIN);

  lv_obj_t *lbl_btn_riavvio = lv_label_create(btn_riavvio_esp);
  lv_label_set_text(lbl_btn_riavvio, "Riavvio ESP32");
  lv_obj_set_style_text_color(lbl_btn_riavvio, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_btn_riavvio, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_btn_riavvio);
  lv_obj_add_event_cb(btn_riavvio_esp, btn_restart_esp32_cb, LV_EVENT_CLICKED,
                      NULL);

  lv_obj_t *btn_reset_esp = lv_button_create(container);
  lv_obj_set_size(btn_reset_esp, 230, 38);
  lv_obj_set_style_bg_color(btn_reset_esp, lv_color_hex(0x2C2C2C),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(btn_reset_esp, lv_color_hex(0xD60429),
                                LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_reset_esp, 1, LV_PART_MAIN);

  lv_obj_t *lbl_btn_reset_esp = lv_label_create(btn_reset_esp);
  lv_label_set_text(lbl_btn_reset_esp, "Reset ESP32");
  lv_obj_set_style_text_color(lbl_btn_reset_esp, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_btn_reset_esp, &lv_font_montserrat_16, 0);
  lv_obj_center(lbl_btn_reset_esp);
  lv_obj_add_event_cb(btn_reset_esp, btn_reset_esp32_cb, LV_EVENT_CLICKED,
                      NULL);
}

// ============================================================================
// GESTIONE DATI E CREAZIONE TILEVIEW TRAMITE API
// ============================================================================
void ui_load_cached_data() {}

void ui_set_rotation_interval(int interval_ms) {
  if (interval_ms < 1000 || auto_rotate_timer == NULL)
    return;
  lv_timer_set_period(auto_rotate_timer, interval_ms);
  lv_timer_reset(auto_rotate_timer);
}

void ui_set_connection_status(int api_status, bool network_connected,
                              const String &saved_ssid) {
  (void)api_status;
  (void)network_connected;
  (void)saved_ssid;

  JsonDocument doc;
  get_system_status_doc(doc);

  bool connected = doc["network_connected"];
  String ssid = doc["ssid"].as<String>();
  String firmware = doc["firmware"].as<String>();
  unsigned long elapsed = doc["elapsed_seconds"];

  if (lbl_wifi_ssid != NULL) {
    if (connected) {
      lv_label_set_text_fmt(lbl_wifi_ssid, "Rete: %s",
                            ssid.length() > 0 ? ssid.c_str() : "Connesso");
    } else {
      lv_label_set_text(lbl_wifi_ssid, "Rete: Disconnesso");
    }
  }

  if (lbl_network_status != NULL) {
    lv_label_set_text_fmt(lbl_network_status, "Stato Rete: %s",
                          connected ? "Connessa" : "Non connessa");
  }

  if (lbl_last_sync != NULL) {
    if (connected) {
      lv_label_set_text(lbl_last_sync, "Ultima Sincronizzazione: ora");
    } else {
      char buf[64];
      if (elapsed < 60) {
        snprintf(buf, sizeof(buf), "Ultima Sincronizzazione: %lu s fa",
                 elapsed);
      } else if (elapsed < 3600) {
        unsigned long mins = elapsed / 60;
        snprintf(buf, sizeof(buf), "Ultima Sincronizzazione: %lu min fa", mins);
      } else {
        unsigned long hours = elapsed / 3600;
        unsigned long mins = (elapsed % 3600) / 60;
        snprintf(buf, sizeof(buf), "Ultima Sincronizzazione: %lu h %lu min fa",
                 hours, mins);
      }
      lv_label_set_text(lbl_last_sync, buf);
    }
  }

  if (lbl_firmware != NULL) {
    lv_label_set_text_fmt(lbl_firmware, "Firmware: %s", firmware.c_str());
  }
}

void ui_set_wifi_setup_state(bool provisioning, const String &saved_ssid) {
  (void)provisioning;
  (void)saved_ssid;
}

void create_tileview(void) {
  pending_screen = NULL;
  is_animating = false;
  lv_obj_t *reload_screen = lv_obj_create(NULL);
  lv_screen_load(reload_screen);

  if (auto_rotate_timer != NULL) {
    lv_timer_delete(auto_rotate_timer);
    auto_rotate_timer = NULL;
  }
  if (qr_modal != NULL) {
    lv_obj_del(qr_modal);
    qr_modal = NULL;
  }
  if (status_modal != NULL) {
    lv_obj_del(status_modal);
    status_modal = NULL;
    lbl_wifi_ssid = NULL;
    lbl_network_status = NULL;
    lbl_last_sync = NULL;
    lbl_firmware = NULL;
  }
  if (wifi_error_msgbox != NULL) {
    lv_obj_del(wifi_error_msgbox);
    wifi_error_msgbox = NULL;
  }
  for (PageDataUI &page : dynamic_pages) {
    if (page.scr != NULL) {
      lv_obj_del(page.scr);
    }
  }
  if (scr_logo != NULL) {
    lv_obj_del(scr_logo);
    scr_logo = NULL;
  }
  if (scr_menu != NULL) {
    lv_obj_del(scr_menu);
    scr_menu = NULL;
  }
  if (global_sidebar != NULL) {
    lv_obj_del(global_sidebar);
    global_sidebar = NULL;
  }

  all_nav_buttons.clear();
  dynamic_pages.clear();

  std::vector<ApiEvent> active_events = api_get_cached_events();

  // Fallback se la lista eventi è vuota
  if (active_events.empty()) {
    active_events.push_back({0, "In attesa di eventi Cloud...", "", 0, 0});
  }

  total_screens_count = active_events.size() + 2; // n eventi + 1 logo + 1 menu

  // Creazione dinamica delle schermate per ogni evento attivo
  for (size_t i = 0; i < active_events.size(); i++) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollable(scr, false);
    lv_obj_add_event_cb(scr, user_interaction_cb, LV_EVENT_PRESSED, NULL);

    create_prototype_event_ui(
        scr, i + 1, active_events[i].id, active_events[i].name,
        active_events[i].reservations, active_events[i].participants,
        active_events[i].qr);
  }

  // Creazione della schermata fissa del Logo
  scr_logo = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_logo, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(scr_logo, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scrollable(scr_logo, false);
  lv_obj_add_event_cb(scr_logo, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  create_logo_ui(scr_logo);

  // Creazione della schermata fissa del Menu
  scr_menu = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_menu, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(scr_menu, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scrollable(scr_menu, false);
  lv_obj_add_event_cb(scr_menu, user_interaction_cb, LV_EVENT_PRESSED, NULL);

  create_menu_diagnostica_ui(scr_menu);

  create_global_fixed_sidebar();
  update_nav_buttons_glow(rotation_enabled);

  current_screen_idx = 0;
  is_animating = false;
  if (!dynamic_pages.empty()) {
    lv_screen_load(dynamic_pages[0].scr);
  } else {
    lv_screen_load(scr_logo);
  }
  lv_obj_del(reload_screen);

  int rot_speed = api_get_rotation_interval();

  auto_rotate_timer = lv_timer_create(auto_rotate_timer_cb, rot_speed, NULL);
}