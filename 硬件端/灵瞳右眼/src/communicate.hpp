#pragma once
#include <Arduino.h>
#include "screen.hpp"
#include "fusion.hpp"
#include "file_system.hpp"
#include "camera.hpp"
#include "network_stream.hpp"

void serial_start()
{
    Serial.begin(115200);
    while (!Serial)
    {
        ;
    }
    Serial.println("Serial communication initialized.");
    fs_init();
    load_align_params();
    load_filter_mode_from_eeprom();
}

void camera_apply_flip()
{
    if (!camera_ok)
        return;
    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
        s->set_vflip(s, camera_vflip);
        s->set_hmirror(s, camera_hmirror);
    }
}

void camera_set_flip(bool vflip, bool hmirror)
{
    camera_vflip = vflip;
    camera_hmirror = hmirror;
    camera_apply_flip();
    save_align_params();
}

void serial_loop()
{
    if (Serial.available() > 0)
    {
        String input = Serial.readStringUntil('\n');
        input.trim();

        if (input == "h")
        {
            Serial.println("\r\n=========================================");
            Serial.println("     ESP32 Overlay Fusion Console       ");
            Serial.println("=========================================");
            Serial.println("[ General Commands ]");
            Serial.println("  h                     - Show this help message");
            Serial.println("  echo <message>        - Echo the input message back to you");
            Serial.println("");
            Serial.println("[ Screen Control ]");
            Serial.println("  screen on             - Turn on the screen smoothly");
            Serial.println("  screen off            - Turn off the screen smoothly");
            Serial.println("  screen brightness <X> - Set brightness level (X: 5~255)");
            Serial.println("");
            Serial.println("[ Overlay Filter ]");
            Serial.println("  filter                - Show current filter");
            Serial.println("  filter next           - Switch to next filter");
            Serial.println("  filter <0-3>          - Set filter:");
            Serial.println("                          0=Classic");
            Serial.println("                          1=White Hot");
            Serial.println("                          2=Black Hot");
            Serial.println("                          3=Edge Hot");
            Serial.println("");
            Serial.println("[ File System Commands ]");
            Serial.println("  ls                    - List files in root directory");
            Serial.println("  rm <filename>         - Remove a file");
            Serial.println("  df                    - Show disk usage information");
            Serial.println("");
            Serial.println("[ Fusion Configuration ]");
            Serial.println("  set_align tx ty sx sy ang - Set alignment params");
            Serial.println("  get_align             - Get current alignment params");
            Serial.println("  set_alpha val         - Set fusion transparency (0-255)");
            Serial.println("  toggle_vflip          - Toggle vertical flip");
            Serial.println("  toggle_hflip          - Toggle horizontal flip");
            Serial.println("  net                   - Show WiFi status");
            Serial.println("=========================================\r\n");
        }
        else if (input.startsWith("echo "))
        {
            String message = input.substring(5);
            Serial.println("Echo: " + message);
        }
        else if (input.startsWith("screen ") || input.startsWith("color_reverse "))
        {
            screen_cli(input);
        }
        else if (input == "mode")
        {
            Serial.println("[Mode] Fixed: Thermal Overlay (4)");
        }
        else if (input == "mode next" || input.startsWith("mode "))
        {
            Serial.println("[Mode] Fixed to Thermal Overlay only.");
        }
        else if (input == "filter")
        {
            Serial.printf("[Filter] Fixed: Classic (%d)\n", FILTER_CLASSIC);
        }
        else if (input == "filter next")
        {
            Serial.println("[Filter] Locked to Classic.");
        }
        else if (input.startsWith("filter "))
        {
            Serial.println("[Filter] Locked to Classic.");
        }
        else if (input.startsWith("ls") || input.startsWith("rm") || input.startsWith("df") || input.startsWith("cat"))
        {
            fs_cli(input);
        }
        else if (input.startsWith("set_align"))
        {
            handle_set_align(input);
        }
        else if (input.startsWith("get_align"))
        {
            Serial.printf("ALIGN %.2f %.2f %.3f %.3f %.2f %d\n",
                          align_tx, align_ty, align_sx, align_sy, align_ang, fusion_alpha);
        }
        else if (input.startsWith("set_alpha"))
        {
            handle_set_alpha(input);
        }
        else if (input.startsWith("toggle_vflip"))
        {
            camera_vflip = !camera_vflip;
            camera_apply_flip();
            save_align_params();
            Serial.printf("VFLIP:%d\n", camera_vflip);
        }
        else if (input.startsWith("toggle_hflip"))
        {
            camera_hmirror = !camera_hmirror;
            camera_apply_flip();
            save_align_params();
            Serial.printf("HFLIP:%d\n", camera_hmirror);
        }
        else if (input.startsWith("set_flip"))
        {
            int v = 0, h = 0;
            sscanf(input.c_str(), "set_flip %d %d", &v, &h);
            camera_set_flip(v == 1, h == 1);
            Serial.printf("OK: Flip set to V:%d H:%d\n", v, h);
        }
        else if (input == "net")
        {
            wl_status_t st = WiFi.status();
            Serial.printf("[NET] WiFi status: %d\n", (int)st);
            if (st == WL_CONNECTED)
            {
                Serial.printf("[NET] Local IP: %s\n", WiFi.localIP().toString().c_str());
            }
        }
        else if (input.startsWith("top"))
        {
            print_heap_usage();
        }
        else if (input.length() > 0)
        {
            Serial.println("Unknown command: '" + input + "'. Type 'h' for help.");
        }
    }
}
