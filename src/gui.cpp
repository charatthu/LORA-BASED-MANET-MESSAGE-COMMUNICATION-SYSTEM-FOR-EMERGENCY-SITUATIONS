/*
 * gui.cpp  –  MANET Chat GUI for 2.8" 240×320 TFT (LVGL 8.x)
 * ออกแบบให้ตรงกับ web UI ใน webchat.cpp ทุกหน้า:
 *   SCR_SPLASH   → โลโก้ + ปุ่ม Login / Register
 *   SCR_LOGIN    → ช่องกรอก username/password + ปุ่ม Login
 *   SCR_REGISTER → ช่องกรอก username/password + ปุ่ม Register
 *   SCR_HOME     → bottom-tab: Friends | Groups | Invites | Settings
 *   SCR_ROOM     → header + chat bubbles + input bar
 *   SCR_ROOM_INFO→ รายชื่อสมาชิก / join requests (overlay)
 */

#include "gui.h"
#include "globals.h"
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Arduino.h>
#include <Preferences.h>  // สำหรับ save touch calibration

// ─────────────────────────────────────────────
//  Display driver
// ─────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[240 * 20];

static void my_disp_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.setSwapBytes(true);
    tft.pushImage(area->x1, area->y1, w, h, (uint16_t*)&px->full);
    lv_disp_flush_ready(drv);
}

// ── SPI pin ของ LoRa (ต้องตรงกับ main.cpp)
#define LORA_CS_PIN  27

// ── Touch state ──
static bool     g_touched = false;
static uint16_t g_touch_x = 0;
static uint16_t g_touch_y = 0;

static void my_touch_read(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    digitalWrite(LORA_CS_PIN, HIGH);

    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
        // ไม่ mirror — ใช้ค่าตรงๆ จาก calibration
        int x = (int)tx;
        int y = (int)ty;

        if (x < 0)   x = 0;
        if (x > 239) x = 239;
        if (y < 0)   y = 0;
        if (y > 319) y = 319;

        data->state   = LV_INDEV_STATE_PR;
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        Serial.printf("TOUCH x=%d y=%d\n", x, y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// stub — ไม่ต้องใช้แล้ว แต่เก็บไว้ให้ main.cpp เรียกได้
void gui_poll_touch() {}

// ─────────────────────────────────────────────
//  Colour palette (เหมือนเว็บ)
// ─────────────────────────────────────────────
#define C_BG       lv_color_hex(0xF4F1EC)   // พื้นหลังทั่วไป (ครีม)
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_GREEN    lv_color_hex(0x7BC96F)   // ปุ่มหลัก / bubble ของเรา
#define C_GREEN2   lv_color_hex(0x9AD97C)   // bubble สี
#define C_BLUE     lv_color_hex(0x1F3A68)   // ปุ่ม Login / header
#define C_DARK     lv_color_hex(0x1F3B1A)   // text บน bubble เรา
#define C_HEADER   lv_color_hex(0xFFFFFF)   // header bar
#define C_BORDER   lv_color_hex(0xE3DED6)
#define C_SUB      lv_color_hex(0x888888)
#define C_RED      lv_color_hex(0xD93025)
#define C_ORANGE   lv_color_hex(0xB07000)
#define C_GRAY_BTN lv_color_hex(0xEEEEEE)

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
static ScreenID  g_current_screen = SCR_SPLASH;
static char      g_current_room[48] = {0};
static uint32_t  g_my_uid = 0;
static char      g_my_name[32] = "Me";
static char      g_toast_msg[64] = {0};

// ─────────────────────────────────────────────
//  Screen root objects
// ─────────────────────────────────────────────
static lv_obj_t* scr_splash  = nullptr;
static lv_obj_t* scr_login   = nullptr;
static lv_obj_t* scr_reg     = nullptr;
static lv_obj_t* scr_home    = nullptr;
static lv_obj_t* scr_room    = nullptr;

// Home sub-panels
static lv_obj_t* panel_friends  = nullptr;
static lv_obj_t* panel_groups   = nullptr;
static lv_obj_t* panel_invites  = nullptr;
static lv_obj_t* panel_settings = nullptr;
static lv_obj_t* nav_btns[4]    = {};
static int       home_tab       = 0;

// Home lists
static lv_obj_t* list_users   = nullptr;
static lv_obj_t* list_rooms   = nullptr;
static lv_obj_t* list_invites = nullptr;

// Room screen
static lv_obj_t* room_title     = nullptr;
static lv_obj_t* room_chat_cont = nullptr;
static lv_obj_t* room_ta        = nullptr;
static lv_obj_t* room_kb        = nullptr;
static lv_obj_t* room_info_panel= nullptr; // slide-over

// Toast
static lv_obj_t* toast_overlay  = nullptr;
static lv_obj_t* toast_label    = nullptr;
static lv_timer_t* toast_timer  = nullptr;

// Profile label on settings tab
static lv_obj_t* lbl_my_name    = nullptr;
static lv_obj_t* lbl_my_uid     = nullptr;  // ✅ FIX: เพิ่ม pointer เพื่ออัปเดต UID label ได้

// ─────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────
static void build_splash();
static void build_login();
static void build_register();
static void build_home();
static void build_room();
static void build_toast_overlay();
static void home_show_tab(int tab);

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────
static lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                           lv_color_t bg, lv_color_t tc,
                           lv_coord_t w, lv_coord_t h) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, tc, 0);
    lv_obj_center(lbl);
    return btn;
}

static lv_obj_t* make_input(lv_obj_t* parent, const char* placeholder, bool is_pwd,
                              lv_coord_t w) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, 40);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, is_pwd);
    lv_obj_set_style_bg_color(ta, C_WHITE, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0x111111), 0);       // text สีดำ
    lv_obj_set_style_text_color(ta, lv_color_hex(0x999999), 0x00050000);
    lv_obj_set_style_radius(ta, 8, 0);
    lv_obj_set_style_border_color(ta, C_BORDER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_pad_left(ta, 10, 0);
    return ta;
}

// dismiss keyboard helper
static void dismiss_kb(lv_obj_t* kb) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

