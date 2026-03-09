#include <WiFi.h>
#include <stdlib.h>
#include <ctype.h>
#include <SPI.h>
#include <LoRa.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiUdp.h>
#include "web.h"
#include "globals.h"
#include "gui.h"

// ── GUI session state (defined later in this file, used in addChatMessage) ──
static String   g_gui_myIP    = "";
static uint32_t g_gui_myUid   = 0;
static String   g_gui_curRoom = "";


WiFiUDP udp;
String discoveredIP = ""; // เอาไว้เก็บไอพีที่ค้นหาเจอ
void discoverServer();

// ===================== CONFIG: LORA MESH =====================
// *** เปลี่ยนเลขนี้ให้ไม่ซ้ำกันในแต่ละบอร์ด (1, 2, 3, 4) ***
// #define NODE_ID 2
const int NODE_ID = 4;

// กำหนดขา Pin ของ LoRa
#define LORA_CS   27
#define LORA_RST  14
#define LORA_IRQ  26 
#define LORA_FREQ 433E6

//#define PKT_SYNC_USER_FULL   20

// ===================== CONFIG: COMPUTER WIFI =====================
const char* COM_SSID = "TEST24G";   
const char* COM_PASS = "12345678";         
const char* SERVER_URL  = "http://192.168.137.1:5001/update"; 
const char* LOG_URL     = "http://192.168.137.1:5001/log_chat";

// DB
struct DbUser {
  uint32_t uid;       
  char username[21];  
  char password[21];  
  char displayName[21];
};
extern int dbUserCount;
extern DbUser dbUsers[];
int findDbUserByName(const char* name);

// User helpers
int findDbUserByUid(uint32_t uid) {
  for (int i = 0; i < dbUserCount; i++) {
    if (dbUsers[i].uid == uid) {
      return i;
    }
  }
  return -1;
}


String getDisplayNameByUid(uint32_t uid) {
  int idx = findDbUserByUid(uid);
  if (idx < 0) return "Unknown";
  if (strlen(dbUsers[idx].displayName) > 0)
    return String(dbUsers[idx].displayName);
  return String(dbUsers[idx].username);
}
bool needSaveDb = true;
void broadcastUserFull(uint32_t uid);

// Packet Structure
struct LoraPacket {
  uint8_t type;         
  uint8_t fromNode;     
  uint8_t fromUserIp;
  uint8_t ttl;          // ✅ เพิ่ม — นับถอยหลัง hop
  uint8_t originNode;   // ✅ เพิ่ม — node ต้นทาง (ใช้ dedup)   
  char senderName[21];  
  char target[25];      
  char payload[100];    
  uint32_t msgId;       
};

// Types
const uint8_t PKT_HEARTBEAT = 0;
const uint8_t PKT_CHAT_GROUP = 1;
const uint8_t PKT_CHAT_DM = 2;
const uint8_t PKT_INVITE = 3;
const uint8_t PKT_JOIN_NOTIFY = 4;
const uint8_t PKT_LEAVE_NOTIFY = 5;
const uint8_t PKT_JOIN_REQ = 6;     
const uint8_t PKT_JOIN_APPROVE = 7; 
const uint8_t PKT_SYNC_USER_FULL = 11;

// ===================== Timings =====================
const int LORA_TIMEOUT_MS = 4000; 
uint32_t lastHeartbeatTime = 0;
const int HEARTBEAT_INTERVAL = 15000; 
unsigned long lastBroadcastTime = 0; 
const unsigned long ONLINE_TIMEOUT_MS = 120000; 
unsigned long lastReportTime = 0; 

// ===================== Wi-Fi AP =====================
char ssid[32]; 
const char* password = "12345678";
WiFiServer server(80);

// ===================== Topology Data =====================
struct NodeStats {
    bool active;
    int rssi;
    unsigned long lastHeard;
    bool isGateway;
    bool userSynced;
};
NodeStats topology[255]; 


// ===================== Default Group =====================
const char* DEFAULT_ROOM_NAME = "General";
const char* DEFAULT_OWNER_IP  = "SYSTEM";       
const bool  ALLOW_LEAVE_DEFAULT = false;        

// ===================== Global =====================
String header;
const char* ADMIN_PASS = "admin123";   // เอาไว้ลบuserทั้งหมด

const char* DB_FILE = "/users.dat";
const char* ROOMS_FILE = "/rooms.dat";
int lastLoRaRSSI = 0;

// ✅ เพิ่มตรงนี้
bool needSaveRooms = false;

// ===================== Data Structures =====================

struct Invite {
  bool used = false;
  String roomName;
  uint32_t inviterUid = 0;
};

struct UserRec {
  bool used = false;
  String ip;  
  uint32_t uid = 0;        
  String username;
  String displayName;
  bool isLoggedIn = false;
  unsigned long lastSeen = 0;
  int fromNode;
  bool isRemote = false; 
  uint8_t nodeID = 0;    
  Invite invites[MAX_INVITES_PER_USER];
};

int findDbUserByName(const char* name) {
  for (int i = 0; i < dbUserCount; i++) {
    if (strcmp(dbUsers[i].username, name) == 0) return i;
  }
  return -1;
}

DbUser dbUsers[MAX_DB_USERS];
int dbUserCount = 0;

UserRec users[MAX_USERS];
RoomRec rooms[MAX_ROOMS];

int findUserByIP(const String& ip);
uint32_t getUidFromIP(const String& ip) {
  int idx = findUserByIP(ip);
  if (idx < 0) return 0;
  return users[idx].uid;
}

// แก้ Error: findUserByUid และ getIPFromUid
int findUserByUid(uint32_t uid) {
    if (uid == 0) return -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].used && users[i].uid == uid) return i;
    }
    return -1;
}

String getIPFromUid(uint32_t uid) {
    int idx = findUserByUid(uid);
    if (idx >= 0) return users[idx].ip;
    return "";
}

// เพิ่มเติมเพื่อความเสถียรของระบบ UID
bool isUidOnline(uint32_t uid) {
  if (uid == 0) return false;

  unsigned long now = millis();
  bool seen = false;

  for (int i = 0; i < MAX_USERS; i++) {
    if (!users[i].used) continue;
    if (users[i].uid != uid) continue;

    seen = true;

    // ถ้ามีสัก record ที่ยัง active → online
    if (users[i].isLoggedIn && (now - users[i].lastSeen < 60000)) {
      return true;
    }
  }

  // ถ้าไม่เจอ record ที่ active
  return false;
}


// ===================== LoRa Helpers Forward =====================
bool sendLoraPacket(LoraPacket& pkt, uint8_t dest);
void broadcastUserStatus(const String& ip, const String& name);
void broadcastJoinRoom(const String& myIP, const String& roomName);

// ===================== Persistence Helpers (String I/O) [NEW] =====================
void writeString(File &f, const String &s) {
  uint16_t len = s.length();
  f.write((uint8_t*)&len, sizeof(len));
  if (len > 0) f.print(s);
}

String readString(File &f) {
  uint16_t len;
  f.read((uint8_t*)&len, sizeof(len));
  if (len == 0) return "";
  char* buf = (char*)malloc(len + 1);
  if (!buf) return "";
  f.read((uint8_t*)buf, len);
  buf[len] = '\0';
  String s = String(buf);
  free(buf);
  return s;
}

// ===================== Database Functions (LittleFS) =====================
void loadDb() {
  if (!LittleFS.exists(DB_FILE)) return;
  File f = LittleFS.open(DB_FILE, "r");
  if (f) {
    f.read((uint8_t*)&dbUserCount, sizeof(dbUserCount));
    if(dbUserCount > MAX_DB_USERS) dbUserCount = 0;
    f.read((uint8_t*)dbUsers, sizeof(DbUser) * dbUserCount);
    f.close();
    Serial.println("DB Loaded users.");
  }
}

void saveDb() {
  File f = LittleFS.open(DB_FILE, "w");
  if (f) {
    f.write((uint8_t*)&dbUserCount, sizeof(dbUserCount));
    f.write((uint8_t*)dbUsers, sizeof(DbUser) * dbUserCount);
    f.close();
    Serial.println("DB Saved users.");
  }
}

// [NEW] ฟังก์ชันบันทึก/โหลด ห้องและแชท
void saveRooms() {
  File f = LittleFS.open(ROOMS_FILE, "w");
  if (!f) return;

  for (int i = 0; i < MAX_ROOMS; i++) {
    f.write((uint8_t*)&rooms[i].used, sizeof(bool));
    if (rooms[i].used) {
      writeString(f, rooms[i].name);
      writeString(f, String(rooms[i].ownerUid)); // เซฟ ownerUid
      writeString(f, rooms[i].logHtml);
      
      // ส่วนของ Members (IP) - ของเดิม
      f.write((uint8_t*)&rooms[i].memberCount, sizeof(int));
      for (int m = 0; m < rooms[i].memberCount; m++) {
        writeString(f, rooms[i].members[m]);
      }

      // --- [จุดที่ต้องเพิ่ม] ส่วนของ MemberUids (สำคัญมาก!) ---
      f.write((uint8_t*)&rooms[i].memberUidCount, sizeof(int));
      for (int m = 0; m < rooms[i].memberUidCount; m++) {
        writeString(f, String(rooms[i].memberUids[m])); 
      }

      // ส่วนของ Requests
      f.write((uint8_t*)&rooms[i].requestCount, sizeof(int));
      for (int r = 0; r < rooms[i].requestCount; r++) {
        writeString(f, String(rooms[i].requests[r]));
      }
    }
  }
  f.close();
}

void loadRooms() {
  if (!LittleFS.exists(ROOMS_FILE)) return;
  File f = LittleFS.open(ROOMS_FILE, "r");
  if (!f) return;

  for (int i = 0; i < MAX_ROOMS; i++) {
    if (f.available()) {
       f.read((uint8_t*)&rooms[i].used, sizeof(bool));
       if (rooms[i].used) {
         rooms[i].name = readString(f);
         rooms[i].ownerUid = (uint32_t)readString(f).toInt();
         rooms[i].logHtml = readString(f);
         
         // โหลด Members (IP)
         f.read((uint8_t*)&rooms[i].memberCount, sizeof(int));
         if (rooms[i].memberCount > MAX_MEMBERS_PER_ROOM) rooms[i].memberCount = MAX_MEMBERS_PER_ROOM;
         for (int m = 0; m < rooms[i].memberCount; m++) {
           rooms[i].members[m] = readString(f);
         }

         // --- [จุดที่ต้องเพิ่ม] โหลด MemberUids กลับเข้า RAM ---
         f.read((uint8_t*)&rooms[i].memberUidCount, sizeof(int));
         if (rooms[i].memberUidCount > MAX_MEMBERS_PER_ROOM) rooms[i].memberUidCount = MAX_MEMBERS_PER_ROOM;
         for (int m = 0; m < rooms[i].memberUidCount; m++) {
           rooms[i].memberUids[m] = (uint32_t)readString(f).toInt();
         }

         // โหลด Requests
         f.read((uint8_t*)&rooms[i].requestCount, sizeof(int));
         if (rooms[i].requestCount > MAX_REQUESTS_PER_ROOM) rooms[i].requestCount = MAX_REQUESTS_PER_ROOM;
         for (int r = 0; r < rooms[i].requestCount; r++) {
           rooms[i].requests[r] = (uint32_t)readString(f).toInt();
         }
       } else {
         // Reset ค่าถ้าห้องว่าง
         rooms[i].name = "";
         rooms[i].logHtml = "";
         rooms[i].memberCount = 0;
         rooms[i].memberUidCount = 0; // เพิ่มบรรทัดนี้ด้วย
         rooms[i].requestCount = 0;
       }
    }
  }
  f.close();
  Serial.println("Rooms Loaded");
}

int findDbUser(const char* name) {
  for(int i=0; i<dbUserCount; i++) {
    if(strcmp(dbUsers[i].username, name) == 0) return i;
  }
  return -1;
}

bool deleteUserByName(const char* name) {
  int idx = findDbUser(name);
  if (idx < 0) return false;

  for (int i = idx; i < dbUserCount - 1; i++) {
    dbUsers[i] = dbUsers[i + 1];
  }

  dbUserCount--;
  needSaveDb = true;

  Serial.println(String("Deleted user: ") + name);
  return true;
}

bool registerUser(const char* u, const char* p, uint32_t uid, bool isLocal) {
  if (findDbUser(u) >= 0) return false;
  if (dbUserCount >= MAX_DB_USERS) return false;

  strcpy(dbUsers[dbUserCount].username, u);
  strcpy(dbUsers[dbUserCount].password, p);

  if (isLocal) {
    dbUsers[dbUserCount].uid =
      ((uint32_t)NODE_ID << 24) | (millis() & 0xFFFFFF);
  } else {
    dbUsers[dbUserCount].uid = uid;
  }

  dbUsers[dbUserCount].displayName[0] = '\0';

  uint32_t newUid = dbUsers[dbUserCount].uid;  // ⭐ สำคัญ
  dbUserCount++;
  needSaveDb = true;

  // ⭐⭐⭐ B-4: sync user ใหม่ไปทุก node ⭐⭐⭐
  if (isLocal) {
    broadcastUserFull(newUid);
  }

  return true;
}

