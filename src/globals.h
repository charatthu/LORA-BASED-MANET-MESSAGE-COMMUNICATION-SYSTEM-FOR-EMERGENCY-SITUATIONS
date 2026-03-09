#pragma once
#include <Arduino.h>

// ===================== CONFIG (shared) =====================
extern const int NODE_ID;
extern const bool ALLOW_LEAVE_DEFAULT;

// ===================== Limits & Constants =====================
const int MAX_ROOMS = 30;  // ✅ FIX: เพิ่มจาก 10 → 30 เพราะ DM แต่ละคู่กินไป 1 slot
const int MAX_USERS = 50; 
const int MAX_DB_USERS = 50;
// --- แก้ไข 3 บรรทัดนี้ครับ ---
extern const char* DB_FILE;
extern const char* ROOMS_FILE;

const int MAX_MEMBERS_PER_ROOM = 40;
const int MAX_REQUESTS_PER_ROOM = 20;
const int MAX_INVITES_PER_USER = 14;
const int MAX_LOG_LEN = 9000;
extern int lastLoRaRSSI;

struct RoomRec {
  bool used = false;
  String name;

  // owner
  uint32_t ownerUid = 0;
  String ownerIP ;

  // chat
  String logHtml;
  int unreadCount = 0;

  // members (transport)
  String members[MAX_MEMBERS_PER_ROOM];
  int memberCount = 0;

  // members (identity)
  uint32_t memberUids[MAX_MEMBERS_PER_ROOM];
  int memberUidCount = 0;

  // join requests (UID)
  uint32_t requests[MAX_REQUESTS_PER_ROOM];
  int requestCount = 0;
};

extern RoomRec rooms[];

void addChatMessage(int ridx, const String& senderName, uint32_t senderUid, const String& msg, bool isSystem);
bool isUidOnline(uint32_t uid);

// ===================== Utils (functions used in web.cpp) =====================
String htmlEscape(String text);
String attrEscape(String text);