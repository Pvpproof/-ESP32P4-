#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "screen.hpp"
#include "shared_val.hpp"
#include "color_map.hpp"
#include "BilinearInterpolation.hpp"
#include "camera.hpp"
#include "mlx_drivers/mlx_probe.hpp"
#include "network_stream.hpp"
#include <TJpg_Decoder.h>

static TFT_eSprite *fusion_sprite = nullptr;
static uint16_t *decode_target_buffer = nullptr;
static int decode_buffer_width = 320;
static int decode_buffer_height = 240;

bool camera_decode_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (decode_target_buffer == nullptr)
        return 0;
    if (y >= decode_buffer_height)
        return 0;

    for (int16_t row = 0; row < h; row++)
    {
        int16_t target_y = y + row;
        if (target_y >= decode_buffer_height)
            break;
        for (int16_t col = 0; col < w; col++)
        {
            int16_t target_x = x + col;
            if (target_x >= decode_buffer_width)
                break;
            decode_target_buffer[target_y * decode_buffer_width + target_x] = bitmap[row * w + col];
        }
    }
    return 1;
}

void init_fusion_sprite()
{
    if (fusion_sprite == nullptr)
    {
        fusion_sprite = new TFT_eSprite(&tft);
        if (fusion_sprite->createSprite(320, 240))
        {
            Serial.println("[Fusion] Fusion sprite created in PSRAM: 320x240");
        }
        else
        {
            Serial.println("[Fusion] Failed to create fusion sprite!");
            delete fusion_sprite;
            fusion_sprite = nullptr;
        }
    }
}

inline uint16_t swap565(uint16_t color)
{
    return (color << 8) | (color >> 8);
}

inline uint16_t alpha_blend(uint16_t bg_color, uint16_t fg_color, uint8_t alpha)
{
    if (alpha == 0)
        return bg_color;
    if (alpha == 255)
        return fg_color;

    bg_color = swap565(bg_color);
    fg_color = swap565(fg_color);

    uint8_t r1 = (bg_color >> 11) & 0x1F;
    uint8_t g1 = (bg_color >> 5) & 0x3F;
    uint8_t b1 = bg_color & 0x1F;
    uint8_t r2 = (fg_color >> 11) & 0x1F;
    uint8_t g2 = (fg_color >> 5) & 0x3F;
    uint8_t b2 = fg_color & 0x1F;

    uint8_t r = (r1 * (255 - alpha) + r2 * alpha) / 255;
    uint8_t g = (g1 * (255 - alpha) + g2 * alpha) / 255;
    uint8_t b = (b1 * (255 - alpha) + b2 * alpha) / 255;

    uint16_t result = (r << 11) | (g << 5) | b;
    return swap565(result);
}

inline uint16_t rgb565_gray(uint8_t gray)
{
    uint16_t r = (gray >> 3) & 0x1F;
    uint16_t g = (gray >> 2) & 0x3F;
    uint16_t b = (gray >> 3) & 0x1F;
    return (r << 11) | (g << 5) | b;
}

inline uint16_t thermal_color_from_value(int value, uint8_t filter_mode)
{
    if (value < 0)
        value = 0;
    if (value > 179)
        value = 179;

    switch (filter_mode)
    {
    case FILTER_WHITE_HOT:
    {
        uint8_t gray = (uint8_t)((value * 255) / 179);
        return swap565(rgb565_gray(gray));
    }
    case FILTER_BLACK_HOT:
    {
        uint8_t gray = 255 - (uint8_t)((value * 255) / 179);
        return swap565(rgb565_gray(gray));
    }
    case FILTER_EDGE_HOT:
    {
        if (value < 110)
            return swap565(0x0000);
        if (value < 135)
            return swap565(0x780F);
        if (value < 155)
            return swap565(0xF800);
        return swap565(0xFFE0);
    }
    case FILTER_CLASSIC:
    default:
        return swap565(colormap[value]);
    }
}