// ─────────────────────────────────────────────
//  Toast
// ─────────────────────────────────────────────
static void toast_timer_cb(lv_timer_t* t) {
    if (toast_overlay) lv_obj_add_flag(toast_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    toast_timer = nullptr;
}

void gui_show_toast(const char* msg) {
    if (!toast_overlay) return;
    lv_label_set_text(toast_label, msg);
    lv_obj_clear_flag(toast_overlay, LV_OBJ_FLAG_HIDDEN);
    if (toast_timer) lv_timer_del(toast_timer);
    toast_timer = lv_timer_create(toast_timer_cb, 2500, nullptr);
    toast_timer->repeat_count = 1;
}

void gui_show_error(const char* msg) {
    gui_show_toast(msg);
}

static void build_toast_overlay() {
    // สร้าง toast ไว้บน scr_home และ scr_room ใช้ร่วมกันไม่ได้ง่าย
    // ใช้ layer บน active screen แทน – สร้างใหม่ทุกครั้งที่ switch screen
    // ดังนั้น build ครั้งเดียวบน lv_layer_top()
    toast_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(toast_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toast_overlay, 0, 0);
    lv_obj_clear_flag(toast_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* box = lv_obj_create(toast_overlay);
    lv_obj_set_size(box, 200, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_border_width(box, 0, 0);

    toast_label = lv_label_create(box);
    lv_obj_set_style_text_color(toast_label, C_WHITE, 0);
    lv_label_set_long_mode(toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(toast_label, 180);
    lv_label_set_text(toast_label, "");
    lv_obj_center(toast_label);
}

// ─────────────────────────────────────────────
//  SPLASH SCREEN
// ─────────────────────────────────────────────
static void splash_login_cb(lv_event_t* e) {
    Serial.println("LOGIN BTN CLICKED!");
    gui_show_screen(SCR_LOGIN);
}
static void splash_reg_cb(lv_event_t* e) {
    Serial.println("REGISTER BTN CLICKED!");
    gui_show_screen(SCR_REGISTER);
}

static void build_splash() {
    scr_splash = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_splash, C_WHITE, 0);

    // Title
    lv_obj_t* title = lv_label_create(scr_splash);
    lv_label_set_text(title, "LoRa-based MANET\nMessage System");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title, 200);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(title, C_BLUE, 0);
    // ใช้ฟอนต์ใหญ่ขึ้นถ้ามี – ใช้ montserrat_20 แทน
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    // subtitle
    lv_obj_t* sub = lv_label_create(scr_splash);
    lv_label_set_text(sub, "NODE MESH NETWORK");
    lv_obj_set_style_text_color(sub, C_SUB, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -10);

    // Login button
    lv_obj_t* btn_login = make_btn(scr_splash, "Login", C_BLUE, C_WHITE, 200, 44);
    lv_obj_align(btn_login, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_event_cb(btn_login, splash_login_cb, LV_EVENT_CLICKED, nullptr);

    // Register button
    lv_obj_t* btn_reg = make_btn(scr_splash, "Register", lv_color_hex(0x2E7D32), C_WHITE, 200, 44);
    lv_obj_align(btn_reg, LV_ALIGN_CENTER, 0, 96);
    lv_obj_add_event_cb(btn_reg, splash_reg_cb, LV_EVENT_CLICKED, nullptr);
}

// ─────────────────────────────────────────────
//  LOGIN SCREEN
// ─────────────────────────────────────────────
static lv_obj_t* ta_login_user = nullptr;
static lv_obj_t* ta_login_pass = nullptr;
static lv_obj_t* kb_login      = nullptr;

static void login_kb_ready_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        dismiss_kb(kb_login);
}
static void login_ta_focus_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(kb_login, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb_login, ta);
    }
}
static void login_submit_cb(lv_event_t* e) {
    const char* u = lv_textarea_get_text(ta_login_user);
    const char* p = lv_textarea_get_text(ta_login_pass);
    if (strlen(u) == 0 || strlen(p) == 0) {
        gui_show_toast("Please fill all fields");
        return;
    }
    dismiss_kb(kb_login);
    on_gui_login(u, p);
}
static void login_back_cb(lv_event_t* e) {
    gui_show_screen(SCR_SPLASH);
}

