#pragma once
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include <Arduino.h>

#define PIN_BLK 4
#define PWM_CHANNEL 0    // LEDC PWM 通道
#define PWM_FREQ 20000   // PWM 频率 20kHz
#define PWM_RESOLUTION 8 // 8位分辨率 (0-255)

#include "shared_val.hpp" // 使用 shared_val.hpp 中定义的 brightness

static const uint16_t screenWidth = 320;
static const uint16_t screenHeight = 240;

TFT_eSPI tft = TFT_eSPI(screenHeight, screenWidth);

// ================= 亮度控制 =================
inline void set_brightness(int _brightness, bool remenber = true)
{
    if (_brightness > 255)
        _brightness = 255;
    if (_brightness < 0)
        _brightness = 0;

    ledcWrite(PWM_CHANNEL, _brightness);

    if (remenber)
    {
        brightness = _brightness;
    }
}

// ================= 亮起屏幕 =================
void smooth_on()
{
    ledcWrite(PWM_CHANNEL, 0);
    for (int i = 0; i < brightness; i++)
    {
        set_brightness(i, false);
        delay(2);
    }
}

// ================= 熄灭屏幕 =================
void smooth_off()
{
    for (int i = brightness; i >= 0; i--)
    {
        ledcWrite(PWM_CHANNEL, i);
        delay(2);
    }
}

// ================= 屏幕初始化主函数 =================
void screen_init()
{
    Serial.println("Initializing screen...");

    // 初始化 LEDC 高频 PWM
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(PIN_BLK, PWM_CHANNEL);
    ledcWrite(PWM_CHANNEL, brightness); // 直接按当前亮度点亮，不做渐变

    tft.init();
    tft.setRotation(1);
    tft.setSwapBytes(true);
    tft.invertDisplay(false);
    tft.initDMA();

    // 不显示开机图，直接清屏
    tft.fillScreen(TFT_BLACK);
}

// 支持的命令：screen off, screen on, screen brightness <value>
void screen_cli(String cmd)
{
    cmd.trim();

    if (cmd.equalsIgnoreCase("screen off"))
    {
        smooth_off();
        Serial.println("[Screen] Turned OFF smoothly.");
    }
    else if (cmd.equalsIgnoreCase("screen on"))
    {
        smooth_on();
        Serial.println("[Screen] Turned ON smoothly.");
    }
    else if (cmd.startsWith("screen brightness "))
    {
        String valueStr = cmd.substring(String("screen brightness ").length());
        int value = valueStr.toInt();
        set_brightness(value);

        Serial.print("[Screen] Brightness set to: ");
        Serial.println(value);
    }
    else
    {
        Serial.println("[Error] Unknown screen command: " + cmd);
    }
}