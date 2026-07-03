#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_camera.h>
#include "camera.hpp"
#include "shared_val.hpp"
#include "network_stream.hpp"

#ifndef THERMAL_UPLOAD_URL
#define THERMAL_UPLOAD_URL "http://101.132.116.112/upload_thermal.php"
#endif

#ifndef CAMERA_UPLOAD_URL
#define CAMERA_UPLOAD_URL "http://101.132.116.112/upload_camera.php"
#endif

#ifndef ALERT_HTTP_TIMEOUT_MS
#define ALERT_HTTP_TIMEOUT_MS 8000
#endif

#ifndef CAMERA_UPLOAD_RETRY_DELAY_MS
#define CAMERA_UPLOAD_RETRY_DELAY_MS 250
#endif

#ifndef UPLOAD_DEVICE_NAME
#define UPLOAD_DEVICE_NAME "esp32_thermal"
#endif

inline bool thermal_values_valid(float tmax, float tmin, float tavg)
{
    return !(isnan(tmax) || isnan(tmin) || isnan(tavg) ||
             !isfinite(tmax) || !isfinite(tmin) || !isfinite(tavg));
}

inline String thermal_sensor_name()
{
    return is_90640 ? "MLX90640" : "MLX90641";
}

inline void thermal_find_hotspot_box(int &hotX, int &hotY, int &hotW, int &hotH)
{
    hotX = 0;
    hotY = 0;
    hotW = 0;
    hotH = 0;

    if (!flag_sensor_ok || pReadBuffer == nullptr)
        return;

    const uint8_t cols = is_90640 ? MLX90640_COLS : MLX90641_COLS;
    const uint8_t rows = is_90640 ? MLX90640_ROWS : MLX90641_ROWS;
    const float *data = (const float *)pReadBuffer;
    const float tmax = T_max_fp;

    if (cols <= 0 || rows <= 0 || !isfinite(tmax))
        return;

    const float hotspotThreshold = tmax - 4.0f;
    long sumX = 0;
    long sumY = 0;
    int count = 0;
    int minX = cols - 1;
    int minY = rows - 1;
    int maxX = 0;
    int maxY = 0;

    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < cols; ++x)
        {
            float v = data[y * cols + x];
            if (!isfinite(v))
                continue;
            if (v >= hotspotThreshold)
            {
                sumX += x;
                sumY += y;
                count++;
                if (x < minX)
                    minX = x;
                if (y < minY)
                    minY = y;
                if (x > maxX)
                    maxX = x;
                if (y > maxY)
                    maxY = y;
            }
        }
    }

    if (count <= 0)
    {
        int maxIdx = 0;
        float maxVal = data[0];
        for (int i = 1; i < cols * rows; ++i)
        {
            if (data[i] > maxVal)
            {
                maxVal = data[i];
                maxIdx = i;
            }
        }
        int mx = maxIdx % cols;
        int my = maxIdx / cols;
        minX = max(0, mx - 1);
        maxX = min((int)cols - 1, mx + 1);
        minY = max(0, my - 1);
        maxY = min((int)rows - 1, my + 1);
        sumX = mx;
        sumY = my;
        count = 1;
    }

    float cxCell = (float)sumX / count;
    float cyCell = (float)sumY / count;

    const int camW = 320;
    const int camH = 240;
    float cellW = (float)camW / cols;
    float cellH = (float)camH / rows;

    float boxCenterX = (cxCell + 0.5f) * cellW;
    float boxCenterY = (cyCell + 0.5f) * cellH;
    int baseW = max(22, (int)((maxX - minX + 1) * cellW * 0.95f));
    int baseH = max(22, (int)((maxY - minY + 1) * cellH * 0.95f));

    baseW = (int)(baseW * align_sx);
    baseH = (int)(baseH * align_sy);

    hotX = (int)(boxCenterX - baseW / 2.0f + align_tx);
    hotY = (int)(boxCenterY - baseH / 2.0f + align_ty);
    hotW = max(18, baseW);
    hotH = max(18, baseH);

    hotX = max(0, min(camW - 1, hotX));
    hotY = max(0, min(camH - 1, hotY));
    if (hotX + hotW > camW)
        hotW = camW - hotX;
    if (hotY + hotH > camH)
        hotH = camH - hotY;
}

