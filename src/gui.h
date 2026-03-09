#pragma once
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Arduino.h>

// ===================== Screen IDs =====================
typedef enum {
  SCR_SPLASH = 0,   // หน้าแรก (โลโก้ + Login/Register)
  SCR_LOGIN,        // หน้ากรอก Login
  SCR_REGISTER,     // หน้ากรอก Register
  SCR_HOME,         // หน้าหลัก (Friends / Groups / Invites / Settings)
  SCR_ROOM,         // หน้าแชทห้อง
  SCR_ROOM_INFO,    // หน้าข้อมูล/สมาชิกห้อง (slide-in panel)
} ScreenID;

// ===================== Callbacks จาก main.cpp =====================
extern void on_gui_login(const char* username, const char* password);
extern void on_gui_register(const char* username, const char* password);
extern void on_gui_send_message(const char* msg);
extern void on_gui_create_room(const char* roomName);
extern void on_gui_join_room(const char* roomName);
extern void on_gui_leave_room(const char* roomName);
extern void on_gui_open_room(const char* roomName);
extern void on_gui_rename_me(const char* newName);
extern void on_gui_logout();
extern void on_gui_delete_self();
extern void on_gui_accept_invite(int idx);
extern void on_gui_decline_invite(int idx);
extern void on_gui_open_dm(uint32_t uid);

// ===================== API structs สำหรับ update =====================
struct GuiUserEntry {
  uint32_t uid;
  char name[32];
  bool online;
};

struct GuiRoomEntry {
  char name[48];
  int memberCount;
  uint8_t status; // 0=none,1=owner,2=member,3=pending
};

struct GuiInviteEntry {
  int idx;
  char room[48];
  char inviter[32];
};

struct GuiMemberEntry {
  uint32_t uid;
  char name[32];
  bool online;
};

// ===================== Init & Task =====================
void gui_init();
void gui_task_handler();
void gui_poll_touch();   // เรียกใน loop() ก่อน gui_task_handler()

// ===================== Navigation =====================
void gui_show_screen(ScreenID id);
void gui_show_room(const char* roomName);

// ===================== Data update (เรียกจาก main.cpp) =====================
void gui_set_my_info(uint32_t uid, const char* displayName);
void gui_update_userlist(const GuiUserEntry* users, int count);
void gui_update_roomlist(const GuiRoomEntry* rooms, int count);
void gui_update_invites(const GuiInviteEntry* invites, int count);
void gui_update_members(const GuiMemberEntry* members, int count);
void gui_update_requests(const GuiMemberEntry* reqs, int count);
void gui_add_chat_bubble(const char* sender, uint32_t senderUid, const char* msg, bool isSystem);
void gui_set_chat_log(const char* htmlLog); // ถ้าต้องการ render HTML (optional)
void gui_show_toast(const char* msg);
void gui_show_error(const char* msg);