bool checkLoginCreds(const char* u, const char* p) {
  int idx = findDbUser(u);
  if (idx < 0) return false;
  return (strcmp(dbUsers[idx].password, p) == 0);
}
void broadcastUserFull(uint32_t uid) {
  int idx = findDbUserByUid(uid);
  if (idx < 0) return;

  LoraPacket pkt;
  memset(&pkt, 0, sizeof(pkt));

  pkt.type = PKT_SYNC_USER_FULL;
  pkt.fromNode = NODE_ID;
  pkt.fromUserIp = 0; // identity ไม่ผูก IP
  pkt.msgId = millis();

  // payload: uid|username|displayName
  String pl =
    String(dbUsers[idx].uid) + "|" +
    String(dbUsers[idx].username) + "|" +
    String(dbUsers[idx].password) + "|" +
    String(dbUsers[idx].displayName);

  strncpy(pkt.payload, pl.c_str(), sizeof(pkt.payload) - 1);
  pkt.payload[sizeof(pkt.payload) - 1] = '\0';

  strncpy(pkt.senderName, "SYSTEM", sizeof(pkt.senderName) - 1);

  sendLoraPacket(pkt, 0xFF); // broadcast ทุก node
}




// ===================== Utility =====================
String ipToString(const IPAddress& ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}

String makeRemoteIP(uint8_t nodeId, uint8_t userIpSuffix) {
  return String("10.") + nodeId + ".0." + userIpSuffix;
}

uint8_t getLastIpByte(const String& ip) {
  int lastDot = ip.lastIndexOf('.');
  if (lastDot == -1) return 0;
  return ip.substring(lastDot + 1).toInt();
}

uint8_t getNodeFromRemoteIP(const String& ip) {
  if (!ip.startsWith("10.")) return NODE_ID; 
  int firstDot = ip.indexOf('.');
  int secondDot = ip.indexOf('.', firstDot + 1);
  return ip.substring(firstDot + 1, secondDot).toInt();
}

String makeLocalPatternIP(uint8_t suffix) {
    return "192.168.4." + String(suffix);
}

String urlDecode(String input) {
  String decoded = "";
  for (int i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '+') decoded += ' ';
    else if (c == '%' && i + 2 < input.length()) {
      char hex[3] = { input[i + 1], input[i + 2], '\0' };
      decoded += (char)strtol(hex, NULL, 16);
      i += 2;
    } else decoded += c;
  }
  return decoded;
}

String urlEncode(const String& s) {
  const char *hex = "0123456789ABCDEF";
  String out = "";
  for (int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c=='-' || c=='_' || c=='.' || c=='~') {
      out += (char)c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String htmlEscape(String text) {
  String out = "";
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else out += c;
  }
  return out;
}

String attrEscape(String text) {
  String out = "";
  for (int i = 0; i < text.length(); i++) {
    char c = text[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String getRequestPath(const String& hdr) {
  int g = hdr.indexOf("GET ");
  if (g < 0) return "/";
  int s = g + 4;
  int e = hdr.indexOf(' ', s);
  if (e < 0) return "/";
  return hdr.substring(s, e);
}

String getQueryParam(const String& path, const String& key) {
  int q = path.indexOf('?');
  if (q < 0) return "";
  String qs = path.substring(q + 1);
  String token = key + "=";
  int k = qs.indexOf(token);
  if (k < 0) return "";
  int vStart = k + token.length();
  int amp = qs.indexOf('&', vStart);
  String val = (amp < 0) ? qs.substring(vStart) : qs.substring(vStart, amp);
  val = urlDecode(val);
  val.trim();
  return val;
}

bool isDmRoomName(const String& n) {
  return n.startsWith("DM:");
}

bool isValidRoomName(const String& r) {
  if (isDmRoomName(r)) {
    if (r.length() < 5 || r.length() > 64) return false;
    return true;
  }
  if (r.length() < 1 || r.length() > 24) return false;
  return true;
}

bool isValidDisplayName(const String& n) {
  if (n.length() < 1 || n.length() > 20) return false;
  return true;
}

bool hasFreeRoomSlot() {
  for (int i = 0; i < MAX_ROOMS; i++) if (!rooms[i].used) return true;
  return false;
}

// ===================== Users =====================
int findUserByIP(const String& ip) {
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].used && users[i].ip == ip) return i;
  }
  return -1;
}

// 1. เช็กว่าออนไลน์ไหม: ต้อง Login + (เวลาไม่เกิน 1 นาที) 
// **หมายเหตุ: หน้าเว็บจะมีระบบ Auto-Ping ทุก 5-10 วินาที ทำให้ lastSeen ใหม่เสมอถ้าเปิดเว็บทิ้งไว้**
bool isUserOnline(const UserRec& u) {
  if (!u.used || !u.isLoggedIn) return false; 
  return (millis() - u.lastSeen < 60000); // 1 นาที (เผื่อไว้กันเน็ตแกว่ง)
}


bool isIpLoggedIn(const String& ip) {
  int idx = findUserByIP(ip);
  if (idx < 0) return false;
  return users[idx].isLoggedIn; 
}

String defaultNameFromIP(const String& ip) {
  int lastDot = ip.lastIndexOf('.');
  String last = (lastDot >= 0) ? ip.substring(lastDot + 1) : "X";
  if (ip.startsWith("10.")) {
      int firstDot = ip.indexOf('.');
      int secondDot = ip.indexOf('.', firstDot + 1);
      String nid = ip.substring(firstDot+1, secondDot);
      return String("Node") + nid + "-" + last;
  }
  return String("User-") + last;
}

String getDisplayName(const String& ip) {
  int idx = findUserByIP(ip);
  if (idx < 0) return defaultNameFromIP(ip);

  // 🔑 guard สำคัญ
  if (users[idx].uid == 0) {
    // ยังไม่ login
    return defaultNameFromIP(ip);
  }

  return getDisplayNameByUid(users[idx].uid);
}

// 2. อัปเดตการติดต่อ: ถ้ามี Request มาจากเว็บ หรือ Heartbeat จาก LoRa
int upsertUserSeen(const String& ip, bool forceLoggedIn = false, int fromNode = -1) {
  int uidx = findUserByIP(ip);
  if (uidx < 0) {
    for (int i = 0; i < MAX_USERS; i++) {
      if (!users[i].used) {
        users[i].used = true;
        users[i].ip = ip;
        users[i].lastSeen = millis();
        users[i].isLoggedIn = forceLoggedIn;
        if (fromNode != -1) users[i].fromNode = fromNode;
        return i;
      }
    }
    return -1;
  }
  // ถ้ามีการติดต่อมา ให้อัปเดตเวลาล่าสุดเสมอ
  users[uidx].lastSeen = millis();
  if (forceLoggedIn) users[uidx].isLoggedIn = true;
  if (fromNode != -1) users[uidx].fromNode = fromNode;
  return uidx;
}

void setDisplayNameByUid(uint32_t uid, const String& name) {
  if (!isValidDisplayName(name)) return;

  int dbIdx = findDbUserByUid(uid);
  if (dbIdx < 0) return;

  if (strcmp(dbUsers[dbIdx].displayName, name.c_str()) == 0) return;

  strncpy(dbUsers[dbIdx].displayName, name.c_str(), 20);
  dbUsers[dbIdx].displayName[20] = '\0';

  needSaveDb = true;
  broadcastUserFull(uid); // ⭐ sync ข้าม node
}



void setDisplayName(const String& ip, const String& name) {
  int idx = findUserByIP(ip);
  if (idx < 0) return;
  if (users[idx].uid == 0) return; // ยังไม่ login
  setDisplayNameByUid(users[idx].uid, name);
}


// ===================== Rooms =====================
int findRoom(const String& name) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].used && rooms[i].name == name) return i;
  }
  return -1;
}

int visibleGroupCount() {
  int c = 0;
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used) continue;
    if (isDmRoomName(rooms[i].name)) continue;
    c++;
  }
  return c;
}

bool isMemberUid(int ridx, uint32_t uid) {
  if (ridx < 0 || ridx >= MAX_ROOMS || uid == 0) return false;
  // ถ้าชื่อห้องคือ general ให้ทุกคนเข้าได้ (Public)
  if (rooms[ridx].name == "general") return true;
  // 1. เพิ่มบรรทัดนี้: ถ้าเป็นเจ้าของกลุ่ม ให้ถือว่าเป็นสมาชิกเสมอ
  if (rooms[ridx].ownerUid == uid) return true;

  // 2. เช็คในรายชื่อสมาชิกทั่วไป (ของเดิมคุณ)
  for (int i = 0; i < rooms[ridx].memberUidCount; i++) {
    if (rooms[ridx].memberUids[i] == uid) return true;
  }
  return false;
}

bool addMemberUid(int ridx, uint32_t uid) {
  if (ridx < 0 || uid == 0) return false;
  if (isMemberUid(ridx, uid)) return true;
  if (rooms[ridx].memberUidCount >= MAX_MEMBERS_PER_ROOM) return false;

  rooms[ridx].memberUids[rooms[ridx].memberUidCount++] = uid;
  return true;
}

bool isMember(int ridx, const String& ip) {
  uint32_t uid = getUidFromIP(ip);
  return isMemberUid(ridx, uid);
}

bool addMember(int ridx, const String& ip) {
  if (ridx < 0) return false;

  uint32_t uid = getUidFromIP(ip);
  if (uid == 0) return false;

  // UID เป็นตัวจริง
  if (!addMemberUid(ridx, uid)) return false;

  // IP เก็บไว้เป็น legacy
  for (int i = 0; i < rooms[ridx].memberCount; i++) {
    if (rooms[ridx].members[i] == ip) {
      needSaveRooms = true;
      return true;
    }
  }

  if (rooms[ridx].memberCount < MAX_MEMBERS_PER_ROOM) {
    rooms[ridx].members[rooms[ridx].memberCount++] = ip;
  }

  needSaveRooms = true;
  return true;
}



bool removeMemberUid(int ridx, uint32_t uid) {
  if (ridx < 0 || uid == 0) return false;

  // remove UID
  for (int i = 0; i < rooms[ridx].memberUidCount; i++) {
    if (rooms[ridx].memberUids[i] == uid) {
      for (int j = i; j < rooms[ridx].memberUidCount - 1; j++)
        rooms[ridx].memberUids[j] = rooms[ridx].memberUids[j + 1];
      rooms[ridx].memberUidCount--;
      break;
    }
  }

  needSaveRooms = true;
  return true;

/* // ===== remove IP =====
  for (int i = 0; i < rooms[ridx].memberCount; i++) {
    if (rooms[ridx].members[i] == ip) {
      for (int j = i; j < rooms[ridx].memberCount - 1; j++)
        rooms[ridx].members[j] = rooms[ridx].members[j + 1];
      rooms[ridx].memberCount--;
      break;
    }
  }

  needSaveRooms = true;
  return true;
  */
}


bool deleteRoomOwnerOnly(const String& roomName, const String& requesterIP) {
  int ridx = findRoom(roomName);
  if (ridx < 0) return false;

  uint32_t reqUid = getUidFromIP(requesterIP);
  if (reqUid == 0) return false;

  if (rooms[ridx].ownerUid != reqUid) return false;
  if (rooms[ridx].ownerUid == 0) return false;

  rooms[ridx] = RoomRec(); // reset ทั้ง struct
  needSaveRooms = true;
  return true;
}


String buildSysBubble(const String& msg) {
  return String("<div class='bubble sys'><span>")
       + String("<div class='sys-label'>System</div>")
       + htmlEscape(msg)
       + String("</span></div>");
}

String buildUserBubble(const String& senderName, uint32_t senderUid, const String& msg) {
  return String("<div class='bubble user' data-uid='") + String(senderUid) 
       + String("' data-sender='") + attrEscape(senderName) + String("'><span>")
       + String("<div class='sender-label'>") + htmlEscape(senderName) + String("</div>")
       + String("<div class='msg-text'>") + htmlEscape(msg) + String("</div>")
       + String("</span></div>");
}

// --- ฟังก์ชันสำหรับส่ง Log ไปเก็บที่คอมพิวเตอร์ ---
// เพิ่ม queue ไว้ข้างบน
struct LogEntry {
  String room, sender, msg;
  bool used = false;
};
LogEntry logQueue[10];