inline bool upload_thermal_monitor_once(const char *trigger)
{
    if (!thermal_wifi_is_connected())
    {
        Serial.println("[ThermalUpload] WiFi not connected");
        return false;
    }

    if (!flag_sensor_ok || pReadBuffer == nullptr)
    {
        Serial.println("[ThermalUpload] Sensor data not ready");
        return false;
    }

    float tmax = T_max_fp;
    float tmin = T_min_fp;
    float tavg = T_avg_fp;
    if (!thermal_values_valid(tmax, tmin, tavg))
    {
        Serial.println("[ThermalUpload] Invalid thermal values");
        return false;
    }

    const uint8_t cols = is_90640 ? MLX90640_COLS : MLX90641_COLS;
    const uint8_t rows = is_90640 ? MLX90640_ROWS : MLX90641_ROWS;

    int hotX, hotY, hotW, hotH;
    thermal_find_hotspot_box(hotX, hotY, hotW, hotH);

    String payload;
    payload.reserve(1024);
    payload += "{";
    payload += "\"ok\":true,";
    payload += "\"device\":\"" + String(UPLOAD_DEVICE_NAME) + "\",";
    payload += "\"sensor\":\"" + thermal_sensor_name() + "\",";
    payload += "\"trigger\":\"" + String(trigger ? trigger : "manual") + "\",";
    payload += "\"t_max\":" + String(tmax, 2) + ",";
    payload += "\"t_min\":" + String(tmin, 2) + ",";
    payload += "\"t_avg\":" + String(tavg, 2) + ",";
    payload += "\"width\":" + String(cols) + ",";
    payload += "\"height\":" + String(rows) + ",";
    payload += "\"filter\":\"Classic\",";
    payload += "\"fusion_alpha\":" + String((int)fusion_alpha) + ",";
    payload += "\"align_tx\":" + String(align_tx, 2) + ",";
    payload += "\"align_ty\":" + String(align_ty, 2) + ",";
    payload += "\"align_sx\":" + String(align_sx, 4) + ",";
    payload += "\"align_sy\":" + String(align_sy, 4) + ",";
    payload += "\"align_ang\":" + String(align_ang, 2) + ",";
    payload += "\"hot_x\":" + String(hotX) + ",";
    payload += "\"hot_y\":" + String(hotY) + ",";
    payload += "\"hot_w\":" + String(hotW) + ",";
    payload += "\"hot_h\":" + String(hotH) + ",";
    payload += "\"updated_at\":\"ESP32 Live\",";
    payload += "\"timestamp\":" + String((unsigned long)millis());
    payload += "}";

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(ALERT_HTTP_TIMEOUT_MS);
    http.setTimeout(ALERT_HTTP_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, THERMAL_UPLOAD_URL))
    {
        Serial.println("[ThermalUpload] http.begin failed");
        return false;
    }

    http.addHeader("Content-Type", "application/json");
    Serial.printf("[ThermalUpload] POST %s\n", THERMAL_UPLOAD_URL);
    int code = http.POST(payload);
    String resp = http.getString();
    Serial.printf("[ThermalUpload] HTTP %d, resp=%s\n", code, resp.c_str());
    http.end();

    return code >= 200 && code < 300;
}

inline bool upload_camera_snapshot_once_try(const char *trigger)
{
    if (!thermal_wifi_is_connected())
    {
        Serial.println("[CameraUpload] WiFi not connected");
        return false;
    }

    if (!camera_ok)
    {
        Serial.println("[CameraUpload] Camera not ready");
        return false;
    }

    if (!camera_frame_lock(1000))
    {
        Serial.println("[CameraUpload] Camera busy");
        return false;
    }

    camera_fb_t *fb_local = esp_camera_fb_get();
    if (!fb_local)
    {
        camera_frame_unlock();
        Serial.println("[CameraUpload] Capture failed");
        return false;
    }

    if (fb_local->format != PIXFORMAT_JPEG || fb_local->len < 100)
    {
        esp_camera_fb_return(fb_local);
        camera_frame_unlock();
        Serial.println("[CameraUpload] Invalid JPEG frame");
        return false;
    }

    float tmax = T_max_fp;
    float tmin = T_min_fp;
    float tavg = T_avg_fp;

    String url = String(CAMERA_UPLOAD_URL);
    url += "?device=" + String(UPLOAD_DEVICE_NAME);
    url += "&sensor=" + thermal_sensor_name();
    url += "&trigger=" + String(trigger ? trigger : "manual");
    url += "&t_max=" + String(tmax, 2);
    url += "&t_min=" + String(tmin, 2);
    url += "&t_avg=" + String(tavg, 2);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(ALERT_HTTP_TIMEOUT_MS);
    http.setTimeout(ALERT_HTTP_TIMEOUT_MS + 4000);
    http.setReuse(false);

    bool ok = false;
    if (http.begin(client, url))
    {
        http.addHeader("Content-Type", "image/jpeg");
        http.addHeader("Connection", "close");
        Serial.printf("[CameraUpload] POST %s (len=%u)\n", url.c_str(), (unsigned int)fb_local->len);
        int code = http.POST(fb_local->buf, fb_local->len);
        String resp = http.getString();
        Serial.printf("[CameraUpload] HTTP %d, resp=%s\n", code, resp.c_str());
        ok = (code >= 200 && code < 300);
        http.end();
    }
    else
    {
        Serial.println("[CameraUpload] http.begin failed");
    }

    esp_camera_fb_return(fb_local);
    camera_frame_unlock();
    return ok;
}

inline bool upload_camera_snapshot_once(const char *trigger)
{
    bool ok = upload_camera_snapshot_once_try(trigger);
    if (!ok)
    {
        Serial.println("[CameraUpload] First attempt failed, retrying once...");
        delay(CAMERA_UPLOAD_RETRY_DELAY_MS);
        ok = upload_camera_snapshot_once_try(trigger);
    }
    return ok;
}

inline bool trigger_snapshot_upload_once(const char *trigger = "button")
{
    upload_in_progress = true;
    delay(20);

    bool thermalOk = upload_thermal_monitor_once(trigger);
    delay(120);
    bool cameraOk = upload_camera_snapshot_once(trigger);

    upload_in_progress = false;
    Serial.printf("[Upload] Done: thermal=%s, camera=%s\n",
                  thermalOk ? "OK" : "FAIL",
                  cameraOk ? "OK" : "FAIL");
    return thermalOk && cameraOk;
}
