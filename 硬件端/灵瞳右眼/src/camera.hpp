#pragma once
#include <Arduino.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_camera.h"
#include "screen.hpp"
#include "shared_val.hpp"
#include <Wire.h>
#include <TJpg_Decoder.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);

#define CAM_WIDTH 320
#define CAM_HEIGHT 240
#define PWDN_GPIO_NUM 17
#define RESET_GPIO_NUM 26
#define XCLK_GPIO_NUM 14
#define PCLK_GPIO_NUM 22
#define SIOD_GPIO_NUM 23
#define SIOC_GPIO_NUM 18
#define VSYNC_GPIO_NUM 21
#define HREF_GPIO_NUM 27
#define D0_GPIO_NUM 34
#define D1_GPIO_NUM 33
#define D2_GPIO_NUM 25
#define D3_GPIO_NUM 35
#define D4_GPIO_NUM 39
#define D5_GPIO_NUM 38
#define D6_GPIO_NUM 37
#define D7_GPIO_NUM 36
#define I2CPULL_UP 19
#define I2CPULL_UP_OPEN LOW

bool camera_ok = false;
camera_fb_t *fb = NULL;
static SemaphoreHandle_t camera_frame_mutex = NULL;

void camera_hard_reset()
{
    pinMode(I2CPULL_UP, OUTPUT);
    digitalWrite(I2CPULL_UP, I2CPULL_UP_OPEN);
    delay(200);
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, HIGH);
    delay(100);
    digitalWrite(PWDN_GPIO_NUM, LOW);
    delay(200);
}

inline bool camera_frame_lock(uint32_t timeout_ms = 50)
{
    if (camera_frame_mutex == NULL)
        return false;
    return xSemaphoreTake(camera_frame_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

inline void camera_frame_unlock()
{
    if (camera_frame_mutex != NULL)
    {
        xSemaphoreGive(camera_frame_mutex);
    }
}

void camera_init()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    camera_hard_reset();
    pinMode(I2CPULL_UP, OUTPUT);
    digitalWrite(I2CPULL_UP, I2CPULL_UP_OPEN);
    delay(800);

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = D0_GPIO_NUM;
    config.pin_d1 = D1_GPIO_NUM;
    config.pin_d2 = D2_GPIO_NUM;
    config.pin_d3 = D3_GPIO_NUM;
    config.pin_d4 = D4_GPIO_NUM;
    config.pin_d5 = D5_GPIO_NUM;
    config.pin_d6 = D6_GPIO_NUM;
    config.pin_d7 = D7_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 10000000;
    config.frame_size = FRAMESIZE_QVGA;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 28;

    if (psramFound())
    {
        Serial.printf("PSRAM found, using it for camera buffers\n");
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
        Serial.printf("PSRAM not found\n");
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    Serial.println("Initializing camera...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("Camera init failed with error: 0x%x", err);
        delay(100);
        camera_ok = false;
        esp_camera_deinit();
        return;
    }
    camera_ok = true;
    Serial.println("Camera init okay");

    if (camera_frame_mutex == NULL)
    {
        camera_frame_mutex = xSemaphoreCreateMutex();
        if (camera_frame_mutex == NULL)
        {
            Serial.println("[Camera] Failed to create frame mutex");
        }
        else
        {
            Serial.println("[Camera] Frame mutex ready");
        }
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID)
    {
        s->set_vflip(s, camera_vflip);
        s->set_hmirror(s, camera_hmirror);
        s->set_brightness(s, 1);
        s->set_saturation(s, -2);
    }
    if (s->id.PID == OV2640_PID)
    {
        s->set_vflip(s, camera_vflip);
        s->set_hmirror(s, camera_hmirror);
    }
}

void screen_draw_jpeg(const uint8_t *jpg_data, size_t len)
{
    if (jpg_data == nullptr || len == 0)
        return;
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(tft_output);
    tft.startWrite();
    TJpgDec.drawJpg(0, 0, jpg_data, len);
    tft.endWrite();
}

void camera_loop()
{
    if (!camera_ok)
        return;
    if (!camera_frame_lock(30))
        return;

    fb = esp_camera_fb_get();
    if (!fb)
    {
        Serial.printf("Camera capture failed\n");
        camera_frame_unlock();
        return;
    }
    if (fb->format == PIXFORMAT_JPEG)
    {
        screen_draw_jpeg(fb->buf, fb->len);
    }
    else
    {
        Serial.println("Non-JPEG frame received!");
    }
    esp_camera_fb_return(fb);
    fb = NULL;
    camera_frame_unlock();
}