// แก้ sendLogToPC ให้แค่ push เข้า queue แทน
void sendLogToPC(String room, String sender, String msg) {
  for (int i = 0; i < 10; i++) {
    if (!logQueue[i].used) {
      logQueue[i].room = room;      // ✅ assign ทีละ field แทน
      logQueue[i].sender = sender;
      logQueue[i].msg = msg;
      logQueue[i].used = true;
      return;
    }
  }
}

void processLogQueue() {
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 2000) return;  // ลองทุก 2 วิ
  lastTry = millis();

  for (int i = 0; i < 10; i++) {
    if (!logQueue[i].used) continue;

    if (WiFi.status() != WL_CONNECTED) return;
    if (discoveredIP == "") { discoverServer(); return; }

    HTTPClient http;
    http.begin("http://" + discoveredIP + ":5001/log_chat");
    http.setTimeout(500);         // ✅ ไม่แก้ (มีอยู่แล้ว ดีแล้ว)
    http.setConnectTimeout(300);  // ✅ ไม่แก้ (มีอยู่แล้ว ดีแล้ว)
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String postData = "room=" + urlEncode(logQueue[i].room)
                    + "&sender=" + urlEncode(logQueue[i].sender)
                    + "&msg=" + urlEncode(logQueue[i].msg);
    int code = http.POST(postData);
    http.end();

    if (code > 0) logQueue[i].used = false;
    return;  // ส่งทีละ 1 entry ต่อ loop
  }
}

void addChatMessage(int ridx, const String& senderName, uint32_t senderUid, const String& msg, bool isSystem) {
  if (ridx < 0) return;
  
  if (isSystem) rooms[ridx].logHtml += buildSysBubble(msg);
  else {
       rooms[ridx].logHtml += buildUserBubble(senderName, senderUid, msg);
      rooms[ridx].unreadCount++; // เพิ่มจำนวนข้อความค้างอ่าน
  }

  if (rooms[ridx].logHtml.length() > MAX_LOG_LEN) {
    rooms[ridx].logHtml.remove(0, rooms[ridx].logHtml.length() - MAX_LOG_LEN);
  }
  needSaveRooms = true;
  String logSender = isSystem ? "System" : senderName;
  sendLogToPC(rooms[ridx].name, logSender, msg);

  // ส่ง bubble ไปจอทัชสกรีน เมื่อห้องที่รับข้อความตรงกับห้องที่เปิดอยู่บนจอ
  if (rooms[ridx].name == g_gui_curRoom || rooms[ridx].name == DEFAULT_ROOM_NAME) {
      gui_add_chat_bubble(logSender.c_str(), senderUid, msg.c_str(), isSystem);
  }
}

bool approveRequestUid(int ridx, uint32_t uid, bool ok) {
  if (ridx < 0 || uid == 0) return false;

  // remove request
  int pos = -1;
  for (int i = 0; i < rooms[ridx].requestCount; i++) {
    if (rooms[ridx].requests[i] == uid) { pos = i; break; }
  }
  if (pos < 0) return false;

  for (int i = pos; i < rooms[ridx].requestCount - 1; i++)
    rooms[ridx].requests[i] = rooms[ridx].requests[i + 1];

  rooms[ridx].requestCount--;

  if (ok) {
    addMemberUid(ridx, uid);

    addChatMessage(
      ridx,
      "",
      0 ,
      getDisplayNameByUid(uid) + " has joined the group",
      true
    );

    // ===== LoRa APPROVE =====
    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.type = PKT_JOIN_APPROVE;
    pkt.fromNode = NODE_ID;
    pkt.msgId = millis();

    strncpy(pkt.senderName, "SYSTEM", 20);
    strncpy(pkt.target, rooms[ridx].name.c_str(), 24);

    // payload = uid
    snprintf(pkt.payload, sizeof(pkt.payload), "%lu", (unsigned long)uid);

    sendLoraPacket(pkt, 0xFF);
  }

  needSaveRooms = true;
  return true;
}


// เปลี่ยนจากรับ ownerIP เป็นรับ ownerUid (uint32_t)
int createRoomInternal(const String& name, uint32_t ownerUid, bool addOwnerAsMember) {
  if (!isValidRoomName(name)) return -1;

  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used) {
      rooms[i].used = true;
      rooms[i].name = name;
      rooms[i].ownerUid = ownerUid; // ใช้ค่าที่ส่งมาโดยตรง ชัวร์ที่สุด
      rooms[i].logHtml = "";
      rooms[i].memberUidCount = 0;
      rooms[i].requestCount = 0;

      // ล้างอาเรย์สมาชิกให้สะอาด
      memset(rooms[i].memberUids, 0, sizeof(rooms[i].memberUids));

      if (addOwnerAsMember && ownerUid != 0) {
        // เพิ่มเจ้าของกลุ่มเข้าไปในรายชื่อสมาชิกทันที
        rooms[i].memberUids[0] = ownerUid;
        rooms[i].memberUidCount = 1; // สำคัญมาก: ต้องตั้งเป็น 1 ไม่ใช่ 0
      }

      addChatMessage(i, "", 0, "Group created: " + name, true);
      needSaveRooms = true;
      return i;
    }
  }
  return -1;
}

bool addRequestUid(int ridx, uint32_t uid) {
  if (ridx < 0 || uid == 0) return false;
  // uint32_t uid = getUidFromIP(ip);
  // ถ้าเป็นสมาชิกแล้ว
  if (isMemberUid(ridx, uid)) return false;

  // กัน request ซ้ำ
  for (int i = 0; i < rooms[ridx].requestCount; i++) {
    if (rooms[ridx].requests[i] == uid) return false;
  }

  if (rooms[ridx].requestCount >= MAX_REQUESTS_PER_ROOM) return false;

  rooms[ridx].requests[rooms[ridx].requestCount++] = uid;
  needSaveRooms = true;
  return true;
}



// ===================== LoRa Helpers =====================
bool sendLoraPacket(LoraPacket& pkt, uint8_t dest) {
    // คง dest ไว้ เพื่อไม่ให้ logic เดิมพัง
    // (routing จริงใช้ pkt.target / pkt.fromNode อยู่แล้ว)

    pkt.fromNode = NODE_ID;
    pkt.ttl        = 3;        // ✅ ตั้ง TTL ทุกครั้งที่ส่งใหม่
    pkt.originNode = NODE_ID;  // ✅ จำต้นทางไว้

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pkt, sizeof(LoraPacket));
    int ok = LoRa.endPacket(true); // true = async   // 1 = ส่งสำเร็จ, 0 = fail

    return (ok == 1);
}


void broadcastUserStatus(const String& ip, const String& name) {
    if (millis() - lastBroadcastTime < 2000) return; // Prevent spamming
    lastBroadcastTime = millis();

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.type = PKT_HEARTBEAT;
    pkt.fromNode = NODE_ID;
    pkt.fromUserIp = getLastIpByte(ip);

    strncpy(pkt.senderName, name.c_str(), 20);
    pkt.senderName[20] = '\0';
    uint32_t uid = getUidFromIP(ip);
    // Send Gateway Status in Payload
    snprintf(pkt.payload, sizeof(pkt.payload),
         "UID:%lu|GW:%d",
         (unsigned long)uid,
         (WiFi.status() == WL_CONNECTED ? 1 : 0));


    // LoRa.h = broadcast อยู่แล้ว
    sendLoraPacket(pkt, 0xFF);
}

// ===================== Broadcast Join Room (UID-based) =====================
void broadcastJoinRoom(const String& myIP, const String& roomName) {
    uint32_t uid = getUidFromIP(myIP);
    if (uid == 0) return; // ยังไม่ login → ไม่ต้อง sync

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.type = PKT_JOIN_NOTIFY;
    pkt.fromNode = NODE_ID;
    pkt.fromUserIp = getLastIpByte(myIP); // legacy / topology
    pkt.msgId = millis();

    // senderName: ใช้เพื่อแสดงผล UI เก่า
    String dn = getDisplayNameByUid(uid);
    strncpy(pkt.senderName, dn.c_str(), 20);
    pkt.senderName[20] = '\0';

    // target = room name
    strncpy(pkt.target, roomName.c_str(), 24);
    pkt.target[24] = '\0';

    // payload = UID (หัวใจของการ sync ห้องตามตัวตน)
    snprintf(pkt.payload, sizeof(pkt.payload), "%lu", (unsigned long)uid);

    // broadcast ไปทุก node
    sendLoraPacket(pkt, 0xFF);
}


// ===================== DM (Internal, UID-based) =====================
String makeDMRoomName(uint32_t uid1, uint32_t uid2) {
  if (uid1 < uid2)
    return String("DM:") + uid1 + "|" + uid2;
  else
    return String("DM:") + uid2 + "|" + uid1;
}

/*int ensureDMRoomUid(const String& myIP, const String& otherIP) {
  uint32_t myUid    = getUidFromIP(myIP);
  uint32_t otherUid = getUidFromIP(otherIP);

  if (myUid == 0 || otherUid == 0) return -1;

  String dmName = makeDMRoomName(myUid, otherUid);
  int ridx = findRoom(dmName);

  if (ridx < 0) {
    ridx = createRoomInternal(dmName, myUid, false);
    if (ridx < 0) return -1;

    rooms[ridx].ownerUid = myUid;     // ⭐ owner เป็น UID
    rooms[ridx].logHtml = "";
  }

  // ผูกสมาชิกด้วย UID
  addMemberUid(ridx, myUid);
  addMemberUid(ridx, otherUid);

  // เก็บ IP ไว้เพื่อ routing (optional)
  addMember(ridx, myIP);
  addMember(ridx, otherIP);

  needSaveRooms = true;
  return ridx;
}*/

int ensureDMRoomUidByUid(uint32_t myUid, uint32_t otherUid) {
  if (myUid == 0 || otherUid == 0) return -1;

  String dmName = makeDMRoomName(myUid, otherUid);
  int ridx = findRoom(dmName);

  if (ridx < 0) {
    ridx = createRoomInternal(dmName, myUid, false);
    if (ridx < 0) return -1;
  }

  addMemberUid(ridx, myUid);
  addMemberUid(ridx, otherUid);

  needSaveRooms = true;
  return ridx;
}



// ===================== Invites =====================
bool addInviteToUser(uint32_t toUid, const String& roomName, uint32_t inviterUid) {
    if (toUid == 0 || inviterUid == 0) return false;
    int uidx = findUserByUid(toUid);
    if (uidx < 0) return false;   // ผู้รับยังไม่เคย login node นี้
    // กัน invite ซ้ำ
    for (int i = 0; i < MAX_INVITES_PER_USER; i++) {
        if (users[uidx].invites[i].used &&
            users[uidx].invites[i].roomName == roomName &&
            users[uidx].invites[i].inviterUid == inviterUid) {
            return false;
        }
    }

    for (int i = 0; i < MAX_INVITES_PER_USER; i++) {
        if (!users[uidx].invites[i].used) {
            users[uidx].invites[i].used = true;
            users[uidx].invites[i].roomName = roomName;
            users[uidx].invites[i].inviterUid = inviterUid;
            return true;
        }
    }
    return false;
}


bool removeInvite(const String& myIP, int inviteIndex) {
  int uidx = findUserByIP(myIP);
  if (uidx < 0) return false;
  if (inviteIndex < 0 || inviteIndex >= MAX_INVITES_PER_USER) return false;

  users[uidx].invites[inviteIndex].used = false;
  users[uidx].invites[inviteIndex].roomName = "";
  users[uidx].invites[inviteIndex].inviterUid = 0;
  return true;
}


bool acceptInvite(const String& myIP, int inviteIndex) {
  int uidx = findUserByIP(myIP);
  if (uidx < 0) return false;
  if (inviteIndex < 0 || inviteIndex >= MAX_INVITES_PER_USER) return false;
  if (!users[uidx].invites[inviteIndex].used) return false;

  uint32_t myUid = users[uidx].uid;
  if (myUid == 0) return false;

  String roomName = users[uidx].invites[inviteIndex].roomName;
  uint32_t inviterUid = users[uidx].invites[inviteIndex].inviterUid;

  int ridx = findRoom(roomName);
  if (ridx < 0) {
    uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
    ridx = createRoomInternal(roomName, adminUid, false);
    if (ridx < 0) {
      removeInvite(myIP, inviteIndex);
      return false;
    }
  }

  // 🔑 เพิ่มสมาชิกด้วย UID (หัวใจ)
  bool ok = addMemberUid(ridx, myUid);
  removeInvite(myIP, inviteIndex);

  if (ok) {
    String myName = getDisplayNameByUid(myUid);
    String inviterName = getDisplayNameByUid(inviterUid);

    addChatMessage(
      ridx,
      "", 0, 
      myName + " joined via invitation from " + inviterName,
      true
    );

    // 🔥 broadcast join ด้วย UID (ใช้ฟังก์ชันที่เธอแก้แล้ว)
    broadcastJoinRoom(myIP, roomName);
  }

  needSaveRooms = true;
  return ok;
}