const char *get_filter_mode_name()
{
    switch (current_filter_mode)
    {
    case FILTER_CLASSIC:
        return "Classic";
    case FILTER_WHITE_HOT:
        return "White Hot";
    case FILTER_BLACK_HOT:
        return "Black Hot";
    case FILTER_EDGE_HOT:
        return "Edge Hot";
    default:
        return "Unknown";
    }
}

void save_filter_mode_to_eeprom()
{
    EEPROM.write(EEPROM_ADDR_FILTER_MODE, current_filter_mode);
    EEPROM.commit();
}

void load_filter_mode_from_eeprom()
{
    uint8_t mode = EEPROM.read(EEPROM_ADDR_FILTER_MODE);
    if (mode <= FILTER_EDGE_HOT)
        current_filter_mode = mode;
    else
        current_filter_mode = DEFAULT_FILTER_MODE;
}

void set_filter_mode(uint8_t mode)
{
    if (mode > FILTER_EDGE_HOT)
        mode = FILTER_CLASSIC;
    current_filter_mode = mode;
    save_filter_mode_to_eeprom();
    Serial.printf("[Filter] Switched to: %s (%d)\n", get_filter_mode_name(), current_filter_mode);
}

void next_filter_mode()
{
    set_filter_mode((current_filter_mode + 1) % 4);
}

