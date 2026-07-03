#pragma once

#include <Arduino.h>
#include "shared_val.hpp"

#define BUTTON_PIN 0
#define BTN_LONG_PUSH_T 1000
#define BUTTON_TRIG_LEVEL LOW
#define BUTTON_UPLOAD_COOLDOWN_MS 3000UL

inline void button_init()
{
    pinMode(BUTTON_PIN, INPUT_PULLUP);
}

inline void func_button_long_pushed()
{
    Serial.println("[Button] Long press ignored");
}

inline void func_button_pushed()
{
    static unsigned long last_upload_request_ms = 0;
    unsigned long now = millis();

    if (upload_in_progress)
    {
        Serial.println("[Button] Upload already in progress, ignore");
        return;
    }

    if (now - last_upload_request_ms < BUTTON_UPLOAD_COOLDOWN_MS)
    {
        Serial.println("[Button] Upload cooldown, ignore");
        return;
    }

    last_upload_request_ms = now;
    upload_request_pending = true;
    Serial.println("[Button] Snapshot upload requested");
}

inline void button_loop()
{
    static unsigned long btn_pushed_start_time = 0;
    static bool btn_pushed = false;
    static bool btn_long_pushed = false;

    if (digitalRead(BUTTON_PIN) == BUTTON_TRIG_LEVEL)
    {
        if (millis() - btn_pushed_start_time >= BTN_LONG_PUSH_T)
        {
            if (!btn_long_pushed)
            {
                func_button_long_pushed();
                btn_long_pushed = true;
            }
        }
        delay(5);
        if (digitalRead(BUTTON_PIN) == BUTTON_TRIG_LEVEL)
        {
            btn_pushed = true;
        }
    }
    else
    {
        btn_pushed_start_time = millis();
        if (btn_pushed)
        {
            if (!btn_long_pushed)
            {
                func_button_pushed();
            }
        }
        btn_pushed = false;
        btn_long_pushed = false;
    }
}