// ===================== Stats =====================
int countOnlineUsers() {
  int c = 0;
  bool counted[MAX_USERS] = {false};

  for (int i = 0; i < MAX_USERS; i++) {
    if (!users[i].used) continue;
    uint32_t uid = users[i].uid;
    if (uid == 0) continue;

    // กันนับซ้ำ UID เดิม
    bool already = false;
    for (int j = 0; j < i; j++) {
      if (users[j].used && users[j].uid == uid) {
        already = true;
        break;
      }
    }
    if (already) continue;

    if (isUidOnline(uid)) c++;
  }
  return c;
}

int countOfflineUsers() {
  int c = 0;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].used && users[i].isLoggedIn && !isUserOnline(users[i])) c++;
  }
  return c;
}

// 5. แก้ไข API Ping: หน้าเว็บจะเรียกตัวนี้ตลอดเวลาที่เปิดค้างไว้
void handlePing(WiFiClient& client, const String& myIP) {
  upsertUserSeen(myIP, true); // อัปเดตว่ายังเปิดหน้าเว็บอยู่ (Online)
  ensureDefaultGroupAndMembership(myIP);
  httpOKText(client, "OK");
}

void handleApiStats(WiFiClient& client, const String& myIP) {
  upsertUserSeen(myIP, true);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);

  int totalUnread = 0;
  if (myUid != 0) {
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (rooms[i].used && isMemberUid(i, myUid)) totalUnread += rooms[i].unreadCount;
    }
  }

  String json = "{";
  json += "\"online\":" + String(countOnlineUsers()) + ",";
  json += "\"offline\":" + String(countOfflineUsers()) + ",";
  json += "\"unread\":" + String(totalUnread) + ",";
  json += "\"meName\":\"" + htmlEscape(getDisplayName(myIP)) + "\"";
  json += "}";

  httpOKJson(client, json);
}

// 6. แก้ไข User List: แสดงจุดเขียว/เทา ตามสถานะจริง
void handleApiUserList(WiFiClient& client, const String& myIP) {
  upsertUserSeen(myIP, true);
  uint32_t myUid = getUidFromIP(myIP);
  String json = "{\"users\":[";
  bool first = true;

  for (int i = 0; i < MAX_USERS; i++) {
    if (!users[i].used) continue;
    if (users[i].uid == 0 || users[i].uid == myUid) continue;
    if (!users[i].isLoggedIn) continue;

    String dName = getDisplayNameByUid(users[i].uid);
    if (dName == "SYSTEM" || dName == "System") continue;

    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"uid\":" + String(users[i].uid) + ",";
    json += "\"name\":\"" + htmlEscape(dName) + "\",";
    json += "\"online\":" + String(isUidOnline(users[i].uid) ? "true" : "false");
    json += "}";
  }
  json += "]}";
  httpOKJson(client, json);
}

void handleApiInvites(WiFiClient& client, const String& myIP) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  int uidx = findUserByIP(myIP);
  uint32_t myUid = getUidFromIP(myIP);
  int dbIdx = findDbUserByUid(myUid);
  String json = "{\"invites\":[";
  bool first = true;
  
  if (uidx >= 0) {
    for (int i = 0; i < MAX_INVITES_PER_USER; i++) {
      if (!users[uidx].invites[i].used) continue;

      if (!first) json += ",";
      first = false;

      uint32_t inviterUid = users[uidx].invites[i].inviterUid;
      String inviterName =
        (inviterUid != 0)
          ? getDisplayNameByUid(inviterUid)
          : "Unknown";

      json += "{";
      json += "\"idx\":" + String(i) + ",";
      json += "\"room\":\"" + htmlEscape(users[uidx].invites[i].roomName) + "\",";
      json += "\"inviter\":\"" + htmlEscape(inviterName) + "\"";
      json += "}";
    }
  }

  json += "]}";
  httpOKJson(client, json);
}


void handleApiRooms(WiFiClient& client, const String& myIP) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);

  // ถ้าหา UID ไม่เจอ ให้ลองดึงจากโครงสร้าง users โดยตรง
  if (myUid == 0) {
      int uidx = findUserByIP(myIP);
      if (uidx >= 0) myUid = users[uidx].uid;
  }

  String json = "{\"rooms\":[";
  bool first = true;

  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].used) continue;
    
    // กรองเอาเฉพาะกลุ่มปกติ (ไม่ใช่ DM) มาโชว์ที่หน้ารายการ Chat
    if (isDmRoomName(rooms[i].name)) continue;

    String status = "none";
    
    // 2. ถ้าไม่ใช่เจ้าของ ค่อยเช็คว่าเป็นสมาชิกไหม
    if (rooms[i].ownerUid == myUid && myUid != 0) {
        status = "member"; 
    } else if (isMemberUid(i, myUid)) {
        status = "member";
    } else {
      for (int k = 0; k < rooms[i].requestCount; k++) {
        if (rooms[i].requests[k] == myUid) { status = "pending"; break; }
      }
    }

    bool isHost = (rooms[i].ownerUid == myUid && myUid != 0);
    bool canDelete = isHost && (rooms[i].name != DEFAULT_ROOM_NAME);
    bool canLeave = (status == "member") && (rooms[i].name != DEFAULT_ROOM_NAME);

    if (!first) json += ",";
    first = false;

    json += "{";
    json += "\"name\":\"" + htmlEscape(rooms[i].name) + "\",";
    json += "\"members\":" + String(rooms[i].memberUidCount) + ",";
    json += "\"owner_uid\":" + String(rooms[i].ownerUid) + ",";
    json += "\"status\":\"" + status + "\",";
    json += "\"isHost\":" + String(isHost ? "true" : "false") + ",";
    json += "\"canDelete\":" + String(canDelete ? "true" : "false") + ",";
    json += "\"canLeave\":" + String(canLeave ? "true" : "false") + ",";
    json += "\"unread\":" + String(rooms[i].unreadCount);
    json += "}";
  }

  json += "]}";
  httpOKJson(client, json);
}


void handleApiRoom(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  String rn = getQueryParam(path, "name");
  int ridx = findRoom(rn);

  if (ridx < 0) {
    httpOKJson(client, "{\"members\":[],\"requests\":[],\"inviteCandidates\":[]}");
    return;
  }

  bool member = isMemberUid(ridx, myUid);
  bool isDM = isDmRoomName(rn);

  String json = "{";

  // members
  json += "\"members\":[";
  for (int i = 0; i < rooms[ridx].memberUidCount; i++) {
    uint32_t uid = rooms[ridx].memberUids[i];
    if (i > 0) json += ",";
    json += "{";
    json += "\"uid\":" + String(uid) + ",";
    json += "\"name\":\"" + htmlEscape(getDisplayNameByUid(uid)) + "\",";
    json += "\"online\":" + String(isUidOnline(uid) ? "true" : "false");
    json += "}";
  }
  json += "],";

  // requests
  json += "\"requests\":[";
  if (member && !isDM) {
    for (int i = 0; i < rooms[ridx].requestCount; i++) {
      if (i > 0) json += ",";
      uint32_t uid = rooms[ridx].requests[i];
      json += "{";
      json += "\"uid\":" + String(uid) + ",";
      json += "\"name\":\"" + htmlEscape(getDisplayNameByUid(uid)) + "\"";
      json += "}";
    }
  }
  json += "],";

  // invite candidates
  json += "\"inviteCandidates\":[";
  if (member && !isDM) {
    bool first2 = true;
    for (int i = 0; i < dbUserCount; i++) {
      uint32_t uid = dbUsers[i].uid;
      if (uid == 0) continue;
      if (uid == myUid) continue;
      if (isMemberUid(ridx, uid)) continue;

      String name = getDisplayNameByUid(uid);
      if (name == "SYSTEM" || name == "System") continue;

      if (!first2) json += ",";
      first2 = false;

      json += "{";
      json += "\"uid\":" + String(uid) + ",";
      json += "\"name\":\"" + htmlEscape(name) + "\"";
      json += "}";
    }
  }
  json += "]";

  json += "}";

  httpOKJson(client, json);
}


void handleApiLog(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpFailText(client, 403, "NOLOGIN"); return; }

  String rn = getQueryParam(path, "name");
  int ridx = findRoom(rn);
  if (ridx < 0) { httpFailText(client, 404, "NF"); return; }

  if (!isMemberUid(ridx, myUid)) {
    httpFailText(client, 403, "NO");
    return;
  }

  rooms[ridx].unreadCount = 0;
  needSaveRooms = true;

  httpOKHtml(client, rooms[ridx].logHtml);
}


// ===================== Actions =====================
// [RESET DB] เพิ่มฟังก์ชันล้างข้อมูล
void handleFactoryReset(WiFiClient& client, const String& myIP, const String& path) {
  LittleFS.remove(DB_FILE);    // ลบ User
  LittleFS.remove(ROOMS_FILE); // ลบแชทและห้อง
  
  httpOKText(client, "RESET OK. Rebooting...");
  delay(1000);
  ESP.restart();
}

void handleSetName(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t uid = getUidFromIP(myIP);
  if (uid == 0) { httpRedirect(client, "/"); return; }

  String nm = getQueryParam(path, "name");
  if (!isValidDisplayName(nm)) {
    httpRedirect(client, "/");
    return;
  }

  setDisplayNameByUid(uid, nm);
  httpRedirect(client, "/");
}


void handleCreateRoom(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) {
    String uidParam = getQueryParam(path, "owner_uid");
    if (uidParam.length() > 0) myUid = (uint32_t)uidParam.toInt();
  }
  if (myUid == 0) { httpFailText(client, 403, "NOLOGIN"); return; }

  String rn = getQueryParam(path, "name");
  if (!isValidRoomName(rn)) {
    httpFailText(client, 400, "BAD");
    return;
  }

  if (findRoom(rn) >= 0) {
    httpFailText(client, 409, "DUP");
    return;
  }

  if (!hasFreeRoomSlot()) {
    httpFailText(client, 409, "FULL");
    return;
  }

  int ridx = createRoomInternal(rn, myUid, true);
  if (ridx < 0) {
    httpFailText(client, 500, "FAIL");
    return;
  }

  // ผูก owner ด้วย UID
  //rooms[ridx].ownerUid = myUid;
  //rooms[ridx].memberUidCount = 0;
  //addMemberUid(ridx, myUid);
  needSaveRooms = true;

  // Broadcast group created
  LoraPacket pkt;
  memset(&pkt, 0, sizeof(pkt));

  pkt.type = PKT_CHAT_GROUP;
  pkt.fromNode = NODE_ID;
  pkt.fromUserIp = getLastIpByte(myIP);
  pkt.msgId = millis();

  strncpy(pkt.senderName, "SYSTEM", 20);
  strncpy(pkt.payload, "New Group Created", 99);
  strncpy(pkt.target, rn.c_str(), 24);

  sendLoraPacket(pkt, 0xFF);

  httpOKText(client, "CREATED");
}


void handleDeleteRoom(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpRedirect(client, "/"); return; }

  String rn = getQueryParam(path, "name");
  int ridx = findRoom(rn);
  if (ridx < 0) { httpRedirect(client, "/"); return; }

  if (rooms[ridx].ownerUid != myUid) { httpRedirect(client, "/"); return; }
  if (rooms[ridx].name == DEFAULT_ROOM_NAME) { httpRedirect(client, "/"); return; }

  rooms[ridx].used = false;
  rooms[ridx].name = "";
  rooms[ridx].ownerUid = 0;
  rooms[ridx].logHtml = "";
  rooms[ridx].memberCount = 0;
  rooms[ridx].memberUidCount = 0;
  rooms[ridx].requestCount = 0;

  needSaveRooms = true;
  httpRedirect(client, "/");
}


void handleLeaveRoom(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpRedirect(client, "/"); return; }

  String rn = getQueryParam(path, "name");
  if (rn == DEFAULT_ROOM_NAME && !ALLOW_LEAVE_DEFAULT) {
    httpRedirect(client, "/");
    return;
  }

  int ridx = findRoom(rn);
  if (ridx < 0) { httpRedirect(client, "/"); return; }

  removeMemberUid(ridx, myUid);

  addChatMessage(
    ridx,
    "", 0,
    getDisplayNameByUid(myUid) + " left the group",
    true
  );

  needSaveRooms = true;
  httpRedirect(client, "/");
}

