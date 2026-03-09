#include "web.h"
#include "globals.h"
#include <Arduino.h>

// ── Inline SVG icons (Feather-style, ไม่ต้องโหลด CDN) ──────────────────────
#define ICO_REFRESH  "<svg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><polyline points='23 4 23 10 17 10'/><path d='M20.49 15a9 9 0 1 1-2.12-9.36L23 10'/></svg>"
#define ICO_EDIT     "<svg xmlns='http://www.w3.org/2000/svg' width='13' height='13' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7'/><path d='M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z'/></svg>"
#define ICO_CHEVRON  "<svg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='#ccc' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><polyline points='9 18 15 12 9 6'/></svg>"
#define ICO_MAIL     "<svg xmlns='http://www.w3.org/2000/svg' width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='#888' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2z'/><polyline points='22,6 12,13 2,6'/></svg>"
#define ICO_LOGOUT   "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='#3d5a80' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4'/><polyline points='16 17 21 12 16 7'/><line x1='21' y1='12' x2='9' y2='12'/></svg>"
#define ICO_USERX    "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='#d93025' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M16 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2'/><circle cx='8.5' cy='7' r='4'/><line x1='18' y1='8' x2='23' y2='13'/><line x1='23' y1='8' x2='18' y2='13'/></svg>"
#define ICO_WARN     "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' fill='none' stroke='#d93025' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z'/><line x1='12' y1='9' x2='12' y2='13'/><line x1='12' y1='17' x2='12.01' y2='17'/></svg>"
#define ICO_NAV_USERS "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2'/><circle cx='9' cy='7' r='4'/><path d='M23 21v-2a4 4 0 0 0-3-3.87'/><path d='M16 3.13a4 4 0 0 1 0 7.75'/></svg>"
#define ICO_NAV_MSG   "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z'/></svg>"
#define ICO_NAV_BELL  "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9'/><path d='M13.73 21a2 2 0 0 1-3.46 0'/></svg>"
#define ICO_NAV_SET   "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><circle cx='12' cy='12' r='3'/><path d='M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z'/></svg>"
// ─────────────────────────────────────────────────────────────────────────────

// ====== extern จาก main.cpp หรือไฟล์อื่น ======
extern char ssid[];
extern const char* DEFAULT_ROOM_NAME;
extern const char* DEFAULT_OWNER_IP;

// โครงสร้าง / ฟังก์ชันที่มีอยู่แล้วในโปรเจกต์เธอ
extern int findRoom(const String& name);
extern int createRoomInternal(const String& name, uint32_t ownerUid, bool addOwnerAsMember);
extern void addMember(int ridx, const String& ip);
extern bool isMemberUid(int ridx, uint32_t uid);
extern bool isDmRoomName(const String& name);
extern bool addMemberUid(int ridx, uint32_t uid);
extern bool isMember(int ridx, const String& ip);
extern uint32_t getUidFromIP(const String& ip);

extern String getDisplayName(const String& ip);
extern String getDisplayNameByUid(uint32_t uid);
extern String urlEncode(const String& s);
bool parseUidSafe(const String& uidStr, uint32_t& out) {
  for (size_t i = 0; i < uidStr.length(); i++) {
    if (!isDigit(uidStr[i])) return false;
  }
  out = uidStr.toInt();
  return true;
}

// ===================== HTTP helpers =====================
void httpRedirect(WiFiClient& client, const String& location) {
  client.println("HTTP/1.1 303 See Other");
  client.println(String("Location: ") + location);
  client.println("Connection: close");
  client.println();
}

void httpOKText(WiFiClient& client, const String& text) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print(text);
}

void httpFailText(WiFiClient& client, int code, const String& text) {
  client.print("HTTP/1.1 ");
  client.print(code);
  client.println(" Error");
  client.println("Content-Type: text/plain; charset=utf-8");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print(text);
}

void httpOKJson(WiFiClient& client, const String& json) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json; charset=utf-8");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print(json);
}

void httpOKHtml(WiFiClient& client, const String& html) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Cache-Control: no-store");
  client.println("Connection: close");
  client.println();
  client.print(html);
}

void ensureDefaultGroupAndMembership(const String& myIP) {
  uint32_t uid = getUidFromIP(myIP);
  if (uid == 0) return;

  int ridx = findRoom(DEFAULT_ROOM_NAME);
  if (ridx < 0) {
    uint32_t adminUid = getUidFromIP(DEFAULT_OWNER_IP);
    ridx = createRoomInternal(DEFAULT_ROOM_NAME, adminUid, false);
  }
  if (ridx >= 0)
    addMemberUid(ridx, uid);
}