static void build_login() {
    scr_login = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_login, C_WHITE, 0);

    // Header bar
    lv_obj_t* hdr = lv_obj_create(scr_login);
    lv_obj_set_size(hdr, 240, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_BLUE, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);

    lv_obj_t* back = lv_label_create(hdr);
    lv_label_set_text(back, "<");
    lv_obj_set_style_text_color(back, C_WHITE, 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, login_back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* hdr_lbl = lv_label_create(hdr);
    lv_label_set_text(hdr_lbl, "Login");
    lv_obj_set_style_text_color(hdr_lbl, C_WHITE, 0);
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_unscii_8, 0);
    lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

    // Form
    lv_obj_t* lbl_u = lv_label_create(scr_login);
    lv_label_set_text(lbl_u, "Username");
    lv_obj_align(lbl_u, LV_ALIGN_TOP_LEFT, 20, 60);

    ta_login_user = make_input(scr_login, "Enter username", false, 200);
    lv_obj_align(ta_login_user, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_add_event_cb(ta_login_user, login_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* lbl_p = lv_label_create(scr_login);
    lv_label_set_text(lbl_p, "Password");
    lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, 20, 126);

    ta_login_pass = make_input(scr_login, "Enter password", true, 200);
    lv_obj_align(ta_login_pass, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_add_event_cb(ta_login_pass, login_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* btn = make_btn(scr_login, "Login", C_BLUE, C_WHITE, 200, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_add_event_cb(btn, login_submit_cb, LV_EVENT_CLICKED, nullptr);

    // Keyboard (ซ่อนไว้ก่อน, ขนาดกะทัดรัด)
    kb_login = lv_keyboard_create(scr_login);
    lv_obj_set_size(kb_login, 240, 130);
    lv_obj_align(kb_login, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb_login, login_kb_ready_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(kb_login, LV_OBJ_FLAG_HIDDEN);
}

// ─────────────────────────────────────────────
//  REGISTER SCREEN
// ─────────────────────────────────────────────
static lv_obj_t* ta_reg_user = nullptr;
static lv_obj_t* ta_reg_pass = nullptr;
static lv_obj_t* kb_reg      = nullptr;

static void reg_kb_ready_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        dismiss_kb(kb_reg);
}
static void reg_ta_focus_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(kb_reg, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb_reg, ta);
    }
}
static void reg_submit_cb(lv_event_t* e) {
    const char* u = lv_textarea_get_text(ta_reg_user);
    const char* p = lv_textarea_get_text(ta_reg_pass);
    if (strlen(u) == 0 || strlen(p) == 0) {
        gui_show_toast("Please fill all fields");
        return;
    }
    dismiss_kb(kb_reg);
    on_gui_register(u, p);
}
static void reg_back_cb(lv_event_t* e) {
    gui_show_screen(SCR_SPLASH);
}

static void build_register() {
    scr_reg = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_reg, C_WHITE, 0);

    lv_obj_t* hdr = lv_obj_create(scr_reg);
    lv_obj_set_size(hdr, 240, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);

    lv_obj_t* back = lv_label_create(hdr);
    lv_label_set_text(back, "<");
    lv_obj_set_style_text_color(back, C_WHITE, 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, reg_back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* hdr_lbl = lv_label_create(hdr);
    lv_label_set_text(hdr_lbl, "Register");
    lv_obj_set_style_text_color(hdr_lbl, C_WHITE, 0);
    lv_obj_set_style_text_font(hdr_lbl, &lv_font_unscii_8, 0);
    lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* lbl_u = lv_label_create(scr_reg);
    lv_label_set_text(lbl_u, "Username (English only)");
    lv_obj_align(lbl_u, LV_ALIGN_TOP_LEFT, 20, 60);

    ta_reg_user = make_input(scr_reg, "Enter username", false, 200);
    lv_obj_align(ta_reg_user, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_add_event_cb(ta_reg_user, reg_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* lbl_p = lv_label_create(scr_reg);
    lv_label_set_text(lbl_p, "Password");
    lv_obj_align(lbl_p, LV_ALIGN_TOP_LEFT, 20, 126);

    ta_reg_pass = make_input(scr_reg, "Enter password", true, 200);
    lv_obj_align(ta_reg_pass, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_add_event_cb(ta_reg_pass, reg_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* btn = make_btn(scr_reg, "Register", lv_color_hex(0x2E7D32), C_WHITE, 200, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_add_event_cb(btn, reg_submit_cb, LV_EVENT_CLICKED, nullptr);

    kb_reg = lv_keyboard_create(scr_reg);
    lv_obj_set_size(kb_reg, 240, 130);
    lv_obj_align(kb_reg, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb_reg, reg_kb_ready_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(kb_reg, LV_OBJ_FLAG_HIDDEN);
}

// ─────────────────────────────────────────────
//  HOME SCREEN  (4 tabs via bottom nav)
// ─────────────────────────────────────────────

// ── Tab icons (text-based fallback ──
static const char* TAB_ICONS[]  = { "Fr",  "Gr",
                                     "Inv",  "Set" };
static const char* TAB_LABELS[] = { "Friends", "Groups", "Invites", "Settings" };

static void nav_tab_cb(lv_event_t* e) {
    int tab = (int)(uintptr_t)lv_event_get_user_data(e);
    home_show_tab(tab);
}

static void home_show_tab(int tab) {
    home_tab = tab;
    lv_obj_t* panels[] = { panel_friends, panel_groups, panel_invites, panel_settings };
    for (int i = 0; i < 4; i++) {
        if (i == tab) lv_obj_clear_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(panels[i],   LV_OBJ_FLAG_HIDDEN);

        // nav button highlight
        lv_obj_set_style_text_color(nav_btns[i],
            i == tab ? C_GREEN : C_SUB, 0);
    }
}

// Friends tab – open DM callback
static void friend_click_cb(lv_event_t* e) {
    uint32_t uid = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    Serial.printf("DM click uid=%lu myUid=%lu\n", (unsigned long)uid, (unsigned long)g_my_uid);
    if (uid == 0) { return; }
    on_gui_open_dm(uid);
}

// Groups tab callbacks
static lv_obj_t* ta_create_room = nullptr;
static lv_obj_t* kb_home        = nullptr;

static void create_room_ta_focus_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(kb_home, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb_home, ta_create_room);
    }
}
static void create_room_kb_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
        lv_obj_add_flag(kb_home, LV_OBJ_FLAG_HIDDEN);
}
static void create_room_btn_cb(lv_event_t* e) {
    if (!ta_create_room) return;
    const char* n = lv_textarea_get_text(ta_create_room);
    if (strlen(n) == 0) { gui_show_toast("Enter group name"); return; }
    lv_obj_add_flag(kb_home, LV_OBJ_FLAG_HIDDEN);
    on_gui_create_room(n);
    lv_textarea_set_text(ta_create_room, "");
}

static void join_room_cb(lv_event_t* e) {
    char* room = (char*)lv_event_get_user_data(e);
    on_gui_join_room(room);
}
static void open_room_cb(lv_event_t* e) {
    char* room = (char*)lv_event_get_user_data(e);
    on_gui_open_room(room);
}

// Settings callbacks
// ── Recalibrate touch ──
static void settings_recal_cb(lv_event_t* e) {
    // ล้างค่าเก่า แล้ว restart เพื่อเข้า calibration
    Preferences prefs;
    prefs.begin("touch_cal", false);
    prefs.putBool("saved", false);
    prefs.end();
    // แจ้งผู้ใช้ก่อน restart
    gui_show_toast("Restarting for calibration...");
    delay(1500);
    ESP.restart();
}

static void settings_logout_cb(lv_event_t* e)  { on_gui_logout(); }
static void settings_delete_cb(lv_event_t* e)  { on_gui_delete_self(); }
static void settings_rename_cb(lv_event_t* e)  {
    gui_show_toast("Rename: use web UI");
}

static void build_home() {
    scr_home = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_home, C_BG, 0);
    lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header ──
    lv_obj_t* hdr = lv_obj_create(scr_home);
    lv_obj_set_size(hdr, 240, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_WHITE, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_shadow_width(hdr, 4, 0);
    lv_obj_set_style_shadow_ofs_y(hdr, 2, 0);
    lv_obj_set_style_shadow_color(hdr, C_BORDER, 0);

    lv_obj_t* htx = lv_label_create(hdr);
    lv_label_set_text(htx, "MANET Comm System");
    lv_obj_set_style_text_font(htx, &lv_font_unscii_8, 0);
    lv_obj_align(htx, LV_ALIGN_LEFT_MID, 10, -4);

    lv_obj_t* hsx = lv_label_create(hdr);
    lv_label_set_text(hsx, "LoRa Mesh Network");
    lv_obj_set_style_text_color(hsx, C_SUB, 0);
    lv_obj_align(hsx, LV_ALIGN_LEFT_MID, 10, 8);

    // ── Bottom nav (height=54) ──
    lv_obj_t* nav = lv_obj_create(scr_home);
    lv_obj_set_size(nav, 240, 54);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, C_WHITE, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_color(nav, C_BORDER, 0);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* nb = lv_btn_create(nav);
        lv_obj_set_size(nb, 60, 54);
        lv_obj_set_style_bg_color(nb, C_WHITE, 0);
        lv_obj_set_style_bg_color(nb, lv_color_hex(0xF0F0F0), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(nb, 0, 0);
        lv_obj_set_style_radius(nb, 0, 0);
        lv_obj_set_style_shadow_width(nb, 0, 0);
        lv_obj_set_flex_flow(nb, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(nb, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* ico = lv_label_create(nb);
        lv_label_set_text(ico, TAB_ICONS[i]);
        lv_obj_set_style_text_color(ico, i == 0 ? C_GREEN : C_SUB, 0);

        lv_obj_t* txt = lv_label_create(nb);
        lv_label_set_text(txt, TAB_LABELS[i]);
        lv_obj_set_style_text_font(txt, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(txt, i == 0 ? C_GREEN : C_SUB, 0);

        nav_btns[i] = ico; // เก็บ icon label สำหรับ color switch
        lv_obj_add_event_cb(nb, nav_tab_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);
    }

    // ── Panel area  (44 top .. 266 = 320-54) = 222px tall ──
    lv_coord_t pY = 44, pH = 320 - 44 - 54;

    // ── PANEL 0: Friends ──
    panel_friends = lv_obj_create(scr_home);
    lv_obj_set_size(panel_friends, 240, pH);
    lv_obj_align(panel_friends, LV_ALIGN_TOP_MID, 0, pY);
    lv_obj_set_style_bg_color(panel_friends, C_BG, 0);
    lv_obj_set_style_border_width(panel_friends, 0, 0);
    lv_obj_set_style_pad_all(panel_friends, 0, 0);
    lv_obj_set_flex_flow(panel_friends, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* lbl_f = lv_label_create(panel_friends);
    lv_label_set_text(lbl_f, "FRIENDS");
    lv_obj_set_style_text_color(lbl_f, C_SUB, 0);
    lv_obj_set_style_text_font(lbl_f, &lv_font_unscii_8, 0);
    lv_obj_set_style_pad_left(lbl_f, 12, 0);
    lv_obj_set_style_pad_top(lbl_f, 8, 0);

    list_users = lv_obj_create(panel_friends);
    lv_obj_set_flex_flow(list_users, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_users, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_size(list_users, 240, pH - 24);
    lv_obj_set_style_bg_color(list_users, C_BG, 0);
    lv_obj_set_style_border_width(list_users, 0, 0);
    lv_obj_set_style_pad_all(list_users, 0, 0);

    // ── PANEL 1: Groups ──
    panel_groups = lv_obj_create(scr_home);
    lv_obj_set_size(panel_groups, 240, pH);
    lv_obj_align(panel_groups, LV_ALIGN_TOP_MID, 0, pY);
    lv_obj_set_style_bg_color(panel_groups, C_BG, 0);
    lv_obj_set_style_border_width(panel_groups, 0, 0);
    lv_obj_set_style_pad_all(panel_groups, 0, 0);
    lv_obj_add_flag(panel_groups, LV_OBJ_FLAG_HIDDEN);

    // Create room bar
    lv_obj_t* cb = lv_obj_create(panel_groups);
    lv_obj_set_size(cb, 236, 44);
    lv_obj_set_style_bg_color(cb, C_WHITE, 0);
    lv_obj_set_style_border_color(cb, C_BORDER, 0);
    lv_obj_set_style_border_width(cb, 1, 0);
    lv_obj_set_style_radius(cb, 10, 0);
    lv_obj_set_style_pad_all(cb, 4, 0);
    lv_obj_set_flex_flow(cb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(cb, LV_ALIGN_TOP_MID, 0, 8);

    ta_create_room = make_input(cb, "Group name...", false, 148);
    lv_obj_add_event_cb(ta_create_room, create_room_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* btn_cr = make_btn(cb, "Create", C_GREEN, C_WHITE, 70, 36);
    lv_obj_add_event_cb(btn_cr, create_room_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl_g = lv_label_create(panel_groups);
    lv_label_set_text(lbl_g, "ALL GROUPS");
    lv_obj_set_style_text_color(lbl_g, C_SUB, 0);
    lv_obj_set_style_text_font(lbl_g, &lv_font_unscii_8, 0);
    lv_obj_set_style_pad_left(lbl_g, 12, 0);
    lv_obj_align(lbl_g, LV_ALIGN_TOP_MID, -56, 60);

    list_rooms = lv_obj_create(panel_groups);
    lv_obj_set_flex_flow(list_rooms, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_rooms, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_size(list_rooms, 240, pH - 76);
    lv_obj_align(list_rooms, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(list_rooms, C_BG, 0);
    lv_obj_set_style_border_width(list_rooms, 0, 0);
    lv_obj_set_style_pad_all(list_rooms, 0, 0);

    // Keyboard (shared for home screen)
    kb_home = lv_keyboard_create(scr_home);
    lv_obj_set_size(kb_home, 240, 130);
    lv_obj_align(kb_home, LV_ALIGN_BOTTOM_MID, 0, -54); // above nav
    lv_obj_add_event_cb(kb_home, create_room_kb_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(kb_home, LV_OBJ_FLAG_HIDDEN);

    // ── PANEL 2: Invites ──
    panel_invites = lv_obj_create(scr_home);
    lv_obj_set_size(panel_invites, 240, pH);
    lv_obj_align(panel_invites, LV_ALIGN_TOP_MID, 0, pY);
    lv_obj_set_style_bg_color(panel_invites, C_BG, 0);
    lv_obj_set_style_border_width(panel_invites, 0, 0);
    lv_obj_set_style_pad_all(panel_invites, 0, 0);
    lv_obj_add_flag(panel_invites, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* lbl_i = lv_label_create(panel_invites);
    lv_label_set_text(lbl_i, "INVITATIONS");
    lv_obj_set_style_text_color(lbl_i, C_SUB, 0);
    lv_obj_set_style_text_font(lbl_i, &lv_font_unscii_8, 0);
    lv_obj_set_style_pad_left(lbl_i, 12, 0);
    lv_obj_set_style_pad_top(lbl_i, 8, 0);

    list_invites = lv_obj_create(panel_invites);
    lv_obj_set_flex_flow(list_invites, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_invites, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_size(list_invites, 240, pH - 24);
    lv_obj_set_style_bg_color(list_invites, C_BG, 0);
    lv_obj_set_style_border_width(list_invites, 0, 0);
    lv_obj_set_style_pad_all(list_invites, 0, 0);

    // ── PANEL 3: Settings ──
    panel_settings = lv_obj_create(scr_home);
    lv_obj_set_size(panel_settings, 240, pH);
    lv_obj_align(panel_settings, LV_ALIGN_TOP_MID, 0, pY);
    lv_obj_set_style_bg_color(panel_settings, C_BG, 0);
    lv_obj_set_style_border_width(panel_settings, 0, 0);
    lv_obj_set_style_pad_all(panel_settings, 0, 0);
    lv_obj_add_flag(panel_settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(panel_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(panel_settings, 10, 0);
    lv_obj_set_style_pad_hor(panel_settings, 10, 0);
    lv_obj_set_style_pad_row(panel_settings, 8, 0);

    // Profile card
    lv_obj_t* prof = lv_obj_create(panel_settings);
    lv_obj_set_size(prof, 220, 54);
    lv_obj_set_style_bg_color(prof, C_WHITE, 0);
    lv_obj_set_style_radius(prof, 12, 0);
    lv_obj_set_style_border_width(prof, 1, 0);
    lv_obj_set_style_border_color(prof, C_BORDER, 0);
    lv_obj_set_style_pad_all(prof, 10, 0);
    lv_obj_set_flex_flow(prof, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(prof, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Avatar circle
    lv_obj_t* av = lv_obj_create(prof);
    lv_obj_set_size(av, 36, 36);
    lv_obj_set_style_bg_color(av, C_GREEN, 0);
    lv_obj_set_style_radius(av, 18, 0);
    lv_obj_set_style_border_width(av, 0, 0);

    lv_obj_t* av_lbl = lv_label_create(av);
    lv_label_set_text(av_lbl, "M");
    lv_obj_set_style_text_color(av_lbl, C_WHITE, 0);
    lv_obj_center(av_lbl);

    lv_obj_t* info = lv_obj_create(prof);
    lv_obj_set_size(info, 140, 36);
    lv_obj_set_style_bg_opa(info, 0, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);

    lbl_my_name = lv_label_create(info);
    lv_label_set_text(lbl_my_name, g_my_name);
    lv_obj_set_style_text_font(lbl_my_name, &lv_font_unscii_8, 0);
    lv_obj_align(lbl_my_name, LV_ALIGN_TOP_LEFT, 6, 0);

    lv_obj_t* uid_lbl = lv_label_create(info);
    char uid_str[24]; snprintf(uid_str, sizeof(uid_str), "UID: %lu", (unsigned long)g_my_uid);
    lv_label_set_text(uid_lbl, uid_str);
    lv_obj_set_style_text_color(uid_lbl, C_SUB, 0);
    lv_obj_set_style_text_font(uid_lbl, &lv_font_unscii_8, 0);
    lv_obj_align(uid_lbl, LV_ALIGN_BOTTOM_LEFT, 6, 0);
    lbl_my_uid = uid_lbl;  // ✅ FIX: เก็บ pointer ไว้อัปเดตได้ภายหลัง

    // ── Setting rows ──
    auto make_setting_row = [&](const char* icon, const char* label, const char* sub,
                                 lv_color_t ico_bg, lv_event_cb_t cb) {
        lv_obj_t* row = lv_obj_create(panel_settings);
        lv_obj_set_size(row, 220, 46);
        lv_obj_set_style_bg_color(row, C_WHITE, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, C_BORDER, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* ico = lv_obj_create(row);
        lv_obj_set_size(ico, 30, 30);
        lv_obj_set_style_bg_color(ico, ico_bg, 0);
        lv_obj_set_style_radius(ico, 8, 0);
        lv_obj_set_style_border_width(ico, 0, 0);
        lv_obj_t* ico_lbl = lv_label_create(ico);
        lv_label_set_text(ico_lbl, icon);
        lv_obj_center(ico_lbl);

        lv_obj_t* txt_cont = lv_obj_create(row);
        lv_obj_set_size(txt_cont, 160, 30);
        lv_obj_set_style_bg_opa(txt_cont, 0, 0);
        lv_obj_set_style_border_width(txt_cont, 0, 0);
        lv_obj_set_style_pad_all(txt_cont, 0, 0);

        lv_obj_t* nm = lv_label_create(txt_cont);
        lv_label_set_text(nm, label);
        lv_obj_set_style_text_font(nm, &lv_font_unscii_8, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 8, 0);

        if (sub) {
            lv_obj_t* sb = lv_label_create(txt_cont);
            lv_label_set_text(sb, sub);
            lv_obj_set_style_text_color(sb, C_SUB, 0);
            lv_obj_set_style_text_font(sb, &lv_font_unscii_8, 0);
            lv_obj_align(sb, LV_ALIGN_BOTTOM_LEFT, 8, 0);
        }
    };

    make_setting_row("*",    "Change Display Name", "Edit your name",
                     lv_color_hex(0xE8F5E9), settings_rename_cb);
    make_setting_row("Off",   "Logout",              "End session",
                     lv_color_hex(0xE8F0FF), settings_logout_cb);
    make_setting_row("Cal", "Recalibrate Touch",   "Fix touch accuracy",
                     lv_color_hex(0xFFF9E0), settings_recal_cb);
    make_setting_row("Del",   "Delete My Account",   "Remove all data",
                     lv_color_hex(0xFDECEA), settings_delete_cb);
}

// ─────────────────────────────────────────────
//  ROOM SCREEN
// ─────────────────────────────────────────────
static lv_obj_t* ta_room_input = nullptr;
static lv_obj_t* kb_room       = nullptr;
static lv_obj_t* room_info_overlay = nullptr;
static lv_obj_t* room_members_list = nullptr;
static lv_obj_t* room_req_list     = nullptr;

static void room_back_cb(lv_event_t* e) {
    gui_show_screen(SCR_HOME);
}
static void room_menu_cb(lv_event_t* e) {
    // toggle info overlay
    if (!room_info_overlay) return;
    bool hidden = lv_obj_has_flag(room_info_overlay, LV_OBJ_FLAG_HIDDEN);
    if (hidden) lv_obj_clear_flag(room_info_overlay, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(room_info_overlay,   LV_OBJ_FLAG_HIDDEN);
}
static void room_info_close_cb(lv_event_t* e) {
    if (room_info_overlay) lv_obj_add_flag(room_info_overlay, LV_OBJ_FLAG_HIDDEN);
}
static void room_kb_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (!ta_room_input) return;
        const char* txt = lv_textarea_get_text(ta_room_input);
        if (strlen(txt) > 0) {
            on_gui_send_message(txt);
            lv_textarea_set_text(ta_room_input, "");
        }
        lv_obj_add_flag(kb_room, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb_room, LV_OBJ_FLAG_HIDDEN);
    }
}
static void room_ta_focus_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(kb_room, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(kb_room, ta_room_input);
    }
}
static void room_send_btn_cb(lv_event_t* e) {
    if (!ta_room_input) return;
    const char* txt = lv_textarea_get_text(ta_room_input);
    if (strlen(txt) == 0) return;
    on_gui_send_message(txt);
    lv_textarea_set_text(ta_room_input, "");
    if (kb_room) lv_obj_add_flag(kb_room, LV_OBJ_FLAG_HIDDEN);
}

static void build_room() {
    scr_room = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr_room, C_BG, 0);
    lv_obj_clear_flag(scr_room, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header (44px) ──
    lv_obj_t* hdr = lv_obj_create(scr_room);
    lv_obj_set_size(hdr, 240, 44);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_WHITE, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(hdr, C_BORDER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Back button
    lv_obj_t* back_btn = lv_btn_create(hdr);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, room_back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_color(back_lbl, C_BLUE, 0);
    lv_obj_center(back_lbl);

    // Room title
    room_title = lv_label_create(hdr);
    lv_label_set_text(room_title, "Room");
    lv_obj_set_style_text_font(room_title, &lv_font_unscii_8, 0);
    lv_obj_align(room_title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_long_mode(room_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(room_title, 140);

    // Menu button
    lv_obj_t* menu_btn = lv_btn_create(hdr);
    lv_obj_set_size(menu_btn, 36, 36);
    lv_obj_align(menu_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(menu_btn, 0, 0);
    lv_obj_set_style_border_width(menu_btn, 0, 0);
    lv_obj_set_style_shadow_width(menu_btn, 0, 0);
    lv_obj_add_event_cb(menu_btn, room_menu_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* menu_lbl = lv_label_create(menu_btn);
    lv_label_set_text(menu_lbl, "Gr");
    lv_obj_set_style_text_color(menu_lbl, lv_color_hex(0x333333), 0);
    lv_obj_center(menu_lbl);

    // ── Chat messages area (44..266) = 222px ──
    room_chat_cont = lv_obj_create(scr_room);
    lv_obj_set_size(room_chat_cont, 240, 222);
    lv_obj_align(room_chat_cont, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_color(room_chat_cont, C_BG, 0);
    lv_obj_set_style_border_width(room_chat_cont, 0, 0);
    lv_obj_set_style_pad_all(room_chat_cont, 6, 0);
    lv_obj_set_flex_flow(room_chat_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(room_chat_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(room_chat_cont, 4, 0);
    lv_obj_set_style_pad_right(room_chat_cont, 4, 0);
    lv_obj_set_style_pad_left(room_chat_cont, 4, 0);

    // ── Input bar (266..320) = 54px ──
    lv_obj_t* input_bar = lv_obj_create(scr_room);
    lv_obj_set_size(input_bar, 240, 54);
    lv_obj_align(input_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(input_bar, C_BG, 0);
    lv_obj_set_style_border_width(input_bar, 0, 0);
    lv_obj_set_style_border_side(input_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(input_bar, C_BORDER, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_pad_hor(input_bar, 6, 0);
    lv_obj_set_style_pad_ver(input_bar, 6, 0);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_bar, 6, 0);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);

    ta_room_input = make_input(input_bar, "Type a message...", false, 174);
    lv_obj_add_event_cb(ta_room_input, room_ta_focus_cb, LV_EVENT_ALL, nullptr);

    lv_obj_t* send_btn = make_btn(input_bar, "Send", C_GREEN, C_WHITE, 54, 38);
    lv_obj_add_event_cb(send_btn, room_send_btn_cb, LV_EVENT_CLICKED, nullptr);

    // ── Keyboard (overlaps chat, slides up) ──
    kb_room = lv_keyboard_create(scr_room);
    lv_obj_set_size(kb_room, 240, 140);
    lv_obj_align(kb_room, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb_room, LV_OBJ_FLAG_HIDDEN);  // ซ่อนก่อน แสดงเมื่อ focus
    lv_obj_add_event_cb(kb_room, room_kb_cb, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(kb_room, LV_OBJ_FLAG_HIDDEN);

    // ── Info Overlay (slide from right, 200px wide) ──
    room_info_overlay = lv_obj_create(scr_room);
    lv_obj_set_size(room_info_overlay, 200, 320);
    lv_obj_align(room_info_overlay, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(room_info_overlay, C_WHITE, 0);
    lv_obj_set_style_border_width(room_info_overlay, 1, 0);
    lv_obj_set_style_border_color(room_info_overlay, C_BORDER, 0);
    lv_obj_set_style_radius(room_info_overlay, 0, 0);
    lv_obj_set_style_shadow_width(room_info_overlay, 10, 0);
    lv_obj_set_style_pad_all(room_info_overlay, 10, 0);
    lv_obj_set_flex_flow(room_info_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(room_info_overlay, 6, 0);
    lv_obj_add_flag(room_info_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* info_title = lv_label_create(room_info_overlay);
    lv_label_set_text(info_title, "Chat Settings");
    lv_obj_set_style_text_font(info_title, &lv_font_unscii_8, 0);

    lv_obj_t* sep1 = lv_obj_create(room_info_overlay);
    lv_obj_set_size(sep1, 180, 1);
    lv_obj_set_style_bg_color(sep1, C_BORDER, 0);
    lv_obj_set_style_border_width(sep1, 0, 0);

    lv_obj_t* mem_lbl = lv_label_create(room_info_overlay);
    lv_label_set_text(mem_lbl, "Members:");
    lv_obj_set_style_text_font(mem_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(mem_lbl, C_SUB, 0);

    room_members_list = lv_list_create(room_info_overlay);
    lv_obj_set_size(room_members_list, 180, 120);
    lv_obj_set_style_bg_opa(room_members_list, 0, 0);
    lv_obj_set_style_border_width(room_members_list, 0, 0);
    lv_obj_set_style_pad_all(room_members_list, 0, 0);

    lv_obj_t* sep2 = lv_obj_create(room_info_overlay);
    lv_obj_set_size(sep2, 180, 1);
    lv_obj_set_style_bg_color(sep2, C_BORDER, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);

    lv_obj_t* req_lbl = lv_label_create(room_info_overlay);
    lv_label_set_text(req_lbl, "Join Requests:");
    lv_obj_set_style_text_font(req_lbl, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(req_lbl, C_SUB, 0);

    room_req_list = lv_list_create(room_info_overlay);
    lv_obj_set_size(room_req_list, 180, 60);
    lv_obj_set_style_bg_opa(room_req_list, 0, 0);
    lv_obj_set_style_border_width(room_req_list, 0, 0);
    lv_obj_set_style_pad_all(room_req_list, 0, 0);

    lv_obj_t* close_btn = make_btn(room_info_overlay, "Close", C_GRAY_BTN,
                                    C_SUB, 180, 36);
    lv_obj_add_event_cb(close_btn, room_info_close_cb, LV_EVENT_CLICKED, nullptr);
}

// ─────────────────────────────────────────────
//  Navigation
// ─────────────────────────────────────────────
void gui_show_screen(ScreenID id) {
    lv_obj_t* target = nullptr;
    switch (id) {
        case SCR_SPLASH:   target = scr_splash; break;
        case SCR_LOGIN:    target = scr_login;  break;
        case SCR_REGISTER: target = scr_reg;    break;
        case SCR_HOME:     target = scr_home;   break;
        case SCR_ROOM:     target = scr_room;   break;
        default: return;
    }
    if (target) {
        g_current_screen = id;
        lv_scr_load(target);
    }
}

void gui_show_room(const char* roomName) {
    strncpy(g_current_room, roomName, sizeof(g_current_room) - 1);
    // Update room title
    if (room_title) lv_label_set_text(room_title, roomName);
    // Clear old chat
    if (room_chat_cont) lv_obj_clean(room_chat_cont);
    // Hide info panel
    if (room_info_overlay) lv_obj_add_flag(room_info_overlay, LV_OBJ_FLAG_HIDDEN);
    gui_show_screen(SCR_ROOM);
}

// ─────────────────────────────────────────────
//  Data update APIs
// ─────────────────────────────────────────────
void gui_set_my_info(uint32_t uid, const char* displayName) {
    g_my_uid = uid;
    strncpy(g_my_name, displayName, sizeof(g_my_name) - 1);
    if (lbl_my_name) lv_label_set_text(lbl_my_name, displayName);
    // ✅ FIX: อัปเดต UID label ด้วย ไม่งั้นจะแสดง 0 ตลอด
    if (lbl_my_uid) {
        char uid_str[24];
        snprintf(uid_str, sizeof(uid_str), "UID: %lu", (unsigned long)uid);
        lv_label_set_text(lbl_my_uid, uid_str);
    }
}

void gui_update_userlist(const GuiUserEntry* users, int count) {
    if (!list_users) return;
    lv_obj_clean(list_users);
    if (count == 0) {
        lv_obj_t* empty = lv_label_create(list_users);
        lv_label_set_text(empty, "No friends yet");
        lv_obj_set_style_text_color(empty, C_SUB, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < count; i++) {
        // Row
        lv_obj_t* row = lv_obj_create(list_users);
        lv_obj_set_size(row, 238, 48);
        lv_obj_set_style_bg_color(row, C_WHITE, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, C_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // Avatar
        lv_obj_t* av = lv_obj_create(row);
        lv_obj_set_size(av, 34, 34);
        lv_obj_set_style_bg_color(av, lv_color_hex(0x3D5A80), 0);
        lv_obj_set_style_radius(av, 10, 0);
        lv_obj_set_style_border_width(av, 0, 0);
        lv_obj_t* av_l = lv_label_create(av);
        char init[2] = { users[i].name[0] ? users[i].name[0] : '?', 0 };
        lv_label_set_text(av_l, init);
        lv_obj_set_style_text_color(av_l, C_WHITE, 0);
        lv_obj_center(av_l);

        // Name info
        lv_obj_t* nc = lv_obj_create(row);
        lv_obj_set_size(nc, 170, 34);
        lv_obj_set_style_bg_opa(nc, 0, 0);
        lv_obj_set_style_border_width(nc, 0, 0);
        lv_obj_set_style_pad_all(nc, 0, 0);

        lv_obj_t* nm = lv_label_create(nc);
        lv_label_set_text(nm, users[i].name);
        lv_obj_set_style_text_font(nm, &lv_font_unscii_8, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 8, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, 155);

        // Online dot
        lv_obj_t* dot = lv_obj_create(nc);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, users[i].online ? C_GREEN : C_SUB, 0);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_LEFT, 8, 0);

        // ให้ child objects bubble click event ขึ้นมาที่ row
        lv_obj_add_flag(av, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(av_l, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(nc, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(nm, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
        // ปิด clickable ของ child เพื่อไม่ให้แย่ง event
        lv_obj_clear_flag(av, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(nc, LV_OBJ_FLAG_CLICKABLE);

        uint32_t uid_copy = users[i].uid;
        lv_obj_add_event_cb(row, friend_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)uid_copy);
    }
}

// Static buffer for room name strings used in callbacks (must outlive the list items)
static char room_name_buf[MAX_ROOMS][48];

void gui_update_roomlist(const GuiRoomEntry* rooms_arr, int count) {
    if (!list_rooms) return;
    lv_obj_clean(list_rooms);
    if (count == 0) {
        lv_obj_t* empty = lv_label_create(list_rooms);
        lv_label_set_text(empty, "No groups");
        lv_obj_set_style_text_color(empty, C_SUB, 0);
        return;
    }

    // Keep static copies of room names for callbacks (uses file-scope room_name_buf)

    for (int i = 0; i < count; i++) {
        strncpy(room_name_buf[i], rooms_arr[i].name, 47);

        lv_obj_t* row = lv_obj_create(list_rooms);
        lv_obj_set_size(row, 238, 52);
        lv_obj_set_style_bg_color(row, C_WHITE, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, C_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // Group icon
        lv_obj_t* av = lv_obj_create(row);
        lv_obj_set_size(av, 36, 36);
        lv_obj_set_style_bg_color(av, lv_color_hex(0x3D5A80), 0);
        lv_obj_set_style_radius(av, 10, 0);
        lv_obj_set_style_border_width(av, 0, 0);
        lv_obj_t* g_l = lv_label_create(av);
        lv_label_set_text(g_l, "G");
        lv_obj_set_style_text_color(g_l, C_WHITE, 0);
        lv_obj_center(g_l);

        // Name + members
        lv_obj_t* nc = lv_obj_create(row);
        lv_obj_set_size(nc, 128, 36);
        lv_obj_set_style_bg_opa(nc, 0, 0);
        lv_obj_set_style_border_width(nc, 0, 0);
        lv_obj_set_style_pad_all(nc, 0, 0);

        lv_obj_t* nm = lv_label_create(nc);
        lv_label_set_text(nm, rooms_arr[i].name);
        lv_obj_set_style_text_font(nm, &lv_font_unscii_8, 0);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_width(nm, 120);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 6, 0);

        char mc[24]; snprintf(mc, sizeof(mc), "%d members", rooms_arr[i].memberCount);
        lv_obj_t* ms = lv_label_create(nc);
        lv_label_set_text(ms, mc);
        lv_obj_set_style_text_color(ms, C_SUB, 0);
        lv_obj_set_style_text_font(ms, &lv_font_unscii_8, 0);
        lv_obj_align(ms, LV_ALIGN_BOTTOM_LEFT, 6, 0);

        // Action button
        uint8_t status = rooms_arr[i].status;
        if (status == 1 || status == 2) {
            // Owner or Member -> Open
            lv_obj_t* btn = make_btn(row,
                status == 1 ? "Owner" : "Open",
                status == 1 ? lv_color_hex(0xFFF6E0) : lv_color_hex(0xE6F9EE),
                status == 1 ? C_ORANGE : lv_color_hex(0x068040),
                58, 30);
            lv_obj_add_event_cb(btn, open_room_cb, LV_EVENT_CLICKED, room_name_buf[i]);
        } else if (status == 3) {
            lv_obj_t* btn = make_btn(row, "Pending", lv_color_hex(0xFFF6E0), C_ORANGE, 58, 30);
            lv_obj_set_style_border_color(btn, C_BORDER, 0);
        } else {
            lv_obj_t* btn = make_btn(row, "Join", C_GREEN, C_WHITE, 58, 30);
            lv_obj_add_event_cb(btn, join_room_cb, LV_EVENT_CLICKED, room_name_buf[i]);
        }

        // bubble events จาก child ขึ้นมา row
        lv_obj_add_flag(av, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(g_l, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(nc, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(nm, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(ms, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(av, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(nc, LV_OBJ_FLAG_CLICKABLE);

        // Click row to open if member/owner
        if (status == 1 || status == 2) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, open_room_cb, LV_EVENT_CLICKED, room_name_buf[i]);
        }
    }
}

// Invite accept/decline callbacks (store idx in user_data)
static void invite_accept_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    on_gui_accept_invite(idx);
}
static void invite_decline_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    on_gui_decline_invite(idx);
}

void gui_update_invites(const GuiInviteEntry* invites, int count) {
    if (!list_invites) return;
    lv_obj_clean(list_invites);
    if (count == 0) {
        lv_obj_t* empty = lv_label_create(list_invites);
        lv_label_set_text(empty, "No invitations");
        lv_obj_set_style_text_color(empty, C_SUB, 0);
        return;
    }
    for (int i = 0; i < count; i++) {
        lv_obj_t* card = lv_obj_create(list_invites);
        lv_obj_set_size(card, 236, 80);
        lv_obj_set_style_bg_color(card, C_WHITE, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, C_BORDER, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_top(card, 4, 0);
        lv_obj_set_style_pad_bottom(card, 4, 0);

        lv_obj_t* room_lbl = lv_label_create(card);
        lv_label_set_text(room_lbl, invites[i].room);
        lv_obj_set_style_text_font(room_lbl, &lv_font_unscii_8, 0);
        lv_obj_align(room_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        char from[48]; snprintf(from, sizeof(from), "From: %s", invites[i].inviter);
        lv_obj_t* from_lbl = lv_label_create(card);
        lv_label_set_text(from_lbl, from);
        lv_obj_set_style_text_color(from_lbl, C_SUB, 0);
        lv_obj_set_style_text_font(from_lbl, &lv_font_unscii_8, 0);
        lv_obj_align(from_lbl, LV_ALIGN_TOP_LEFT, 0, 18);

        lv_obj_t* acc = make_btn(card, "Accept", C_GREEN, C_WHITE, 96, 28);
        lv_obj_align(acc, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(acc, invite_accept_cb, LV_EVENT_CLICKED, (void*)(intptr_t)invites[i].idx);

        lv_obj_t* dec = make_btn(card, "Decline", C_GRAY_BTN, C_SUB, 96, 28);
        lv_obj_align(dec, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_event_cb(dec, invite_decline_cb, LV_EVENT_CLICKED, (void*)(intptr_t)invites[i].idx);
    }
}

void gui_update_members(const GuiMemberEntry* members, int count) {
    if (!room_members_list) return;
    lv_obj_clean(room_members_list);
    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_list_add_btn(room_members_list, nullptr, members[i].name);
        lv_obj_set_style_bg_color(row, C_WHITE, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        // online dot
        lv_obj_t* dot = lv_obj_create(row);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, members[i].online ? C_GREEN : C_SUB, 0);
        lv_obj_set_style_radius(dot, 4, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void gui_update_requests(const GuiMemberEntry* reqs, int count) {
    if (!room_req_list) return;
    lv_obj_clean(room_req_list);
    for (int i = 0; i < count; i++) {
        lv_obj_t* row = lv_list_add_btn(room_req_list, nullptr, reqs[i].name);
        lv_obj_set_style_bg_color(row, C_WHITE, 0);
        lv_obj_set_style_border_width(row, 0, 0);
    }
}

// ─────────────────────────────────────────────
//  Chat bubbles
// ─────────────────────────────────────────────
void gui_add_chat_bubble(const char* sender, uint32_t senderUid,
                          const char* msg, bool isSystem) {
    if (!room_chat_cont) return;
    bool is_me = (senderUid == g_my_uid);

    if (isSystem) {
        // System message – centered pill
        lv_obj_t* pill = lv_obj_create(room_chat_cont);
        lv_obj_set_width(pill, 220);
        lv_obj_set_height(pill, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(pill, lv_color_hex(0x00000026), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_40, 0);
        lv_obj_set_style_radius(pill, 20, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_pad_hor(pill, 12, 0);
        lv_obj_set_style_pad_ver(pill, 4, 0);
        lv_obj_set_align(pill, LV_ALIGN_CENTER);

        lv_obj_t* lbl = lv_label_create(pill);
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_font(lbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x555555), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, 196);
        lv_obj_center(lbl);
    } else {
        // User bubble — วางใน container เต็มความกว้าง แล้ว align bubble ซ้าย/ขวา
        lv_obj_t* row = lv_obj_create(room_chat_cont);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, 0, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 2, 0);
        lv_obj_set_style_pad_top(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Sender label (เฉพาะข้อความจากคนอื่น)
        if (!is_me) {
            lv_obj_t* slbl = lv_label_create(row);
            lv_label_set_text(slbl, sender);
            lv_obj_set_style_text_font(slbl, &lv_font_unscii_8, 0);
            lv_obj_set_style_text_color(slbl, C_SUB, 0);
            lv_obj_align(slbl, LV_ALIGN_TOP_LEFT, 4, 0);
        }

        // Bubble
        lv_obj_t* bubble = lv_obj_create(row);
        lv_obj_set_height(bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bubble, is_me ? C_GREEN2 : C_WHITE, 0);
        lv_obj_set_style_radius(bubble, 14, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, C_BORDER, 0);
        lv_obj_set_style_pad_all(bubble, 8, 0);

        lv_obj_t* mlbl = lv_label_create(bubble);
        lv_label_set_text(mlbl, msg);
        lv_obj_set_style_text_font(mlbl, &lv_font_unscii_8, 0);
        lv_obj_set_style_text_color(mlbl,
            is_me ? C_DARK : lv_color_hex(0x222222), 0);
        lv_label_set_long_mode(mlbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(mlbl, 150);

        // size bubble ตาม content แล้ว align ซ้าย/ขวา
        lv_obj_set_width(bubble, LV_SIZE_CONTENT);
        int top_offset = is_me ? 2 : 18;  // เว้นพื้นที่ให้ sender label
        if (is_me) {
            lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -4, top_offset);
        } else {
            lv_obj_align(bubble, LV_ALIGN_TOP_LEFT, 4, top_offset);
        }
        // กำหนดความสูง row ให้พอ
        lv_obj_set_height(row, LV_SIZE_CONTENT);
    }

    // Auto-scroll to bottom
    lv_obj_scroll_to_y(room_chat_cont, LV_COORD_MAX, LV_ANIM_ON);
}

// ─────────────────────────────────────────────
//  Init
// ─────────────────────────────────────────────
void gui_init() {
    tft.begin();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    // ── Auto-calibration ──────────────────────────────────────────
    // ถ้ากด touch ค้างไว้ตอนเปิดเครื่อง จะเข้าโหมด calibrate
    // ถ้าไม่ได้กด ใช้ค่าที่ save ไว้ใน NVS (หรือ default)
    uint16_t calData[5];
    bool doCalibration = false;

    // อ่านค่าที่เคย save ไว้ (ใช้ Preferences/NVS)
    Preferences prefs;
    prefs.begin("touch_cal", true); // read-only
    bool calSaved = prefs.getBool("saved", false);
    if (calSaved) {
        calData[0] = prefs.getUShort("c0", 275);
        calData[1] = prefs.getUShort("c1", 3620);
        calData[2] = prefs.getUShort("c2", 264);
        calData[3] = prefs.getUShort("c3", 3532);
        calData[4] = prefs.getUShort("c4", 1);
    } else {
        doCalibration = true;
    }
    prefs.end();

    // ตรวจว่ากด touch ค้างตอนบูต → force re-calibrate
    uint16_t tx, ty;
    if (tft.getTouch(&tx, &ty)) {
        doCalibration = true;
    }

    if (doCalibration) {
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(20, 130);
        tft.setTextColor(TFT_WHITE);
        tft.setTextSize(1);
        tft.println("Touch calibration");
        tft.println("Tap the corners");
        tft.calibrateTouch(calData, TFT_WHITE, TFT_BLACK, 15);

        // บันทึกค่า
        Preferences prefsW;
        prefsW.begin("touch_cal", false);
        prefsW.putBool("saved", true);
        prefsW.putUShort("c0", calData[0]);
        prefsW.putUShort("c1", calData[1]);
        prefsW.putUShort("c2", calData[2]);
        prefsW.putUShort("c3", calData[3]);
        prefsW.putUShort("c4", calData[4]);
        prefsW.end();

        Serial.printf("Cal: %d %d %d %d %d\n",
            calData[0], calData[1], calData[2], calData[3], calData[4]);
    }

    tft.setTouch(calData);
    tft.fillScreen(TFT_BLACK);
    // ─────────────────────────────────────────────────────────────

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, 240 * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 240;
    disp_drv.ver_res  = 320;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touch_read;
    lv_indev_drv_register(&indev_drv);

    // Build all screens up-front
    build_splash();
    build_login();
    build_register();
    build_home();
    build_room();
    build_toast_overlay();

    // Start at splash
    gui_show_screen(SCR_SPLASH);
}

void gui_task_handler() {
    lv_tick_inc(5);        // บอก LVGL ว่าผ่านไป ~5ms ทุกครั้งที่เรียก
    lv_timer_handler();
}