void handleRequestJoin(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpRedirect(client, "/"); return; }

  String rn = getQueryParam(path, "name");
  int ridx = findRoom(rn);
  if (ridx < 0) { httpRedirect(client, "/"); return; }

  if (rooms[ridx].name == DEFAULT_ROOM_NAME) {
    addMemberUid(ridx, myUid);
    needSaveRooms = true;
    httpRedirect(client, String("/room?name=") + urlEncode(rn));
    return;
  }

  addRequestUid(ridx, myUid);

  LoraPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = PKT_JOIN_REQ;
  pkt.fromNode = NODE_ID;
  pkt.msgId = millis();

  strncpy(pkt.senderName, getDisplayNameByUid(myUid).c_str(), 20);
  snprintf(pkt.payload, sizeof(pkt.payload), "%lu", (unsigned long)myUid);
  strncpy(pkt.target, rn.c_str(), 24);

  sendLoraPacket(pkt, 0xFF);
  httpRedirect(client, "/");
}



void handleApprove(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpRedirect(client, "/"); return; }

  String rn = getQueryParam(path, "name");
  uint32_t targetUid = getQueryParam(path, "uid").toInt();
  bool ok = (getQueryParam(path, "ok") == "1");

  int ridx = findRoom(rn);
  if (ridx < 0) { httpRedirect(client, "/"); return; }
  if (rooms[ridx].ownerUid != myUid) {
    httpRedirect(client, String("/room?name=") + urlEncode(rn));
    return;
  }

  approveRequestUid(ridx, targetUid, ok);
  httpRedirect(client, String("/room?name=") + urlEncode(rn));
}


void handleSendMsg(WiFiClient& client, const String& myIP, const String& path) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  uint32_t myUid = getUidFromIP(myIP);
  if (myUid == 0) { httpRedirect(client, "/"); return; }

  String rn = getQueryParam(path, "name");
  String msg = getQueryParam(path, "msg");
  int ridx = findRoom(rn);

  if (ridx < 0) { httpRedirect(client, "/"); return; }
  if (!isMemberUid(ridx, myUid)) {
    httpRedirect(client, String("/room?name=") + urlEncode(rn));
    return;
  }

  // บันทึกลง Log ในเครื่องตัวเอง (เอาเฉพาะข้อความ msg เพียวๆ)
  addChatMessage(ridx, getDisplayNameByUid(myUid), myUid, msg, false);

  LoraPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type = isDmRoomName(rn) ? PKT_CHAT_DM : PKT_CHAT_GROUP;
  pkt.fromNode = NODE_ID;
  pkt.msgId = millis();

  strncpy(pkt.senderName, getDisplayNameByUid(myUid).c_str(), 20);
  // แพ็ก payload ในรูปแบบ "UID|MESSAGE"
  snprintf(pkt.payload, sizeof(pkt.payload), "%lu|%s", (unsigned long)myUid, msg.c_str());
  strncpy(pkt.target, rn.c_str(), 24);

  sendLoraPacket(pkt, 0xFF);
  httpOKText(client, "OK");
}


void handleInvite(WiFiClient& client, const String& myIP, const String& path) {
    upsertUserSeen(myIP);
    ensureDefaultGroupAndMembership(myIP);

    uint32_t myUid = getUidFromIP(myIP);
    if (myUid == 0) { httpFailText(client, 403, "LOGIN"); return; }

    String rn = getQueryParam(path, "name");
    // รับค่าเป็น String ก่อนแล้วค่อยแปลงเป็น uint32_t
    String toStr = getQueryParam(path, "to");
    uint32_t targetUid = (uint32_t)strtoul(toStr.c_str(), NULL, 10);

    int ridx = findRoom(rn);
    if (ridx < 0) { httpFailText(client, 400, "BAD"); return; }
    if (!isMemberUid(ridx, myUid)) { httpFailText(client, 403, "NO"); return; }
    if (isMemberUid(ridx, targetUid)) { httpFailText(client, 409, "IN"); return; }

    // เรียกใช้ฟังก์ชันที่แก้ใหม่ (ส่ง UID แทน IP)
    addInviteToUser(targetUid, rn, myUid);

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_INVITE;
    pkt.fromNode = NODE_ID;
    strncpy(pkt.target, rn.c_str(), 24);
    snprintf(pkt.payload, sizeof(pkt.payload), "%lu", (unsigned long)targetUid);

    sendLoraPacket(pkt, 0xFF);

    addChatMessage(ridx, "", 0,
        getDisplayNameByUid(myUid) + " invited " + getDisplayNameByUid(targetUid),
        true);

    httpOKText(client, "INVITED");
}


void handleInviteAccept(WiFiClient& client, const String& myIP, const String& path, bool accept) {
  upsertUserSeen(myIP);
  ensureDefaultGroupAndMembership(myIP);

  int idx = getQueryParam(path, "idx").toInt();
  if (accept) acceptInvite(myIP, idx);
  else removeInvite(myIP, idx);
  httpRedirect(client, "/");
}

void handleChat(WiFiClient& client, const String& myIP, const String& path) {
    uint32_t myUid = getUidFromIP(myIP);
    String toStr = getQueryParam(path, "to");
    uint32_t otherUid = (uint32_t)strtoul(toStr.c_str(), NULL, 10);
    
    if (myUid == 0 || otherUid == 0) {
        httpRedirect(client, "/");
        return;
    }

    // แก้ปัญหา Ambiguous โดยแปลง UID เป็น String ให้ชัดเจน
    int ridx = ensureDMRoomUidByUid(myUid, otherUid);

    
    if (ridx < 0) {
        httpRedirect(client, "/");
        return;
    }

    httpRedirect(client, String("/room?name=") + urlEncode(rooms[ridx].name));
}

struct RelayEntry {
  LoraPacket pkt;
  uint32_t sendAt;
  bool used = false;
};
RelayEntry relayQueue[4];

void enqueueRelay(const LoraPacket& pkt) {
  for (int i = 0; i < 4; i++) {
    if (!relayQueue[i].used) {
      relayQueue[i].pkt    = pkt;
      relayQueue[i].sendAt = millis() + random(30, 150);
      relayQueue[i].used   = true;
      return;
    }
  }
}

void processRelayQueue() {
  for (int i = 0; i < 4; i++) {
    if (!relayQueue[i].used) continue;
    if (millis() < relayQueue[i].sendAt) continue;
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&relayQueue[i].pkt, sizeof(LoraPacket));
    LoRa.endPacket(true); // async non-blocking
    relayQueue[i].used = false;
  }
}

void checkLoRa() {
  int packetSize = LoRa.parsePacket();
  if (!packetSize) return;
  
  // 1. ตรวจสอบขนาดและอ่านข้อมูล
  if (packetSize != sizeof(LoraPacket)) {
    while (LoRa.available()) LoRa.read();
    return;
  }

  LoraPacket pkt;
  LoRa.readBytes((uint8_t*)&pkt, sizeof(LoraPacket));

  // 2. ระบบป้องกัน Packet ซ้ำ (Deduplication)
// ✅ แทนด้วย seen list (กัน loop ได้จริง)
// ✅ แทนด้วย seen list (กัน loop ได้จริง)
    struct SeenEntry { uint32_t msgId; uint8_t originNode; };
    static SeenEntry seen[64];
    static int seenIdx = 0;
    for (int i = 0; i < 64; i++) {
      if (seen[i].msgId == pkt.msgId && seen[i].originNode == pkt.originNode) return;
    }
    seen[seenIdx % 64] = {pkt.msgId, pkt.originNode};
    seenIdx++;
  int rssi = LoRa.packetRssi();

  // 3. อัปเดต Topology (Mesh Map)
  topology[pkt.fromNode].active = true;
  topology[pkt.fromNode].rssi = rssi;
  topology[pkt.fromNode].lastHeard = millis();

  // 4. จัดการ Heartbeat (อัปเดตสถานะออนไลน์โหนดอื่น)
  if (pkt.type == PKT_HEARTBEAT) {
    topology[pkt.fromNode].isGateway = (String(pkt.payload).startsWith("GW:1"));
    String remoteIP = makeRemoteIP(pkt.fromNode, pkt.fromUserIp);
    int uidx = upsertUserSeen(remoteIP);
    if (uidx >= 0) {
      users[uidx].isRemote = true;
      users[uidx].lastSeen = millis();
      users[uidx].isLoggedIn = true;
      users[uidx].fromNode = pkt.fromNode; // จำว่าเพื่อนคนนี้อยู่โหนดไหน
        
      String pl = String(pkt.payload);

      // parse UID
      int uPos = pl.indexOf("UID:");
      if (uPos >= 0) {
        int uEnd = pl.indexOf('|', uPos);
        String uidStr = (uEnd > uPos)
                          ? pl.substring(uPos + 4, uEnd)
                          : pl.substring(uPos + 4);
        users[uidx].uid = (uint32_t)uidStr.toInt();
      }

      // parse GW (ถ้าต้องใช้)
      topology[pkt.fromNode].isGateway = pl.indexOf("GW:1") >= 0;
    }
     // ✅ เพิ่มตรงนี้ — relay Heartbeat ให้โหนดที่ไม่ได้ยินกันโดยตรง
    if (pkt.ttl > 0) {
      pkt.ttl--;
      pkt.fromNode = NODE_ID;
      enqueueRelay(pkt);
    }
    
    return;
  }

  // 5. เตรียม Parsing ข้อมูลแชท (UID|Msg)
  String fullPayload = String(pkt.payload);
  uint32_t incomingUid = 0;
  String finalMsg = "";
  int sep = fullPayload.indexOf('|');
  if (sep >= 0) {
    incomingUid = (uint32_t)fullPayload.substring(0, sep).toInt();
    finalMsg = fullPayload.substring(sep + 1);
  } else {
    incomingUid = (uint32_t)fullPayload.toInt();
    finalMsg = fullPayload; 
  }

  // 6. ประมวลผลตามประเภท Packet
  if (pkt.type == PKT_CHAT_GROUP) {
    String roomName = pkt.target;
    int ridx = findRoom(roomName);

    // ตรวจสอบว่าเป็น Packet สำหรับ Sync ข้อมูลห้องหรือไม่
    if (finalMsg.startsWith("SYNC_ROOM|")) {
      uint32_t ownerUid = (uint32_t)finalMsg.substring(10).toInt();
      if (ridx < 0) {
        uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
        ridx = createRoomInternal(roomName, adminUid, false);
      }
      if (ridx >= 0) {
        rooms[ridx].ownerUid = ownerUid;
        needSaveRooms = true;
      }
      return; // จบงาน ไม่บันทึกเป็นแชท
    }

    // กรณีเป็นข้อความแชทปกติ
    if (ridx < 0 && !isDmRoomName(roomName)) {
      uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
      ridx = createRoomInternal(roomName, adminUid, false);
    }
    if (ridx >= 0 && incomingUid != 0) {
      addMemberUid(ridx, incomingUid);
      addChatMessage(ridx, getDisplayNameByUid(incomingUid), incomingUid, finalMsg, false);
    }
  } 
  else if (pkt.type == PKT_CHAT_DM) {
    int ridx = findRoom(pkt.target);
    if (ridx >= 0 && incomingUid != 0) {
      addMemberUid(ridx, incomingUid);
      addChatMessage(ridx, getDisplayNameByUid(incomingUid), incomingUid, finalMsg, false);
    }
  }
  else if (pkt.type == PKT_JOIN_REQ) {
    // request join เท่านั้น → pending
    int ridx = findRoom(pkt.target);
    if (ridx >= 0 && incomingUid != 0) {
      addRequestUid(ridx, incomingUid);
    }
  }
  else if (pkt.type == PKT_INVITE) {
    // invite = ไม่แตะ room status ใด ๆ
    // handled via users[].invites เท่านั้น
  }
  else if (pkt.type == PKT_JOIN_NOTIFY) {
    int ridx = findRoom(pkt.target);
    if (ridx >= 0 && incomingUid != 0) {
      addMemberUid(ridx, incomingUid);
      addChatMessage(ridx, "", 0, getDisplayNameByUid(incomingUid) + " has joined the group", true);
    }
  }
  else if (pkt.type == PKT_JOIN_APPROVE) {
    int ridx = findRoom(pkt.target);
    if (ridx < 0) {
      uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
      ridx = createRoomInternal(pkt.target, adminUid, false);
    }
    if (ridx >= 0 && incomingUid != 0) addMemberUid(ridx, incomingUid);
  }
  else if (pkt.type == PKT_SYNC_USER_FULL) {
    int p1 = fullPayload.indexOf('|');
    int p2 = fullPayload.indexOf('|', p1 + 1);
    int p3 = fullPayload.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) return;

    uint32_t suid = fullPayload.substring(0, p1).toInt();
    String username = fullPayload.substring(p1 + 1, p2);
    String password = fullPayload.substring(p2 + 1, p3);
    String displayName = fullPayload.substring(p3 + 1);

    int idx = findDbUserByUid(suid);
    if (idx < 0) {
      registerUser(username.c_str(), password.c_str(), suid, false);
      idx = findDbUserByUid(suid);
    }
    if (idx >= 0) {
      strncpy(dbUsers[idx].displayName, displayName.c_str(), 20);
      needSaveDb = true;
      Serial.println("[SYNC USER] OK: " + username);
    
        // ✅ Mesh Relay — non-blocking
    if (pkt.ttl > 0 && pkt.type != PKT_HEARTBEAT) {
      pkt.ttl--;
      pkt.fromNode = NODE_ID;
      enqueueRelay(pkt);
    }
    }
  }
}

