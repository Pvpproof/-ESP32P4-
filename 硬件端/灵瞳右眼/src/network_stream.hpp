#pragma once

#include <Arduino.h>
#include <WiFi.h>

#ifndef WIFI_SSID
#define WIFI_SSID "Pvpproof"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "psy13162516512"
#endif

#ifndef WIFI_RETRY_INTERVAL_MS
#define WIFI_RETRY_INTERVAL_MS 10000UL
#endif

#ifndef WIFI_CONNECT_WAIT_MS
#define WIFI_CONNECT_WAIT_MS 15000UL
#endif

static unsigned long thermalLastWifiLogMs = 0;
static unsigned long thermalLastWifiBeginMs = 0;
static wl_status_t thermalLastWifiStatus = WL_IDLE_STATUS;

inline const char *wifi_status_name(wl_status_t st)
{
    switch (st)
    {
    case WL_NO_SHIELD:
        return "NO_SHIELD";
    case WL_IDLE_STATUS:
        return "IDLE";
    case WL_NO_SSID_AVAIL:
        return "NO_SSID";
    case WL_SCAN_COMPLETED:
        return "SCAN_DONE";
    case WL_CONNECTED:
        return "CONNECTED";
    case WL_CONNECT_FAILED:
        return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
        return "CONNECTION_LOST";
    case WL_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
    }
}

inline void wifi_begin_simple()
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.disconnect(true, false);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    thermalLastWifiBeginMs = millis();
    Serial.printf("[NET] WiFi begin issued, SSID: %s\n", WIFI_SSID);
}

inline void thermal_tcp_begin()
{
    wifi_begin_simple();
}

inline bool thermal_wifi_is_connected()
{
    return WiFi.status() == WL_CONNECTED;
}

inline bool thermal_wifi_consume_just_connected()
{
    static bool last_connected = false;
    bool now_connected = (WiFi.status() == WL_CONNECTED);
    bool just_connected = (!last_connected && now_connected);
    last_connected = now_connected;
    return just_connected;
}

inline void thermal_tcp_loop()
{
    wl_status_t st = WiFi.status();
    unsigned long now = millis();

    if (st != thermalLastWifiStatus)
    {
        thermalLastWifiStatus = st;
        Serial.printf("[NET] WiFi status changed: %s (%d)\n", wifi_status_name(st), (int)st);
        if (st == WL_CONNECTED)
        {
            Serial.printf("[NET] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
        }
    }

    if (st != WL_CONNECTED)
    {
        if (now - thermalLastWifiLogMs >= 3000)
        {
            thermalLastWifiLogMs = now;
            Serial.printf("[NET] Waiting for WiFi... Status: %d\n", (int)st);
        }

        bool connectTimedOut = (thermalLastWifiBeginMs > 0) && (now - thermalLastWifiBeginMs >= WIFI_CONNECT_WAIT_MS);
        bool retryIntervalHit = (thermalLastWifiBeginMs == 0) || (now - thermalLastWifiBeginMs >= WIFI_RETRY_INTERVAL_MS);

        if (connectTimedOut || retryIntervalHit)
        {
            wifi_begin_simple();
        }
    }
}
