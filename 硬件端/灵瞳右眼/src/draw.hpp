#ifndef DRAW_H
#define DRAW_H

#include <Arduino.h>
#include "screen.hpp"
#include "shared_val.hpp"
#include "color_map.hpp"
#include "BilinearInterpolation.hpp"
#include "mlx_drivers/mlx_probe.hpp"
#include "fusion.hpp"

const int biox = 32;
const int bioy = 24;
const int lines = 3;
uint16_t lineBuffer[32 * 10 * lines];
uint16_t dmaBuffer1[32 * 10 * lines];
uint16_t dmaBuffer2[32 * 10 * lines];
uint16_t *dmaBufferPtr = dmaBuffer1;
bool dmaBufferSel = 0;

inline void draw_cross(int x, int y, int len)
{
   tft.drawLine(x - len / 2, y, x + len / 2, y, tft.color565(255, 255, 255));
   tft.drawLine(x, y - len / 2, x, y + len / 2, tft.color565(255, 255, 255));
   tft.drawLine(x - len / 4, y, x + len / 4, y, tft.color565(0, 0, 0));
   tft.drawLine(x, y - len / 4, x, y + len / 4, tft.color565(0, 0, 0));
}

inline void show_local_temp(int x, int y, int cursor_size)
{
   draw_cross(x, y, 8);
   float *tempBuffer = (float *)pReadBuffer;
   float temp_xy = tempBuffer[(24 - y / (int)mlx_scale()) * 32 + (x / (int)mlx_scale())];
   int shift_x, shift_y;
   if (x < 140)
   {
      shift_x = 10;
   }
   else
   {
      shift_x = -60;
   }
   if (y < 120)
   {
      shift_y = 10;
   }
   else
   {
      shift_y = -20;
   }
   tft.setTextSize(2);
   tft.setCursor(x + shift_x, y + shift_y);
   tft.printf("%.2f", temp_xy);
}

inline void show_local_temp(int x, int y)
{
   show_local_temp(x, y, 2);
}

void draw()
{
   static int value;
   int now_y = 0;
   int cols = mlx_cols();
   int rows = mlx_rows();
   int scale = (int)mlx_scale();
   int render_w = cols * scale;
   int render_h = rows * scale;
   if (use_upsample)
   {
      init_interp_tables(cols, rows, scale);
      tft.startWrite();
      for (int y = 0; y < rows * scale; y++)
      {
         for (int x = 0; x < cols * scale; x++)
         {
            value = bio_linear_interpolation(x, render_h - 1 - y, mlx90640To_buffer, cols, rows);
            lineBuffer[x + now_y * render_w] = colormap[value];
         }
         now_y++;
         if (now_y == lines)
         {
            dmaBufferPtr = dmaBufferSel ? dmaBuffer2 : dmaBuffer1;
            dmaBufferSel = !dmaBufferSel;
            tft.pushImageDMA(0, y - now_y + 1, render_w, lines, lineBuffer, dmaBufferPtr);
            now_y = 0;
         }
      }
      if (now_y != 0)
      {
         dmaBufferPtr = dmaBufferSel ? dmaBuffer2 : dmaBuffer1;
         dmaBufferSel = !dmaBufferSel;
         tft.pushImageDMA(0, render_h - now_y, render_w, now_y, lineBuffer, dmaBufferPtr);
         now_y = 0;
      }
      tft.endWrite();
   }
   else
   {
      static uint16_t c565;
      tft.startWrite();
      for (int i = 0; i < rows; i++)
      {
         for (int j = 0; j < cols; j++)
         {
            c565 = colormap[mlx90640To_buffer[(rows - 1 - i) * cols + j]];
            tft.fillRect(j * scale, (i * scale), scale, scale, c565);
         }
      }
      tft.endWrite();
   }
   if (flag_trace_max == true)
   {
      draw_cross(y_max * scale, (rows - 1 - x_max) * scale, 8);
   }
   if (flag_show_cursor == true)
   {
      show_local_temp(test_point[0], test_point[1]);
   }
}

void freeze_handeler()
{
   if (flag_clear_cursor)
   {
      draw_fusion();
      flag_clear_cursor = false;
   }
   if (flag_show_cursor)
   {
      show_local_temp(test_point[0], test_point[1]);
   }
}

void preparing_loop()
{
   tft.setRotation(1);
   if (prob_status == PROB_CONNECTING)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 220, 30, TFT_BLACK);
      tft.printf("Triying to connect to MLX...");
      tft.setCursor(80, 190);
      tft.printf("address: %d\n", 0x33);
      delay(10);
   }
   else if (prob_status == PROB_INITIALIZING)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 200, 30, TFT_BLACK);
      tft.printf("MLX... is ready, initializing...\n");
      delay(10);
   }
   else if (prob_status == PROB_PREPARING)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 200, 30, TFT_BLACK);
      tft.printf("MLX... initializing... \n");
      delay(10);
   }
}

void refresh_status(uint8_t tp_status)
{
   if (tp_status == TP_CONNECTING)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 220, 30, TFT_BLACK);
      tft.printf("prepering touch panel...");
      delay(10);
   }
   else if (tp_status == TP_READY)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 220, 30, TFT_BLACK);
      tft.printf("touch panel connected...");
      delay(10);
   }
   else if (tp_status == TP_NOTFOUND)
   {
      tft.setCursor(40, 180);
      tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
      tft.setTextSize(1);
      tft.fillRect(40, 180, 220, 30, TFT_BLACK);
      tft.printf("touch panel notfound...");
      delay(10);
   }
}

void screen_loop()
{
   static unsigned long dt;
   if (upload_in_progress)
   {
      return;
   }
   if (!flag_in_photo_mode)
   {
      dt = millis();

      while (prob_lock != false)
      {
         delay(1);
      }
      prob_lock = true;
      float *tempBuffer = (float *)pReadBuffer;
      __sync_synchronize();
      float local_T_min = T_min_fp;
      float local_T_max = T_max_fp;
      int pixel_count = mlx_pixel_count();
      float denom = local_T_max - local_T_min;
      if (fabs(denom) < 0.001f)
         denom = 0.001f;
      for (int i = 0; i < pixel_count; i++)
      {
         int temp_val = (int)(180.0f * (tempBuffer[i] - local_T_min) / denom);
         if (temp_val < 0)
            temp_val = 0;
         if (temp_val > 179)
            temp_val = 179;
         mlx90640To_buffer[i] = (uint16_t)temp_val;
      }
      prob_lock = false;

      draw_fusion();

      dt = millis() - dt;
      if (flag_show_cursor == true)
      {
         float display_max = T_max_fp;
         float display_min = T_min_fp;
         float display_avg = T_avg_fp;
         tft.setTextSize(1);
         tft.setCursor(25, 220);
         tft.printf("max: %.2f  ", display_max);
         tft.setCursor(25, 230);
         tft.printf("min: %.2f  ", display_min);
         tft.setCursor(105, 220);
         tft.printf("avg: %.2f  ", display_avg);
         tft.setCursor(105, 230);
         tft.printf("bat: %d %% ", vbat_percent);
         tft.setCursor(180, 220);
         tft.printf("bright: %d  ", brightness);
         tft.setCursor(180, 230);
         tft.printf("%d ms", dt);
      }
   }
   else
   {
      freeze_handeler();
   }
}

#endif