void broadcastRoomInfo(int ridx) {
    if (ridx < 0 || !rooms[ridx].used) return;

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_CHAT_GROUP; // ใช้ Type เดิมแต่ส่งข้อความ System เพื่อประกาศตัวตนกลุ่ม
    pkt.fromNode = NODE_ID;
    pkt.msgId = millis();
    
    strncpy(pkt.senderName, "SYSTEM", 20);
    strncpy(pkt.target, rooms[ridx].name.c_str(), 24);
    // Payload บอกว่านี่คือการ Sync ข้อมูลกลุ่ม (Owner UID)
    snprintf(pkt.payload, sizeof(pkt.payload), "SYNC_ROOM|%lu", (unsigned long)rooms[ridx].ownerUid);

    sendLoraPacket(pkt, 0xFF);
}

// ===================== Periodic Functions =====================
void sendHeartbeats() {
    if (millis() - lastHeartbeatTime <= HEARTBEAT_INTERVAL) return;
    lastHeartbeatTime = millis();

    bool imGateway = (WiFi.status() == WL_CONNECTED);

    // --- ส่วนที่ 1: Heartbeat แบบ state machine ทีละ 1 user ต่อ loop ---
    // ใช้ static เพื่อจำตำแหน่งที่ค้างไว้ ไม่ส่งทั้งหมดในครั้งเดียว
    static int hbUserIdx = 0;
    static unsigned long lastHbSend = 0;

    // ส่งทีละ 1 packet ทุก 50ms โดยไม่ใช้ delay()
    if (millis() - lastHbSend >= 50) {
        // หาคนต่อไปที่ต้องส่ง heartbeat
        int checked = 0;
        while (checked < MAX_USERS) {
            int i = hbUserIdx % MAX_USERS;
            hbUserIdx++;
            checked++;
            if (users[i].used && !users[i].isRemote && isUserOnline(users[i])) {
                LoraPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.type = PKT_HEARTBEAT;
                pkt.fromNode = NODE_ID;
                pkt.fromUserIp = getLastIpByte(users[i].ip);
                pkt.msgId = millis();
                strncpy(pkt.senderName, getDisplayName(users[i].ip).c_str(), 20);
                snprintf(pkt.payload, sizeof(pkt.payload),
                    "UID:%lu|GW:%d",
                    (unsigned long)users[i].uid,
                    imGateway ? 1 : 0);

                LoRa.beginPacket();
                LoRa.write((uint8_t*)&pkt, sizeof(LoraPacket));
                LoRa.endPacket(true); // async non-blocking — ไม่มี delay แล้ว!
                lastHbSend = millis();
                break; // ส่งแค่ 1 คน แล้วออก loop ให้ LVGL ได้รัน
            }
        }
    }

    // --- ส่วนที่ 2: Background Sync (User & Rooms) ทีละ 1 รายการ ---
    // 2.1 Sync ข้อมูล User 1 คน
    if (dbUserCount > 0) {
        static int syncUserIdx = 0;
        if (syncUserIdx >= dbUserCount) syncUserIdx = 0;
        broadcastUserFull(dbUsers[syncUserIdx].uid);
        syncUserIdx++;
    }

    // 2.2 Sync ข้อมูลกลุ่ม 1 กลุ่ม
    static int syncRoomIdx = 0;
    for (int j = 0; j < MAX_ROOMS; j++) {
        int r = (syncRoomIdx + j) % MAX_ROOMS;
        if (rooms[r].used && !isDmRoomName(rooms[r].name)) {
            LoraPacket rpkt;
            memset(&rpkt, 0, sizeof(rpkt));
            rpkt.type = PKT_CHAT_GROUP;
            rpkt.fromNode = NODE_ID;
            rpkt.msgId = millis();
            strncpy(rpkt.senderName, "SYSTEM", 20);
            strncpy(rpkt.target, rooms[r].name.c_str(), 24);
            snprintf(rpkt.payload, sizeof(rpkt.payload), "SYNC_ROOM|%lu", (unsigned long)rooms[r].ownerUid);

            LoRa.beginPacket();
            LoRa.write((uint8_t*)&rpkt, sizeof(LoraPacket));
            LoRa.endPacket(true); // async non-blocking

            syncRoomIdx = (r + 1) % MAX_ROOMS;
            break;
        }
    }
}

void reportToComputer() {
  if (WiFi.status() == WL_CONNECTED && millis() - lastReportTime > 3000) {
    lastReportTime = millis();
    HTTPClient http;
    http.setTimeout(800);          // ✅ FIX: timeout 800ms แทนที่จะรอ default ~5 วินาที
    http.setConnectTimeout(500);   // ✅ FIX: connect timeout 500ms
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"gatewayId\":" + String(NODE_ID) + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\","; // ส่ง IP ของโหนดไปเพื่อให้ Server สั่งการกลับได้
    
    // ===== โครงสร้าง Network (Topology & Neighbors) =====
    json += "\"nodes\":[";
    // เพิ่มตัวเองเป็นโหนดหลัก
    json += "{\"id\":" + String(NODE_ID) + ",\"rssi\":0,\"isGw\":true,\"status\":\"online\",\"nbs\":[]}";
    
    for(int i=0; i<255; i++) {
      if(topology[i].active) {
        bool isOnline = (millis() - topology[i].lastHeard < 20000);
        json += ",{";
        json += "\"id\":" + String(i) + ",";
        json += "\"rssi\":" + String(topology[i].rssi) + ",";
        json += "\"isGw\":" + String(topology[i].isGateway ? "true" : "false") + ",";
        json += "\"status\":\"" + String(isOnline ? "online" : "offline") + "\",";
        json += "\"nbs\":[" + String(NODE_ID) + "]"; // แสดงว่าโหนดนี้เชื่อมต่อกับ Gateway
        json += "}";
      }
    }
    json += "],";

    // ===== ข้อมูล User ทั้งหมดใน Database (จัดการได้จากคอม) =====
    json += "\"dbUsers\":[";
    for(int i=0; i < MAX_USERS; i++) {
      if(!users[i].used) continue;
      if (i > 0) json += ",";
      json += "{";
      json += "\"uid\":" + String(users[i].uid) + ",";
      json += "\"u\":\"" + String(users[i].username) + "\",";
      json += "\"isLoggedIn\":" + String(users[i].isLoggedIn ? "true" : "false");
      json += "}";
    }
    json += "],";

    // ===== ข้อมูลกลุ่ม/ห้องแชท (Rooms) =====
    json += "\"rooms\":[";
    bool firstRoom = true;
    for(int i=0; i < MAX_ROOMS; i++) {
      if(!rooms[i].used) continue;
      if (!firstRoom) json += ",";
      firstRoom = false;
      json += "{\"name\":\"" + rooms[i].name + "\",\"owner\":" + String(rooms[i].ownerUid) + "}";
    }
    json += "]";

    json += "}";

    http.POST(json);
    http.end();
  }
}

void handleRemoteAdmin(WiFiClient& client, const String& path) {
  // ตรวจสอบว่าเป็นคำสั่งลบ User หรือไม่
  if (path.startsWith("/admin/delete_user?")) {
    uint32_t targetUid = (uint32_t)getQueryParam(path, "uid").toInt();
    int idx = findDbUserByUid(targetUid);
    
    if (idx >= 0) {
      String uname = String(dbUsers[idx].username);
      deleteUserByName(uname.c_str()); // ใช้ฟังก์ชันเดิมที่มีอยู่
      
      // ลบ Session ของ User คนนั้นทิ้งด้วยถ้าเขายังเกาะโหนดนี้อยู่
      int uidx = findUserByUid(targetUid);
      if (uidx >= 0) users[uidx].isLoggedIn = false;

      httpOKText(client, "DELETED_SUCCESS");
    } else {
      httpFailText(client, 404, "USER_NOT_FOUND");
    }
  }
}

void discoverServer() {
  // ถ้าเคยหาเจอแล้ว ไม่ต้องหาซ้ำ
  if (discoveredIP != "") return;

  udp.beginPacket(IPAddress(255, 255, 255, 255), 5001);
  udp.print("DISCOVER_LOG_SERVER");
  udp.endPacket();

  // รอแค่ 200ms ไม่ใช่ 20 วินาที
  unsigned long startWait = millis();
  while (millis() - startWait < 50) {
    int packetSize = udp.parsePacket();
    if (packetSize) {
      char reply[20];
      int len = udp.read(reply, sizeof(reply) - 1);
      reply[len] = 0;
      if (String(reply) == "I_AM_LOG_SERVER") {
        discoveredIP = udp.remoteIP().toString();
        Serial.println("✅ Found Server at: " + discoveredIP);
        return;
      }
    }
  }
  // ไม่เจอก็ไม่เป็นไร ข้ามไปก่อน
}
// ============================================================
//  GUI ←→ main callbacks
//  (เรียกจาก gui.cpp เมื่อผู้ใช้กดปุ่มบนจอทัชสกรีน)
// ============================================================

// ── state ที่จอทัชสกรีนต้องจำ ──
// (ประกาศ static ไว้ข้างบนสุดของไฟล์แล้ว)

// ── helper: ส่งข้อมูลสดกลับไปอัปเดตหน้าจอ ──
static void gui_refresh_home() {
    // Friends
    static GuiUserEntry ue[MAX_USERS];
    int uc = 0;
    for (int i = 0; i < MAX_USERS && uc < MAX_USERS; i++) {
        if (!users[i].used) continue;
        if (users[i].uid == 0 || users[i].uid == g_gui_myUid) continue;
        if (!users[i].isLoggedIn) continue;
        String dn = getDisplayNameByUid(users[i].uid);
        if (dn == "SYSTEM" || dn == "System") continue;
        ue[uc].uid = users[i].uid;
        strncpy(ue[uc].name, dn.c_str(), 31);
        ue[uc].online = isUidOnline(users[i].uid);
        uc++;
    }
    gui_update_userlist(ue, uc);

    // Rooms
    static GuiRoomEntry re[MAX_ROOMS];
    int rc = 0;
    for (int i = 0; i < MAX_ROOMS && rc < MAX_ROOMS; i++) {
        if (!rooms[i].used) continue;
        if (isDmRoomName(rooms[i].name)) continue;
        strncpy(re[rc].name, rooms[i].name.c_str(), 47);
        re[rc].memberCount = rooms[i].memberUidCount;
        if (rooms[i].ownerUid == g_gui_myUid && g_gui_myUid != 0)
            re[rc].status = 1; // owner
        else if (isMemberUid(i, g_gui_myUid))
            re[rc].status = 2; // member
        else {
            re[rc].status = 0;
            for (int k = 0; k < rooms[i].requestCount; k++)
                if (rooms[i].requests[k] == g_gui_myUid) { re[rc].status = 3; break; }
        }
        rc++;
    }
    gui_update_roomlist(re, rc);

    // Invites
    int uidx = findUserByIP(g_gui_myIP);
    if (uidx >= 0) {
        static GuiInviteEntry ie[MAX_INVITES_PER_USER];
        int ic = 0;
        for (int i = 0; i < MAX_INVITES_PER_USER && ic < MAX_INVITES_PER_USER; i++) {
            if (!users[uidx].invites[i].used) continue;
            ie[ic].idx = i;
            strncpy(ie[ic].room, users[uidx].invites[i].roomName.c_str(), 47);
            String invNm = getDisplayNameByUid(users[uidx].invites[i].inviterUid);
            strncpy(ie[ic].inviter, invNm.c_str(), 31);
            ic++;
        }
        gui_update_invites(ie, ic);
    }
}

static void gui_refresh_room(const String& roomName) {
    int ridx = findRoom(roomName);
    if (ridx < 0) return;

    // Members
    static GuiMemberEntry me[MAX_MEMBERS_PER_ROOM];
    int mc = 0;
    for (int i = 0; i < rooms[ridx].memberUidCount && mc < MAX_MEMBERS_PER_ROOM; i++) {
        uint32_t uid = rooms[ridx].memberUids[i];
        me[mc].uid = uid;
        String dn = getDisplayNameByUid(uid);
        strncpy(me[mc].name, dn.c_str(), 31);
        me[mc].online = isUidOnline(uid);
        mc++;
    }
    gui_update_members(me, mc);

    // Requests (เฉพาะ owner)
    static GuiMemberEntry qe[MAX_REQUESTS_PER_ROOM];
    int qc = 0;
    if (rooms[ridx].ownerUid == g_gui_myUid) {
        for (int i = 0; i < rooms[ridx].requestCount && qc < MAX_REQUESTS_PER_ROOM; i++) {
            uint32_t uid = rooms[ridx].requests[i];
            qe[qc].uid = uid;
            String dn = getDisplayNameByUid(uid);
            strncpy(qe[qc].name, dn.c_str(), 31);
            qe[qc].online = isUidOnline(uid);
            qc++;
        }
    }
    gui_update_requests(qe, qc);
}