// ===================== Pages =====================
// *** ตรงนี้คือโค้ด sendLoginPage / sendHomePage / sendRoomPage
// *** ใช้เหมือนที่เธอส่งมาได้ทั้งก้อน
// *** ฉันไม่แก้เนื้อหา HTML/JS ภายในเลย
void sendLoginPage(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();
  client.println("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
  client.println("<title>Login</title>");
  client.println("<style>");
  client.println("*{box-sizing:border-box;margin:0;padding:0;font-family:sans-serif;}");
  client.println("body{background:#f3f3f3;display:flex;align-items:center;justify-content:center;min-height:100vh;padding:20px;}");
  client.println(".box{background:#fff;padding:16px;border-radius:12px;width:100%;max-width:380px;border:1px solid #ddd;}");  client.println("h2{font-size:16px;text-align:center;color:#1f3a68;margin-bottom:20px;line-height:1.5;}");
  client.println("h3,h4{font-size:15px;color:#333;margin-bottom:12px;}");
  client.println("label{font-size:13px;color:#555;display:block;margin-bottom:4px;}");
  client.println("input{display:block;width:100%;padding:9px 12px;font-size:14px;border-radius:8px;border:1px solid #ccc;margin-bottom:8px;outline:none;}");  client.println("input:focus{border-color:#1f3a68;}");
  client.println("button{width:100%;padding:12px;font-size:14px;font-weight:bold;border-radius:8px;border:none;color:#fff;cursor:pointer;}");
  client.println(".btn-login{background:#1f3a68;}");
  client.println(".btn-register{background:#2e7d32;}");
  client.println(".divider{margin:12px 0;border-top:0px solid #eee;}");
  client.println("</style></head><body>");
  client.println("<div class='box'>");
  client.println("<h2>LoRa-based MANET<br>Message Communication System</h2>");

  client.println("<h3>Login</h3>");
  client.println("<form action='/login_act' method='get'>");
  client.println("<label>Username</label>");
  client.println("<input type='text' name='u' placeholder='Enter username' required>");
  client.println("<label>Password</label>");
  client.println("<input type='password' name='p' placeholder='Enter password' required>");
  client.println("<button type='submit' class='btn-login'>Login</button>");
  client.println("</form>");

  client.println("<div class='divider'></div>");

  client.println("<h4>Register New Account</h4>");
  client.println("<form action='/register_act' method='get'>");
  client.println("<label>Username</label>");
  client.println("<input type='text' name='u' placeholder='English only' required>");
  client.println("<label>Password</label>");
  client.println("<input type='password' name='p' placeholder='Enter password' required>");
  client.println("<button type='submit' class='btn-register'>Register</button>");
  client.println("</form>");

  client.println("</div></body></html>");
}


void sendHomePage(WiFiClient& client, const String& myUID) {
  uint32_t uid     = myUID.toInt();
  String meName    = getDisplayNameByUid(uid);
  String meInitial = (meName.length() > 0) ? String((char)meName[0]) : "?";

  // ── HTTP headers ──
  client.print(F("HTTP/1.1 200 OK\r\nContent-type:text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"));

  // ── Head + CSS ──
  client.print(F("<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
    "<title>MANET Comm</title><style>"
    ":root{--g:#06c755;--g2:#05a847;--bg:#f5f5f5;--card:#fff;--bd:#e8e8e8;--tx:#111;--sub:#888;--red:#d93025;}"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{font-family:-apple-system,'Helvetica Neue',Arial,sans-serif;background:var(--bg);color:var(--tx);padding-bottom:64px;}"
    ".hdr{background:#fff;height:54px;padding:0 16px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid var(--bd);position:sticky;top:0;z-index:900;}"
    ".htx{font-size:15px;font-weight:700;letter-spacing:.2px;}"
    ".hsx{font-size:10px;color:var(--sub);margin-top:1px;}"
    ".rbtn{display:flex;align-items:center;gap:6px;background:none;border:1px solid var(--bd);border-radius:8px;padding:6px 12px;font-size:12px;color:var(--sub);cursor:pointer;font-weight:500;}"
    ".rbtn:hover{border-color:var(--g);color:var(--g);}"
    ".sl{padding:16px 16px 6px;font-size:11px;font-weight:600;color:var(--sub);letter-spacing:.8px;text-transform:uppercase;}"
    ".pc{margin:8px 12px;background:var(--card);border-radius:12px;border:1px solid var(--bd);padding:14px 16px;display:flex;align-items:center;gap:14px;}"
    ".pav{width:48px;height:48px;border-radius:12px;background:#e8f9ef;border:1.5px solid var(--g);display:flex;align-items:center;justify-content:center;font-size:20px;font-weight:700;color:var(--g);flex-shrink:0;}"
    ".pnm{font-size:15px;font-weight:700;}.puid{font-size:11px;color:var(--sub);margin-top:2px;}"
    ".ebtn{margin-left:auto;display:flex;align-items:center;gap:5px;background:none;border:1px solid var(--bd);border-radius:8px;padding:7px 12px;font-size:12px;font-weight:500;color:var(--sub);cursor:pointer;}"
    ".ebtn:hover{border-color:#aaa;color:var(--tx);}"
    ".lg{margin:0 12px;background:var(--card);border-radius:12px;border:1px solid var(--bd);overflow:hidden;}"
    ".li{display:flex;align-items:center;padding:12px 14px;gap:12px;border-bottom:1px solid var(--bd);cursor:pointer;transition:background .12s;}"
    ".li:last-child{border-bottom:none;}.li:hover{background:#fafafa;}"
    ".av{width:42px;height:42px;border-radius:10px;display:flex;align-items:center;justify-content:center;font-size:16px;font-weight:700;color:#fff;flex-shrink:0;position:relative;}"
    ".ad{width:9px;height:9px;border-radius:50%;border:2px solid #fff;position:absolute;bottom:-1px;right:-1px;}"
    ".on{background:var(--g);}.off{background:#ccc;}"
    ".ib{flex:1;min-width:0;}.in{font-size:14px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
    ".is{font-size:11px;color:var(--sub);margin-top:2px;}"
    ".badge{font-size:10px;font-weight:600;padding:3px 9px;border-radius:20px;white-space:nowrap;min-width:60px;text-align:center;}"
    ".bo{background:#fff6e0;color:#b07000;border:1px solid #f0d080;}"
    ".bm{background:#e6f9ee;color:#068040;border:1px solid #90ddb0;}"
    ".bp{background:#fff6e0;color:#a06010;border:1px solid #f0c060;}"
    ".bn{background:#f2f2f2;color:#999;border:1px solid #e0e0e0;}"
    ".ga{display:flex;align-items:center;justify-content:flex-end;gap:8px;min-width:120px;}"
    ".btn{padding:6px 14px;border-radius:8px;font-size:12px;font-weight:600;cursor:pointer;border:none;width:64px;text-align:center;}"
    ".bj{background:var(--g);color:#fff;}.bj:hover{background:var(--g2);}"
    ".bd2{background:#fdecea;color:var(--red);border:1px solid #f5c0bc;}"
    ".bph{width:64px;visibility:hidden;}"
    ".cb{margin:4px 12px 0;display:flex;gap:8px;background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:10px 12px;}"
    ".cb input{flex:1;border:none;outline:none;font-size:14px;background:transparent;}"
    ".cb input::placeholder{color:#bbb;}"
    ".cb button{background:var(--g);color:#fff;border:none;border-radius:8px;padding:8px 16px;font-size:13px;font-weight:600;cursor:pointer;}"
    ".icard{margin:0 12px 8px;background:var(--card);border-radius:12px;border:1px solid var(--bd);padding:14px;}"
    ".itop{display:flex;align-items:center;gap:10px;}"
    ".iico{width:38px;height:38px;background:#f0f0f0;border-radius:10px;display:flex;align-items:center;justify-content:center;flex-shrink:0;}"
    ".inm{font-size:14px;font-weight:600;}.ifr{font-size:11px;color:var(--sub);margin-top:2px;}"
    ".ibtns{display:flex;gap:8px;margin-top:12px;}"
    ".bacc{flex:1;padding:9px;background:var(--g);color:#fff;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;}"
    ".bdec{flex:1;padding:9px;background:#f2f2f2;color:#555;border:none;border-radius:8px;font-size:13px;font-weight:500;cursor:pointer;}"
    ".si{display:flex;align-items:center;gap:12px;padding:14px 16px;border-bottom:1px solid var(--bd);cursor:pointer;transition:background .12s;}"
    ".si:last-child{border-bottom:none;}.si:hover{background:#fafafa;}"
    ".sico{width:34px;height:34px;border-radius:9px;display:flex;align-items:center;justify-content:center;flex-shrink:0;}"
    ".slb{font-size:14px;font-weight:500;}.ssb{font-size:11px;color:var(--sub);margin-top:1px;}"
    ".dlb{color:var(--red)!important;}.dico{background:#fdecea!important;}"
    ".nav{position:fixed;bottom:0;left:0;right:0;height:64px;background:#fff;border-top:1px solid var(--bd);display:flex;z-index:900;}"
    ".ni{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:10px;font-weight:500;color:var(--sub);cursor:pointer;gap:4px;position:relative;border:none;background:none;}"
    ".ni.active{color:var(--g);}"
    ".ni.active::after{content:'';position:absolute;bottom:0;left:30%;right:30%;height:2px;background:var(--g);border-radius:2px 2px 0 0;}"
    ".nb{background:var(--red);color:#fff;border-radius:9px;padding:0 5px;font-size:9px;font-weight:700;line-height:16px;position:absolute;top:6px;right:20%;}"
    ".toast-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.3);z-index:9999;align-items:center;justify-content:center;}"
    ".toast-overlay.show{display:flex;}"
    ".toast-box{background:#fff;border-radius:16px;padding:20px 24px;min-width:270px;box-shadow:0 8px 32px rgba(0,0,0,.15);position:relative;text-align:center;}"
    ".toast-title{font-size:14px;font-weight:600;color:#111;}"
    ".toast-close{position:absolute;top:10px;right:14px;background:none;border:none;font-size:18px;color:#aaa;cursor:pointer;line-height:1;}"
    ".panel{display:none;}.panel.active{display:block;}"
    "</style></head><body>"));

  // ── Header ──
  client.print(F("<div class='hdr'>"
    "<div><div class='htx'>MANET Comm System</div><div class='hsx'>LoRa Mesh Network</div></div>"
    "<button class='rbtn' onclick='location.reload()'>"));
  client.print(ICO_REFRESH);
  client.print(F(" Refresh</button></div>"));

  // ════ PANEL 0 : FRIENDS ════
  client.print(F("<div id='p0' class='panel active'><div class='sl'>My Profile</div>"
    "<div class='pc'><div class='pav'>"));
  client.print(meInitial);
  client.print(F("</div><div><div class='pnm'>"));
  client.print(htmlEscape(meName));
  client.print(F("</div><div class='puid'>UID: "));
  client.print(myUID);
  client.print(F("</div></div><button class='ebtn' onclick='renameMe()'>"));
  client.print(ICO_EDIT);
  client.print(F(" Edit</button></div>"
    "<div class='sl'>Friends</div><div class='lg' id='userList'></div></div>"));

  // ════ PANEL 1 : GROUPS ════
  client.print(F("<div id='p1' class='panel'><div class='sl'>Create Group</div>"
    "<div class='cb'><input placeholder='Group name...' id='roomIn'>"
    "<button onclick='createRoom()'>Create</button></div>"
    "<div class='sl'>All Groups</div><div class='lg' id='roomsBox'></div></div>"));

  // ════ PANEL 2 : INVITATIONS ════
  client.print(F("<div id='p2' class='panel'><div class='sl'>Invitations</div>"
    "<div id='invitesBox'></div></div>"));

  // ════ PANEL 3 : SETTINGS ════
  client.print(F("<div id='p3' class='panel'><div class='sl'>Account</div>"
    "<div class='lg' style='margin:0 12px;'>"));

  // settings item: rename
  client.print(F("<div class='si' onclick='renameMe()'>"
    "<div class='sico' style='background:#e8f5e9;'>"));
  client.print(ICO_EDIT);
  client.print(F("</div><div style='flex:1;'><div class='slb'>Change Display Name</div>"
    "<div class='ssb'>Currently: "));
  client.print(htmlEscape(meName));
  client.print(F("</div></div><div>"));
  client.print(ICO_CHEVRON);
  client.print(F("</div></div>"));

  // settings item: logout
  client.print(F("<div class='si' onclick='location.href=\"/logout\"'>"
    "<div class='sico' style='background:#e8f0ff;'>"));
  client.print(ICO_LOGOUT);
  client.print(F("</div><div style='flex:1;'><div class='slb'>Logout</div>"
    "<div class='ssb'>End current session</div></div><div>"));
  client.print(ICO_CHEVRON);
  client.print(F("</div></div></div>"));

  // danger zone
  client.print(F("<div class='sl'>Danger Zone</div>"
    "<div class='lg' style='margin:0 12px;border-color:#fdd;'>"));

  // settings item: delete account
  client.print(F("<div class='si' onclick='confirmDelete()'>"
    "<div class='sico dico'>"));
  client.print(ICO_USERX);
  client.print(F("</div><div style='flex:1;'><div class='slb dlb'>Delete My Account</div>"
    "<div class='ssb'>Remove all personal data</div></div><div>"));
  client.print(ICO_CHEVRON);
  client.print(F("</div></div>"));

  // settings item: factory reset
  client.print(F("<div class='si' onclick='adminReset()'>"
    "<div class='sico dico'>"));
  client.print(ICO_WARN);
  client.print(F("</div><div style='flex:1;'><div class='slb dlb'>Factory Reset</div>"
    "<div class='ssb'>Administrator &middot; Wipe entire database</div></div><div>"));
  client.print(ICO_CHEVRON);
  client.print(F("</div></div></div></div>"));

// ── Bottom Nav ──
  client.print(F("<div class='toast-overlay' id='toast'>"
    "<div class='toast-box'>"
    "<button class='toast-close' onclick='closeToast()'>&#x2715;</button>"
    "<div class='toast-title' id='toastMsg'></div>"
    "</div></div>"));
  client.print(F("<div class='nav'>"));
  client.print(F("<button class='ni active' onclick='showTab(0)' id='n0'>"));
  client.print(ICO_NAV_USERS);
  client.print(F("Friends</button>"));
  client.print(F("<button class='ni' onclick='showTab(1)' id='n1'>"));
  client.print(ICO_NAV_MSG);
  client.print(F("Groups</button>"));
  client.print(F("<button class='ni' onclick='showTab(2)' id='n2'>"));
  client.print(ICO_NAV_BELL);
  client.print(F("Invites<span id='invBadge' class='nb' style='display:none;'>0</span></button>"));
  client.print(F("<button class='ni' onclick='showTab(3)' id='n3'>"));
  client.print(ICO_NAV_SET);
  client.print(F("Settings</button></div>"));

// ── Script ──
  client.print(F("<script>const MY_UID='"));
  client.print(myUID);
  client.print(F("';\n"
      "function showTab(i){"
        "document.querySelectorAll('.panel').forEach((p,x)=>p.classList.toggle('active',x===i));"
        "document.querySelectorAll('.ni').forEach((n,x)=>n.classList.toggle('active',x===i));}\n"
      "function renameMe(){let n=prompt('New Display Name:');if(n)location.href='/setname?name='+encodeURIComponent(n);}\n"
      "let toastTimer;\n"
      "function showToast(msg){document.getElementById('toastMsg').innerText=msg;document.getElementById('toast').classList.add('show');}\n"
      "function closeToast(){document.getElementById('toast').classList.remove('show');}\n"
      "async function createRoom(){let n=document.getElementById('roomIn').value.trim();if(!n)return;await fetch('/create?name='+encodeURIComponent(n)+'&owner_uid='+MY_UID);document.getElementById('roomIn').value='';showToast('Group created successfully');refresh();}\n"
      "function confirmDelete(){if(confirm('Are you sure? This will delete all your data.'))location.href='/delete_self';}\n"
      "function adminReset(){if(confirm('WARNING: Wipe entire database?'))location.href='/reset_db';}\n"));

  // ICO_MAIL ฝังใน JS แบบ escape เพื่อใช้ใน invite card ที่ render ทีหลัง
    client.print(F("const ICOMAIL=`"));
    client.print(ICO_MAIL);
    client.print(F("`;\n"
    "const ICOCHR='<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#ccc\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><polyline points=\"9 18 15 12 9 6\"/></svg>';\n"
    "async function refresh(){try{\n"
    // userList
    "let ul=await(await fetch('/api/userlist')).json();let uH='';\n"
    "if(ul.users)ul.users.forEach(u=>{"
      "uH+=`<div class='li' data-uid='${u.uid}'>`"
        "+`<div class='av' style='background:#3d5a80;'>${u.name[0]||'?'}"
          "<div class='ad ${u.online?'on':'off'}'></div></div>`"
        "+`<div class='ib'><div class='in'>${u.name}</div><div class='is'>UID: ${u.uid}</div></div>`"
        "+ICOCHR+'</div>';"
    "});\n"
    "document.getElementById('userList').innerHTML="
      "uH||'<div style=\"padding:40px;text-align:center;color:#ccc;font-size:13px;\">No friends yet</div>';\n"
    // roomsBox
    "let rl=await(await fetch('/api/rooms')).json();let rH='';\n"
    "if(rl.rooms)rl.rooms.forEach(r=>{"
      "let badge,action;\n"
      "if(r.owner_uid==MY_UID){"
        "badge=\"<span class='badge bo'>Owner</span>\";"
        "action=`<button onclick=\"event.stopPropagation();if(confirm('Delete group?'))location.href='/delete?name=${encodeURIComponent(r.name)}'\" class='btn bd2'>Delete</button>`;}\n"
      "else if(r.status==='member'){badge=\"<span class='badge bm'>Member</span>\";action=\"<div class='bph'></div>\";}\n"
      "else if(r.status==='pending'){badge=\"<span class='badge bp'>Pending</span>\";action=\"<div class='bph'></div>\";}\n"
      "else{badge=\"<span class='badge bn'>None</span>\";"
        "action=`<button onclick=\"event.stopPropagation();location.href='/request?name=${encodeURIComponent(r.name)}'\" class='btn bj'>Join</button>`;}\n"
      "rH+=`<div class='li' onclick=\"location.href='/room?name=${encodeURIComponent(r.name)}'\">`"
        "+`<div class='av' style='background:#3d5a80;'>G</div>`"
        "+`<div class='ib'><div class='in'>${r.name}</div><div class='is'>${r.members} members</div></div>`"
        "+`<div class='ga'>${badge}${action}</div></div>`;"
    "});\n"
    "document.getElementById('roomsBox').innerHTML="
      "rH||'<div style=\"padding:40px;text-align:center;color:#ccc;font-size:13px;\">No groups</div>';\n"
    // invitesBox
    "let iv=await(await fetch('/api/invites')).json();let iH='';let b=document.getElementById('invBadge');\n"
    "if(iv.invites&&iv.invites.length>0){"
      "b.style.display='inline';b.innerText=iv.invites.length;\n"
      "iv.invites.forEach(it=>{"
        "iH+=`<div class='icard'><div class='itop'><div class='iico'>${ICOMAIL}</div>`"
            "+`<div><div class='inm'>${it.room}</div><div class='ifr'>From: ${it.inviter}</div></div></div>`"
            "+`<div class='ibtns'>"
              "<button class='bacc' onclick=\"location.href='/invite_accept?idx=${it.idx}'\">Accept</button>`"
            "+`<button class='bdec' onclick=\"location.href='/invite_decline?idx=${it.idx}'\">Decline</button>"
            "</div></div>`;"
      "});"
    "}else{b.style.display='none';iH='<div style=\"padding:40px;text-align:center;color:#ccc;font-size:13px;\">No invitations</div>';}\n"
    "document.getElementById('invitesBox').innerHTML=iH;\n"
    "}catch(e){console.error(e)}}\n"
    "document.getElementById('userList').addEventListener('click',function(e){"
      "let item=e.target.closest('.li');if(!item)return;"
      "let uid=item.getAttribute('data-uid');if(uid)location.href='/dm?to='+uid;});\n"
    "setInterval(refresh,3000);refresh();"
    "</script></body></html>"));
}

void sendRoomPage(WiFiClient& client, const String& myUID, const String& roomName) {
  int ridx = findRoom(roomName);
  if (ridx < 0) { httpRedirect(client, "/"); return; }

  uint32_t currentUid;
  bool uidOk = parseUidSafe(myUID, currentUid);

  bool member = false;
  if (uidOk) {
    member = isMemberUid(ridx, currentUid);
  }
  bool pending = false;
  if (uidOk) {
    for (int i = 0; i < rooms[ridx].requestCount; i++) {
      if (rooms[ridx].requests[i] == currentUid) {
        pending = true;
        break;
      }
    }
  }

  // --- Case: Non-member ---
  if (!member) {
    bool pending = false;
    for (int i = 0; i < rooms[ridx].requestCount; i++) {
       if (rooms[ridx].requests[i] == currentUid) { // เช็คด้วย UID เลข
          pending = true;
          break;
       }
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-type:text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>");
    client.println("<style>body{font-family:sans-serif;background:#f3f3f3;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}");
    client.println(".card{background:#fff;padding:30px;border-radius:20px;text-align:center;box-shadow:0 4px 12px rgba(0,0,0,0.1);max-width:300px;}");
    client.println(".btn{display:block;width:100%;padding:12px;margin-top:10px;border-radius:25px;border:none;font-weight:bold;cursor:pointer;}");
    client.println(".a{background:#00B900;color:#fff;} .b{background:#eee;color:#333;}</style></head><body>");
    client.println("<div class='card'><h3>" + htmlEscape(roomName) + "</h3><p style='color:#888'>You are not a member of this group.</p>");
    
    if (pending) {
      client.println("<button class='btn b' disabled>Waiting for approval...</button>");
    } else {
      client.println("<button class='btn a' style='width:100%; margin-top:10px;' onclick=\"location.href='/request?name=" + roomName + "'\">Request Join</button>");
    }
    client.println("<button class='btn b' onclick=\"location.href='/'\">Back to Home</button></div></body></html>");
    return;
    Serial.print("Access Denied! MyUID: "); Serial.print(currentUid);
    Serial.print(" RoomOwner: "); Serial.println(rooms[ridx].ownerUid);
  }

  // --- กรณีเป็นสมาชิก (หน้าแชทหลัก) ---
  String titleRoom = roomName;
  bool isDM = isDmRoomName(roomName);
  if (isDM) {
    uint32_t otherUid = 0;
    for (int i = 0; i < rooms[ridx].memberUidCount; i++) {
      if (rooms[ridx].memberUids[i] != currentUid) {
        otherUid = rooms[ridx].memberUids[i];
        break;
      }
    }
    titleRoom = (otherUid != 0) ? getDisplayNameByUid(otherUid) : "Chat";
  }

  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.println("<!doctype html><html><head><meta charset='utf-8'>");
  client.println("<meta name='viewport' content='width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no'>");
  client.println("<style>");
  client.println("body{margin:0;font-family:Helvetica,Arial,sans-serif;background:#F4F1EC;display:flex;flex-direction:column;height:100vh;}");
  client.println(".header{background:#FFFFFF;color:#2B2B2B;padding:10px 15px;display:flex;align-items:center;gap:10px;position:sticky;top:0;z-index:3000;border-bottom:1px solid #E3DED6;}");
  client.println(".back-btn{text-decoration:none;color:#2B2B2B;font-size:20px;margin-right:5px;}");
  client.println(".room-info{flex:1;min-width:0;}");
  client.println(".room-name{font-weight:bold;font-size:16px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}");
  client.println(".room-sub{font-size:11px;opacity:0.7;}");
  client.println(".menu-btn { background: none; border: none; color: #000000; font-size: 32px; cursor: pointer; padding: 0 5px; line-height: 1; font-weight: bold; }");
  client.println(".msgs{flex:1;overflow-y:auto;padding:15px;display:flex;flex-direction:column;background:#F4F1EC;}");
  client.println(".bubble{margin:6px 0;display:flex;}");
  client.println(".bubble span{padding:8px 10px;border-radius:14px;display:inline-block;max-width:82%;border:1px solid #eee;}");
  client.println(".bubble.user.me{justify-content:flex-end;}");
  client.println(".bubble.user.me span{background:#9AD97C;color:#1F3B1A;border-bottom-right-radius:0;}");
  client.println(".bubble.user.other{justify-content:flex-start;}");
  client.println(".bubble.user.other span{background:#ffffff;border-bottom-left-radius:0;}");
  client.println(".sender-label{font-size:11px;opacity:.75;margin-bottom:4px;}");
  client.println(".msg-text{font-size:14px;}");
  client.println(".bubble.sys { justify-content: center; margin: 10px 0; }");
  client.println(".bubble.sys span { background: rgba(0,0,0,0.15); color: #fff; font-size: 11px; padding: 4px 14px; border-radius: 20px; border: none; font-weight: 300; letter-spacing: 0.3px; }");
  client.println(".sys-label { display: none; }");
  client.println(".input-area{background:#F4F1EC;padding:10px;display:flex;gap:8px;box-sizing:border-box;width:100%;position:sticky;bottom:0;}");
  client.println(".input-area input{flex:1;min-width:0;height:44px;padding:0 12px;border-radius:22px;border:1px solid #D8D2C8;font-size:16px;}");
  client.println(".input-area button{height:44px;padding:0 16px;border:none;border-radius:22px;background:#7BC96F;color:#1F3B1A;font-weight:bold;cursor:pointer;flex:0 0 auto;white-space:nowrap;}");
  client.println(".side{position:fixed;top:0;right:-100%;width:80%;max-width:360px;height:100%;background:#fff;z-index:2001;transition:right 0.3s;padding:70px 20px 20px;box-sizing:border-box;overflow-y:auto;}");
  client.println(".side.show{right:0;}");
  client.println(".backdrop{position:fixed;inset:0;background:rgba(0,0,0,0.5);z-index:2000;opacity:0;pointer-events:none;transition:opacity 0.3s;}");
  client.println(".backdrop.show{opacity:1;pointer-events:auto;}");
  client.println(".dot { height: 10px; width: 10px; border-radius: 50%; display: inline-block; margin-right: 8px; }");
  client.println(".online { background-color: #2ecc71; } .offline { background-color: #95a5a6; }");
  client.println("</style></head><body>");

  client.println("<div class='header'>");
  client.println("  <a href='/' class='back-btn'>❮</a>");
  client.println("  <div class='room-info'>");
  client.println("    <div class='room-name'>" + htmlEscape(titleRoom) + "</div>");
  client.println("    <div class='room-sub'>UID: <span id='meUIDDisplay'>" + myUID + "</span></div>");
  client.println("  </div>");
  client.println("  <button class='menu-btn' onclick='toggleSide(true)'>☰</button>");
  client.println("</div>");

  client.println("<div class='msgs' id='msgs'>" + rooms[ridx].logHtml + "</div>");

  client.println("<form id='sendForm' class='input-area'>");
  client.println("  <input type='hidden' name='name' value='" + htmlEscape(roomName) + "'>");
  client.println("  <input name='msg' id='msg' placeholder='Type a message...' required autocomplete='off'>");
  client.println("  <button type='submit'>Send</button>");
  client.println("</form>");

  client.println("<div id='backdrop' class='backdrop' onclick='toggleSide(false)'></div>");
  client.println("<div id='side' class='side'>");
  client.println("  <h3>Chat Settings</h3><hr style='border:0; border-top:1px solid #eee;'>");
  client.println("  <div style='margin-top:15px;'><b style='font-size:13px; color:#666;'>Members:</b><div id='membersBox' style='font-size:14px; margin-top:5px;'>Loading...</div></div>");
  client.println("  <div id='reqBox' style='margin-top:20px; border-top:1px solid #eee; padding-top:15px; display:none;'><b style='font-size:13px; color:#666;'>Join Requests:</b><div id='reqList' style='margin-top:8px;'></div></div>");
  client.println("  <div id='inviteArea' style='margin-top:20px; border-top:1px solid #eee; padding-top:15px; display:none;'><b style='font-size:12px; color:#999;'>INVITE FRIENDS</b>");
  client.println("    <select id='userSelect' style='width:100%; padding:10px; border-radius:12px; border:1px solid #ddd; margin-top:10px;'><option value=''>-- Select Friend --</option></select>");
  client.println("    <button style='width:100%; padding:10px; background:#7BC96F; color:#fff; border:none; border-radius:12px; font-weight:bold; margin-top:8px;' onclick='sendInvite()'>Send Invitation</button></div>");
  
  if (!isDM && roomName != "general") {
    client.println("  <button style='width:100%; margin-top:25px; color:#ff4d4d; background:none; border:1px solid #ff4d4d; border-radius:10px; padding:10px; font-weight:bold;' onclick='leaveGroup()'>Leave Group</button>");
  }
  client.println("  <button style='width:100%; margin-top:15px; color:#888; background:none; border:none;' onclick='toggleSide(false)'>Close</button>");
  client.println("</div>");

  client.println("<script>");
  client.println("const MY_UID = '" + myUID + "';");
  client.println("const ROOM = '" + roomName + "';");
  
  // --- ฟังก์ชันหลัก (ตาม UID) ---
  client.println("async function acceptReq(uid){ await fetch('/approve?name='+encodeURIComponent(ROOM)+'&uid='+uid+'&ok=1'); refresh(); }");
  client.println("async function rejectReq(uid){ await fetch('/approve?name='+encodeURIComponent(ROOM)+'&uid='+uid+'&ok=0'); refresh(); }");
  client.println("async function sendInvite(){ const sel=document.getElementById('userSelect'); if(!sel.value)return; await fetch('/invite?name='+encodeURIComponent(ROOM)+'&to='+encodeURIComponent(sel.value)); alert('Sent'); sel.value=''; }");
  client.println("function toggleSide(s){ document.getElementById('side').classList.toggle('show',s); document.getElementById('backdrop').classList.toggle('show',s); }");
  client.println("function markBubbles(){ document.querySelectorAll('.bubble.user').forEach(b=>{ const uid=(b.getAttribute('data-uid')||'').trim(); b.classList.toggle('me',uid===MY_UID); b.classList.toggle('other',uid!==MY_UID); }); }");

  client.println("async function refresh(){ try {");
  client.println("  const st = await fetch('/api/stats').then(r=>r.json());");
  client.println("  const logRes = await fetch('/api/log?name=' + encodeURIComponent(ROOM));");
  client.println("  const html = await logRes.text(); const box = document.getElementById('msgs');");
  client.println("  const isBottom = (box.scrollHeight - box.scrollTop - box.clientHeight) < 100;");
  client.println("  box.innerHTML = html; markBubbles(); if(isBottom) box.scrollTop = box.scrollHeight;");
  
  client.println("  const data = await fetch('/api/room?name='+encodeURIComponent(ROOM)).then(r=>r.json());");
  client.println("  document.getElementById('membersBox').innerHTML = data.members.map(m=>`<div style='display:flex;align-items:center;margin-bottom:5px;'><span class=\"dot ${m.online?'online':'offline'}\"></span>${m.name}</div>`).join('');");
  
  // จัดการ Join Requests (ใช้ UID)
  client.println("  const reqBox = document.getElementById('reqBox'); if(data.requests && data.requests.length > 0){");
  client.println("    reqBox.style.display='block'; document.getElementById('reqList').innerHTML = data.requests.map(r=>`<div style='display:flex;justify-content:space-between;align-items:center;padding:10px;background:#f9f9f9;border-radius:10px;margin-bottom:5px;'>${r.name}<div style='display:flex;gap:5px;'><button onclick='acceptReq(\"${r.uid}\")'>OK</button><button onclick='rejectReq(\"${r.uid}\")'>X</button></div></div>`).join('');");
  client.println("  } else reqBox.style.display='none';");

  // Invite Area
  client.println("  if(data.inviteCandidates && data.inviteCandidates.length && ROOM!=='General'){ document.getElementById('inviteArea').style.display='block'; const sel=document.getElementById('userSelect'); const cur=sel.value;");
  client.println("   sel.innerHTML = \"<option value=''>-- Select Friend --</option>\" + data.inviteCandidates.map(u => \"<option value=\\\"\" + u.uid + \"\\\">\" + u.name + \"</option>\").join(\"\");");
  client.println("  }");
  client.println("} catch(e) {} }");

  client.println("document.getElementById('sendForm').onsubmit = async (e) => { e.preventDefault(); const i=document.getElementById('msg'); const m=i.value.trim(); if(!m)return; i.value=''; await fetch('/send?name='+encodeURIComponent(ROOM)+'&msg='+encodeURIComponent(m)); refresh(); };");
  client.println("function leaveGroup(){ if(confirm('Leave Group?')) location.href='/leave?name='+encodeURIComponent(ROOM); }");
  client.println("setInterval(refresh, 5000); refresh();");
  client.println("</script></body></html>");

  Serial.print("myUID string = ");
  Serial.println(myUID);

}