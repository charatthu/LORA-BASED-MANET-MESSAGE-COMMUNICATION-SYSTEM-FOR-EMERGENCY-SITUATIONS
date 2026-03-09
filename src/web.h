#ifndef WEB_H
#define WEB_H

#include <Arduino.h>
#include <WiFi.h>

// ===================== HTTP helpers =====================
void httpRedirect(WiFiClient& client, const String& location);
void httpOKText(WiFiClient& client, const String& text);
void httpFailText(WiFiClient& client, int code, const String& text);
void httpOKJson(WiFiClient& client, const String& json);
void httpOKHtml(WiFiClient& client, const String& html);

void ensureDefaultGroupAndMembership(const String& myIP);

// ===================== Pages =====================
void sendLoginPage(WiFiClient& client);
void sendHomePage(WiFiClient& client, const String& myIP);
void sendRoomPage(WiFiClient& client, const String& myIP, const String& roomName);

#endif // WEB_H
#define ICO_*