// ── Login ──
void on_gui_login(const char* u, const char* p) {
    if (!checkLoginCreds(u, p)) {
        gui_show_toast("Login Failed");
        return;
    }
    int dbIdx = findDbUserByName(u);
    if (dbIdx < 0) { gui_show_toast("User not found"); return; }

    // ใช้ IP จำลองสำหรับ "session จอ" ที่ไม่ชนกับ wifi client จริง
    g_gui_myIP = "192.168.4.254"; // IP สงวนสำหรับ local GUI
    int uidx = upsertUserSeen(g_gui_myIP);
    if (uidx < 0) { gui_show_toast("Session full"); return; }

    users[uidx].isLoggedIn = true;
    users[uidx].uid        = dbUsers[dbIdx].uid;
    users[uidx].username   = String(u);
    users[uidx].lastSeen   = millis();

    g_gui_myUid = users[uidx].uid;

    // rebind rooms
    for (int i = 0; i < MAX_ROOMS; i++) {
        if (!rooms[i].used) continue;
        if (isMemberUid(i, g_gui_myUid)) addMember(i, g_gui_myIP);
    }
    ensureDefaultGroupAndMembership(g_gui_myIP);

    String dn = getDisplayNameByUid(g_gui_myUid);
    gui_set_my_info(g_gui_myUid, dn.c_str());
    gui_show_screen(SCR_HOME);
    gui_refresh_home();
}

// ── Register ──
void on_gui_register(const char* u, const char* p) {
    if (registerUser(u, p, 0, true)) {
        gui_show_toast("Registered! Please login.");
        gui_show_screen(SCR_LOGIN);
    } else {
        gui_show_toast("Register Failed: name taken");
    }
}

// ── Send message (ห้องปัจจุบัน) ──
void on_gui_send_message(const char* msg) {
    String roomName = (g_gui_curRoom.length() > 0) ? g_gui_curRoom : String(DEFAULT_ROOM_NAME);
    int ridx = findRoom(roomName);
    if (ridx < 0) return;
    if (g_gui_myUid == 0) return;

    String text = String(msg);
    String senderName = getDisplayNameByUid(g_gui_myUid);

    addChatMessage(ridx, senderName, g_gui_myUid, text, false);

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = isDmRoomName(roomName) ? PKT_CHAT_DM : PKT_CHAT_GROUP;
    pkt.fromNode = NODE_ID;
    pkt.msgId    = millis();

    strncpy(pkt.senderName, senderName.c_str(), 20);
    snprintf(pkt.payload, sizeof(pkt.payload), "%lu|%s", (unsigned long)g_gui_myUid, text.c_str());
    strncpy(pkt.target, roomName.c_str(), 24);

    sendLoraPacket(pkt, 0xFF);
}

// ── Create room ──
void on_gui_create_room(const char* name) {
    if (g_gui_myUid == 0) { gui_show_toast("Login first"); return; }
    String rn = String(name);
    if (!isValidRoomName(rn))  { gui_show_toast("Invalid name"); return; }
    if (findRoom(rn) >= 0)     { gui_show_toast("Name taken");   return; }
    if (!hasFreeRoomSlot())    { gui_show_toast("Room limit");   return; }

    int ridx = createRoomInternal(rn, g_gui_myUid, true);
    if (ridx < 0) { gui_show_toast("Create failed"); return; }

    needSaveRooms = true;
    gui_show_toast("Group created!");
    gui_refresh_home();
}

// ── Join room (request) ──
void on_gui_join_room(const char* name) {
    if (g_gui_myUid == 0) return;
    String rn = String(name);
    int ridx = findRoom(rn);
    if (ridx < 0) { gui_show_toast("Room not found"); return; }

    if (rn == DEFAULT_ROOM_NAME) {
        addMemberUid(ridx, g_gui_myUid);
        needSaveRooms = true;
        on_gui_open_room(name);
        return;
    }

    addRequestUid(ridx, g_gui_myUid);

    LoraPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PKT_JOIN_REQ;
    pkt.fromNode = NODE_ID;
    pkt.msgId    = millis();
    strncpy(pkt.senderName, getDisplayNameByUid(g_gui_myUid).c_str(), 20);
    snprintf(pkt.payload, sizeof(pkt.payload), "%lu", (unsigned long)g_gui_myUid);
    strncpy(pkt.target, rn.c_str(), 24);
    sendLoraPacket(pkt, 0xFF);

    gui_show_toast("Join request sent");
    gui_refresh_home();
}

// ── Open room ──
void on_gui_open_room(const char* name) {
    if (g_gui_myUid == 0) return;
    String rn = String(name);
    int ridx = findRoom(rn);
    if (ridx < 0) { gui_show_toast("Room not found"); return; }
    if (!isMemberUid(ridx, g_gui_myUid)) { gui_show_toast("Not a member"); return; }

    g_gui_curRoom = rn;
    rooms[ridx].unreadCount = 0;

    // เปิดหน้า room และแสดง chat log ที่มีอยู่ทีละ bubble
    gui_show_room(name);

    // parse logHtml แล้วส่ง bubble ทีละข้อ (simplistic: ส่งทั้ง log เป็น system msg)
    // วิธีที่ง่ายที่สุดคือแสดง "Loaded X messages" แล้วรอ bubble ใหม่ real-time
    String info = "Room: " + rn + " (" + String(rooms[ridx].memberUidCount) + " members)";
    gui_add_chat_bubble("System", 0, info.c_str(), true);

    gui_refresh_room(rn);
}

// ── Open DM ──
void on_gui_open_dm(uint32_t uid) {
    Serial.printf("on_gui_open_dm uid=%lu myUid=%lu\n", (unsigned long)uid, (unsigned long)g_gui_myUid);
    if (uid == 0) { gui_show_toast("Invalid user"); return; }
    if (g_gui_myUid == 0) { gui_show_toast("Please login first"); return; }
    if (uid == g_gui_myUid) { gui_show_toast("Cannot DM yourself"); return; }

    int ridx = ensureDMRoomUidByUid(g_gui_myUid, uid);
    if (ridx < 0) { gui_show_toast("Cannot open DM"); return; }

    String dmName = rooms[ridx].name;
    g_gui_curRoom = dmName;
    rooms[ridx].unreadCount = 0;

    String otherName = getDisplayNameByUid(uid);
    gui_show_room(otherName.c_str());
    gui_refresh_room(dmName);
}

// ── Rename (ใช้จอทัช) ──
void on_gui_rename_me(const char* newName) {
    if (g_gui_myUid == 0) return;
    setDisplayNameByUid(g_gui_myUid, String(newName));
    gui_set_my_info(g_gui_myUid, newName);
    gui_show_toast("Name updated");
}

// ── Logout ──
void on_gui_logout() {
    int uidx = findUserByIP(g_gui_myIP);
    if (uidx >= 0) users[uidx].isLoggedIn = false;
    g_gui_myUid  = 0;
    g_gui_myIP   = "";
    g_gui_curRoom = "";
    gui_show_screen(SCR_SPLASH);
}

// ── Delete account ──
void on_gui_delete_self() {
    if (g_gui_myUid == 0) return;
    int dbIdx = findDbUserByUid(g_gui_myUid);
    if (dbIdx >= 0) deleteUserByName(dbUsers[dbIdx].username);
    on_gui_logout();
    gui_show_toast("Account deleted");
}

// ── Accept invite ──
void on_gui_accept_invite(int idx) {
    if (g_gui_myIP.length() == 0) return;
    if (acceptInvite(g_gui_myIP, idx)) {
        gui_show_toast("Joined room!");
        gui_refresh_home();
    }
}