void draw_thermal_overlay()
{
    static unsigned long dt = 0;
    unsigned long last_time = millis();

    if (mlx90640To_buffer == nullptr || !flag_sensor_ok)
    {
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(90, 110);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(2);
        tft.println("MLX Error");
        return;
    }
    if (!camera_ok)
    {
        tft.fillScreen(TFT_BLACK);
        tft.setCursor(80, 110);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(2);
        tft.println("Camera Error");
        return;
    }

    if (fusion_sprite == nullptr)
        init_fusion_sprite();
    if (fusion_sprite == nullptr)
        return;

    if (!camera_frame_lock(30))
        return;

    fb = esp_camera_fb_get();
    if (!fb || fb->format != PIXFORMAT_JPEG)
    {
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
        }
        camera_frame_unlock();
        return;
    }

    static uint16_t *cam_buffer = nullptr;
    if (cam_buffer == nullptr)
    {
        cam_buffer = (uint16_t *)ps_malloc(320 * 240 * sizeof(uint16_t));
        if (cam_buffer == nullptr)
        {
            Serial.println("[Fusion] Failed to allocate cam buffer!");
            esp_camera_fb_return(fb);
            fb = NULL;
            camera_frame_unlock();
            return;
        }
    }

    decode_target_buffer = cam_buffer;
    decode_buffer_width = 320;
    decode_buffer_height = 240;
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(true);
    TJpgDec.setCallback(camera_decode_callback);
    TJpgDec.drawJpg(0, 0, fb->buf, fb->len);

    esp_camera_fb_return(fb);
    fb = NULL;
    camera_frame_unlock();

    uint16_t *sprite_buffer = (uint16_t *)fusion_sprite->getPointer();
    if (sprite_buffer == nullptr)
        return;

    fusion_sprite->fillSprite(TFT_BLACK);

    int cols = is_90640 ? MLX90640_COLS : MLX90641_COLS;
    int rows = is_90640 ? MLX90640_ROWS : MLX90641_ROWS;
    int scale = 10;
    int render_w = cols * scale;
    int render_h = rows * scale;
    int offset_x = (320 - render_w) / 2;
    int offset_y = (240 - render_h) / 2;

    init_interp_tables(cols, rows, scale);
    for (int y = 0; y < render_h; y++)
    {
        int draw_y = offset_y + y;
        if (draw_y < 0 || draw_y >= 240)
            continue;
        for (int x = 0; x < render_w; x++)
        {
            int draw_x = offset_x + x;
            if (draw_x < 0 || draw_x >= 320)
                continue;
            int value = bio_linear_interpolation(x, render_h - 1 - y, mlx90640To_buffer, cols, rows);
            sprite_buffer[draw_y * 320 + draw_x] = thermal_color_from_value(value, current_filter_mode);
        }
    }

    float rad = -align_ang * PI / 180.0f;
    float cos_a = cos(rad);
    float sin_a = sin(rad);
    float inv_sx = (align_sx != 0) ? (1.0f / align_sx) : 1.0f;
    float inv_sy = (align_sy != 0) ? (1.0f / align_sy) : 1.0f;
    float dst_cx = 160.0f;
    float dst_cy = 120.0f;
    float src_cx = 160.0f;
    float src_cy = 120.0f;
    float m00 = cos_a * inv_sx;
    float m01 = sin_a * inv_sx;
    float m10 = -sin_a * inv_sy;
    float m11 = cos_a * inv_sy;
    float dx0 = 0.0f - dst_cx - align_tx;
    float dy0 = 0.0f - dst_cy - align_ty;
    float start_u = dx0 * m00 + dy0 * m01 + src_cx;
    float start_v = dx0 * m10 + dy0 * m11 + src_cy;

    for (int y = 0; y < 240; y++)
    {
        float u_f = start_u + y * m01;
        float v_f = start_v + y * m11;
        uint16_t *pSpriteLine = sprite_buffer + y * 320;
        for (int x = 0; x < 320; x++)
        {
            int u = (int)u_f;
            int v = (int)v_f;
            if (u >= 0 && u < 320 && v >= 0 && v < 240)
            {
                uint16_t cam_pixel = cam_buffer[v * 320 + u];
                uint16_t therm_pixel = *pSpriteLine;
                uint8_t alpha = fusion_alpha;

                if (current_filter_mode == FILTER_EDGE_HOT)
                {
                    if (therm_pixel == swap565(0x0000))
                        alpha = 0;
                    else if (alpha < 180)
                        alpha = 180;
                }
                else
                {
                    if (alpha < 50)
                        alpha = 50;
                }
                if (alpha > 255)
                    alpha = 255;

                *pSpriteLine = alpha_blend(cam_pixel, therm_pixel, alpha);
            }
            u_f += m00;
            v_f += m10;
            pSpriteLine++;
        }
    }

    tft.startWrite();
    fusion_sprite->pushSprite(0, 0);
    tft.endWrite();

    tft.setTextSize(1);

    tft.setCursor(10, 10);
    if (WiFi.status() == WL_CONNECTED)
    {
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.print("WiFi OK");
    }
    else
    {
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.print("WiFi NO");
    }

    tft.setCursor(80, 10);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    if (WiFi.status() == WL_CONNECTED)
    {
        tft.print(WiFi.localIP());
    }
    else
    {
        tft.print("No IP");
    }

    tft.setCursor(240, 10);
    if (upload_in_progress)
    {
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.print("UPLOAD");
    }
    else
    {
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.print("MONITOR");
    }

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(25, 220);
    tft.printf("max: %.2f  ", T_max_fp);
    tft.setCursor(25, 230);
    tft.printf("min: %.2f  ", T_min_fp);
    tft.setCursor(105, 220);
    tft.printf("avg: %.2f  ", T_avg_fp);
    tft.setCursor(105, 230);
    tft.printf("flt: %s", get_filter_mode_name());
    tft.setCursor(220, 220);
    tft.printf("a:%d  ", fusion_alpha);
    tft.setCursor(220, 230);
    tft.printf("%d ms", dt);

    dt = millis() - last_time;
}

const char *get_display_mode_name()
{
    return "Thermal Overlay";
}

void save_display_mode_to_eeprom()
{
    EEPROM.write(EEPROM_ADDR_DISPLAY_MODE, MODE_THERMAL_OVERLAY);
    EEPROM.commit();
}

DisplayMode load_display_mode_from_eeprom()
{
    return MODE_THERMAL_OVERLAY;
}

void set_display_mode(DisplayMode mode)
{
    current_display_mode = MODE_THERMAL_OVERLAY;
    save_display_mode_to_eeprom();
    init_fusion_sprite();
}

void next_display_mode()
{
    current_display_mode = MODE_THERMAL_OVERLAY;
}

void draw_fusion()
{
    draw_thermal_overlay();
}