// ── Decline invite ──
void on_gui_decline_invite(int idx) {
    if (g_gui_myIP.length() == 0) return;
    removeInvite(g_gui_myIP, idx);
    gui_refresh_home();
}
// ===================== setup/loop =====================
void setup() {
  Serial.begin(115200);
  delay(300);

  // ปิดการสื่อสาร (CS = HIGH) ของทุกอุปกรณ์ที่ใช้ SPI ไว้ก่อน
  pinMode(LORA_CS, OUTPUT); 
  digitalWrite(LORA_CS, HIGH); // ปิด LoRa
  
  pinMode(5, OUTPUT); 
  digitalWrite(5, HIGH);       // ปิด TFT_CS
  
  pinMode(15, OUTPUT); 
  digitalWrite(15, HIGH);      // ปิด TOUCH_CS


  // Mount LittleFS
  if(!LittleFS.begin(true)){ 
    Serial.println("LittleFS Mount Failed");
  } else {
    loadDb(); 
    loadRooms(); // [NEW] โหลดห้องและแชทกลับมา
  }

  gui_init(); 
  Serial.println("GUI init OK");

  // Init LoRa
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, LOW); delay(10);
  digitalWrite(LORA_RST, HIGH); delay(10);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_IRQ);

  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed");
    while (1);
  }

  LoRa.setTxPower(20);
  LoRa.enableCrc();

  Serial.println("LoRa init OK");


  // Init Data
  for (int i = 0; i < 255; i++) {
    topology[i].active = false;
    topology[i].userSynced = false; // ⭐ สำคัญ
  }

  // reset runtime users (IP session)
  for (int i = 0; i < MAX_USERS; i++) {
    users[i].used = false;
    users[i].isLoggedIn = false;
    users[i].uid = 0;
    for (int k = 0; k < MAX_INVITES_PER_USER; k++)
      users[i].invites[k].used = false;
  }
  
  // ตรวจสอบ Default Group ถ้าไม่มีให้สร้าง
  if (findRoom(DEFAULT_ROOM_NAME) < 0) {
      uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
      createRoomInternal(DEFAULT_ROOM_NAME, adminUid, false);
  }

  // Init WiFi
  WiFi.mode(WIFI_AP_STA);
  sprintf(ssid, "ESP32-MESH-Node%d", NODE_ID);
  WiFi.softAP(ssid, password);
  
  Serial.print("Connecting PC WiFi: ");
  Serial.println(COM_SSID);
  WiFi.begin(COM_SSID, COM_PASS);
  
  server.begin();
  
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  // ── LVGL ต้องรันบ่อยๆ ห้ามมีอะไร block นานก่อน ──
  digitalWrite(LORA_CS, HIGH);
  gui_task_handler();   // อ่าน touch + render ใน lv_timer_handler

  // ── งานหนัก ทำแบบ time-slice ──
  static unsigned long lastHeavy = 0;
  if (millis() - lastHeavy >= 10) {
    lastHeavy = millis();
    checkLoRa();
    processRelayQueue();
  }

  static unsigned long lastSave = 0;
  if (millis() - lastSave > 10000) {
    lastSave = millis();
    if (needSaveRooms) { saveRooms(); needSaveRooms = false; }
    if (needSaveDb)    { saveDb();    needSaveDb = false; }
    gui_task_handler(); // ✅ FIX: refresh จอหลัง save เผื่อ LittleFS ค้างนาน
  }

  // อัปเดต GUI ทุก 5 วินาที (refresh home/room list)
  static unsigned long lastGuiRefresh = 0;
  if (millis() - lastGuiRefresh > 5000) {
    lastGuiRefresh = millis();
    if (g_gui_myUid != 0) {
      int uidx = findUserByIP(g_gui_myIP);
      if (uidx >= 0) users[uidx].lastSeen = millis();
      gui_refresh_home();
    }
  }

  // ── LoRa งานอื่น (ไม่บ่อย) ──
  sendHeartbeats();
  gui_task_handler(); // ✅ FIX: refresh หลัง sendHeartbeats เผื่อมีหลาย user

  reportToComputer();
  gui_task_handler(); // ✅ FIX: refresh หลัง HTTP POST ที่อาจค้างนานได้

  processLogQueue();

  WiFiClient client = server.available();
  if (!client) return;

  header = "";
  String currentLine = "";
  unsigned long startClient = millis();

  while (client.connected() && millis() - startClient < 500) {
    if (!client.available()) {
      gui_task_handler(); // ✅ FIX: ขณะรอ client ส่งข้อมูล ให้ LVGL ได้ทำงาน
      continue;
    }

    char c = client.read();
    header += c;

    if (c == '\n') {
      if (currentLine.length() == 0) {

        String path = getRequestPath(header);
        String myIP = ipToString(client.remoteIP());

        int uidx = upsertUserSeen(myIP);
        bool loggedIn = (uidx >= 0 && users[uidx].isLoggedIn);
        bool isPublic =
          path.startsWith("/login") ||
          path.startsWith("/register") ||
          path.startsWith("/favicon");

        // ===== REGISTER =====
        if (path.startsWith("/register_act?")) {
          String u = getQueryParam(path, "u");
          String p = getQueryParam(path, "p");

          if (registerUser(u.c_str(), p.c_str(), 0, true)) {
            httpOKHtml(client,
              "<script>alert('Register Success');location='/login';</script>");
          } else {
            httpOKHtml(client,
              "<script>alert('Register Failed');location='/login';</script>");
          }
          break;
        }

        // ===== LOGIN =====
        if (path.startsWith("/login_act?")) {
          String u = getQueryParam(path, "u");
          String p = getQueryParam(path, "p");

          if (!checkLoginCreds(u.c_str(), p.c_str())) {
            httpOKHtml(client,
              "<script>alert('Login Failed');location='/login';</script>");
            break;
          }

          int dbIdx = findDbUserByName(u.c_str());
          if (dbIdx < 0) {
            httpRedirect(client, "/login");
            break;
          }

          users[uidx].isLoggedIn = true;
          users[uidx].uid = dbUsers[dbIdx].uid;
          users[uidx].username = u;

          uint32_t uid = users[uidx].uid;

          // rebind rooms by UID
          for (int i = 0; i < MAX_ROOMS; i++) {
            if (!rooms[i].used) continue;
            if (isMemberUid(i, uid)) {
              addMember(i, myIP);
            }
          }

          httpRedirect(client, "/");
          
          break;
        }

        // ===== LOGOUT =====
        if (path.startsWith("/logout")) {
          if (uidx >= 0) {
            users[uidx].isLoggedIn = false;   // offline ทันที
          }
          httpRedirect(client, "/login");
          break;
        }
        
        if (path.startsWith("/delete_self")) {
          int uidx = findUserByIP(myIP);
          if (uidx >= 0) {
            String uname = users[uidx].username;
            if (uname.length() > 0) {
              deleteUserByName(uname.c_str());
            }

            users[uidx].username = "";
            users[uidx].displayName = "";
            users[uidx].isLoggedIn = false;
          }
          httpRedirect(client, "/login");
          break;
        }
          
        // [NEW] Reset DB Route (ล้างเครื่อง)
        if (path.startsWith("/reset_db")) { handleFactoryReset(client, myIP, path); break; }
        if (path.startsWith("/delete_all")) {
          String pw = getQueryParam(path, "p");
          if (pw != ADMIN_PASS) {
            httpFailText(client, 403, "Wrong admin password");
            break;
          }
          LittleFS.remove(DB_FILE);
          dbUserCount = 0;
          httpOKHtml(client, "<h1>All users deleted</h1><a href='/login'>OK</a>");
          break;
        }

        if (path.startsWith("/admin/")) { 
            handleRemoteAdmin(client, path); 
            break; 
        }
        // ===== FORCE LOGIN =====
        if (!loggedIn && !isPublic) {
          sendLoginPage(client);
          break;
        }
          
        // ===== ROUTES (ของเดิมทั้งหมด) =====
        if (path == "/") { 
          uint32_t uid = getUidFromIP(myIP);
          sendHomePage(client, String(uid)); break; }
        if (path == "/login") { sendLoginPage(client); break; }

        if (path.startsWith("/ping")) { handlePing(client, myIP); break; }
        if (path.startsWith("/api/stats")) { handleApiStats(client, myIP); break; }
        if (path.startsWith("/api/userlist")) { handleApiUserList(client, myIP); break; }
        if (path.startsWith("/api/invites")) { handleApiInvites(client, myIP); break; }
        if (path.startsWith("/api/rooms")) { handleApiRooms(client, myIP); break; }
        if (path.startsWith("/api/room?")) { handleApiRoom(client, myIP, path); break; }
        if (path.startsWith("/api/log?")) { handleApiLog(client, myIP, path); break; }

        if (path.startsWith("/setname?")) { handleSetName(client, myIP, path); break; }
        if (path.startsWith("/create?")) { handleCreateRoom(client, myIP, path); break; }
        if (path.startsWith("/delete?")) { handleDeleteRoom(client, myIP, path); break; }
        if (path.startsWith("/leave?")) { handleLeaveRoom(client, myIP, path); break; }
        if (path.startsWith("/request?")) { handleRequestJoin(client, myIP, path); break; }
        if (path.startsWith("/approve?")) { handleApprove(client, myIP, path); break; }
        if (path.startsWith("/send?")) { handleSendMsg(client, myIP, path); break; }
        if (path.startsWith("/invite?")) { handleInvite(client, myIP, path); break; }
        if (path.startsWith("/invite_accept?")) { handleInviteAccept(client, myIP, path, true); break; }
        if (path.startsWith("/invite_decline?")) { handleInviteAccept(client, myIP, path, false); break; }
        if (path.startsWith("/dm?") || path.startsWith("/chat?")) {
          handleChat(client, myIP, path);
          break;
        }
        if (path.startsWith("/room?")) {
          uint32_t uid = getUidFromIP(myIP);
          sendRoomPage(client, String(uid), getQueryParam(path, "name"));
          break;
        }

        httpRedirect(client, "/");
        break;
      }
      currentLine = "";
    } else if (c != '\r') {
      currentLine += c;
    }
  }

  header = "";
  client.stop();

  /*
  checkLoRa();
  sendHeartbeats();
  reportToComputer();

  WiFiClient client = server.available();
  if (!client) return;

  header = "";
  String currentLine = "";
  unsigned long startClient = millis();

  while (client.connected() && (millis() - startClient < 2000)) { 
    if (client.available()) {
      char c = client.read();
      header += c;

      if (c == '\n') {
        if (currentLine.length() == 0) {
          String path = getRequestPath(header);
          String myIP = ipToString(client.remoteIP());

          upsertUserSeen(myIP); 
          ensureDefaultGroupAndMembership(myIP);

          bool isPublic = path.startsWith("/login") || path.startsWith("/register") || path.startsWith("/favicon");
          bool loggedIn = isIpLoggedIn(myIP);

          // Handle Register
          // [แก้ไข] ส่วนจัดการการลงทะเบียน ให้แสดง Popup แทนหน้าขาว
          if (path.startsWith("/register_act?")) {
            String u = getQueryParam(path, "u");
            String p = getQueryParam(path, "p");

            // เรียกฟังก์ชันสมัครสมาชิก (uid=0, isLocal=true)
            if (registerUser(u.c_str(), p.c_str(), 0, true)) {
              // สมัครสำเร็จ -> ส่ง HTML ที่มี script เพื่อ Alert และเด้งกลับหน้า Login
              String html = "<script>alert('Register Success! Please Login.'); window.location.href='/login';</script>";
              httpOKHtml(client, html);
            } else {
              // สมัครไม่สำเร็จ -> Alert แจ้งเตือนและเด้งกลับ
              String html = "<script>alert('Register Failed! Username taken or System full.'); window.location.href='/login';</script>";
              httpOKHtml(client, html); // ใช้ httpOKHtml เพื่อให้ Browser รัน Script
            }
            break;
          }

          // Handle Login
          // [แก้ไข] ส่วนจัดการการล็อคอิน ให้แสดง Popup
          if (path.startsWith("/login_act?")) {
            String u = getQueryParam(path, "u");
            String p = getQueryParam(path, "p");

            if (checkLoginCreds(u.c_str(), p.c_str())) {

              int dbIdx = findDbUserByName(u.c_str());
              if (dbIdx < 0) {
                httpOKHtml(client,
                  "<script>alert('Login Failed'); window.location.href='/login';</script>");
                break;
              }

              int uidx = upsertUserSeen(myIP);

              // ⭐ login state
              users[uidx].isLoggedIn = true;

              // ⭐ ผูกตัวตนจริง
              users[uidx].uid = dbUsers[dbIdx].uid;
              uint32_t uid = users[uidx].uid;
              for (int i = 0; i < MAX_ROOMS; i++) {
                if (!rooms[i].used) continue;

                // ถ้า uid นี้เคยเป็นสมาชิก
                if (isMemberUid(i, uid)) {
                  addMember(i, myIP);   // ผูก ip ปัจจุบันกลับเข้าห้อง
                }
              }
              users[uidx].username = u;

              // fallback
              if (users[uidx].displayName.length() == 0)
                users[uidx].displayName = users[uidx].username;

              httpRedirect(client, "/");
            } else {
              String html =
                "<script>alert('Login Failed! Wrong username or password.');"
                "window.location.href='/login';</script>";
              httpOKHtml(client, html);
            }
            break;
          }
                   
          if (path.startsWith("/logout")) {
             int uidx = upsertUserSeen(myIP);
             users[uidx].isLoggedIn = false;
             httpRedirect(client, "/login");
             break;
          }
          if (path.startsWith("/delete_self")) {
             int uidx = findUserByIP(myIP);
             if (uidx >= 0) {
                String uname = users[uidx].username;
                if (uname.length() > 0) {
                  deleteUserByName(uname.c_str());
                }

                users[uidx].username = "";
                users[uidx].displayName = "";
                users[uidx].isLoggedIn = false;
             }
             httpRedirect(client, "/login");
             break;
          }
          
          // [NEW] Reset DB Route (ล้างเครื่อง)
          if (path.startsWith("/reset_db")) { handleFactoryReset(client, myIP, path); break; }
          if (path.startsWith("/delete_all")) {
             String pw = getQueryParam(path, "p");
             if (pw != ADMIN_PASS) {
                httpFailText(client, 403, "Wrong admin password");
                break;
             }
             LittleFS.remove(DB_FILE);
             dbUserCount = 0;
             httpOKHtml(client, "<h1>All users deleted</h1><a href='/login'>OK</a>");
             break;
          }


          // Force Login
          if (!loggedIn && !isPublic) {
             sendLoginPage(client);
             break;
          }

          if (path == "/") { sendHomePage(client, myIP); break; }
          if (path == "/login") { sendLoginPage(client); break; } 

          if (path.startsWith("/ping")) { handlePing(client, myIP); break; }
          if (path.startsWith("/api/stats")) { handleApiStats(client, myIP); break; }
          if (path.startsWith("/api/userlist")) { handleApiUserList(client, myIP); break; }
          if (path.startsWith("/api/invites")) { handleApiInvites(client, myIP); break; }
          if (path.startsWith("/api/rooms")) { handleApiRooms(client, myIP); break; }
          if (path.startsWith("/api/room?")) { handleApiRoom(client, myIP, path); break; }
          if (path.startsWith("/api/log?"))  { handleApiLog(client, myIP, path); break; }

          if (path.startsWith("/setname?")) { handleSetName(client, myIP, path); break; }
          if (path.startsWith("/create?")) { handleCreateRoom(client, myIP, path); break; }
          if (path.startsWith("/delete?")) { handleDeleteRoom(client, myIP, path); break; }
          if (path.startsWith("/leave?")) { handleLeaveRoom(client, myIP, path); break; }
          if (path.startsWith("/request?")) { handleRequestJoin(client, myIP, path); break; }
          if (path.startsWith("/approve?")) { handleApprove(client, myIP, path); break; }
            
          if (path.startsWith("/send?")) { handleSendMsg(client, myIP, path); break; }

          if (path.startsWith("/invite?")) { handleInvite(client, myIP, path); break; }
            // เพิ่มคำสั่งสำหรับล้างเครื่องผ่าน Browser โดยพิมพ์ http://192.168.4.1/factory_reset
          if (path.startsWith("/factory_reset")) {
            LittleFS.format(); // สั่งล้างข้อมูล
            httpOKText(client, "Memory Formatted. Please Restart Device."); // แจ้งเตือน
            delay(1000);
            ESP.restart(); // รีสตาร์ทเครื่อง
            break;
          }
          if (path.startsWith("/invite_accept?")) { handleInviteAccept(client, myIP, path, true); break; }
          if (path.startsWith("/invite_decline?")) { handleInviteAccept(client, myIP, path, false); break; }

          if (path.startsWith("/dm?")) { handleChat(client, myIP, path); break; }
          if (path.startsWith("/chat?")) { handleChat(client, myIP, path); break; }

          if (path.startsWith("/room?")) {
            String rn = getQueryParam(path, "name");
            sendRoomPage(client, myIP, rn);
            break;
          }

          httpRedirect(client, "/");
          break;
        } else {
          currentLine = "";
        }
      } else if (c != '\r') {
        currentLine += c;
      }
    }
  }

  header = "";
  client.stop();
  */
}