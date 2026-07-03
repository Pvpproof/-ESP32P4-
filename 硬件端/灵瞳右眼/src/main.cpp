#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include "communicate.hpp"
#include "screen.hpp"
#include "shared_val.hpp"
#include "draw.hpp"
#include "bat.hpp"
#include "button.hpp"
#include "camera.hpp"
#include "mlx_drivers/mlx_probe.hpp"
#include "network_stream.hpp"
#include "thermal_alert_upload.hpp"

static unsigned long g_last_wifi_log_ms = 0;
static unsigned long g_last_wifi_begin_ms = 0;
static wl_status_t g_last_wifi_status = WL_IDLE_STATUS;
static unsigned long g_last_hot_trigger_ms = 0;
static bool g_hot_trigger_armed = true;
static uint8_t g_hot_trigger_confirm_count = 0;
TaskHandle_t TaskSensorCore0;
TaskHandle_t TaskUploadCore1;

#ifndef HOT_TRIGGER_TEMP_C
#define HOT_TRIGGER_TEMP_C 50.0f
#endif

#ifndef HOT_TRIGGER_RESET_C
#define HOT_TRIGGER_RESET_C 45.0f
#endif

#ifndef HOT_TRIGGER_CONFIRM_FRAMES
#define HOT_TRIGGER_CONFIRM_FRAMES 2
#endif

#ifndef HOT_TRIGGER_COOLDOWN_MS
#define HOT_TRIGGER_COOLDOWN_MS 3000UL
#endif

inline const char *wifi_status_name_local(wl_status_t st)
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

inline void wifi_begin_main()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  g_last_wifi_begin_ms = millis();
  Serial.printf("[NET] WiFi begin issued, SSID: %s\n", WIFI_SSID);
}

inline void wifi_loop_main()
{
  wl_status_t st = WiFi.status();
  unsigned long now = millis();

  if (st != g_last_wifi_status)
  {
    g_last_wifi_status = st;
    Serial.printf("[NET] WiFi status changed: %s (%d)\n", wifi_status_name_local(st), (int)st);
    if (st == WL_CONNECTED)
    {
      Serial.printf("[NET] WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
    }
  }

  if (st != WL_CONNECTED)
  {
    if (now - g_last_wifi_log_ms >= 3000)
    {
      g_last_wifi_log_ms = now;
      Serial.printf("[NET] Waiting for WiFi... Status: %d\n", (int)st);
    }

    if (g_last_wifi_begin_ms == 0 || now - g_last_wifi_begin_ms >= 10000UL)
    {
      wifi_begin_main();
    }
  }
}

void setup_sensor_core0()
{
  sensor_power_on();
  EEPROM.begin(512);
  current_sensor = EEPROM.read(EEPROM_ADDR_SENSOR_TYPE);
  if (current_sensor > SENSOR_MLX90641)
  {
    current_sensor = SENSOR_MLX90640;
  }
  if (current_sensor == SENSOR_MLX90640)
  {
    is_90640 = true;
  }
  else
  {
    is_90640 = false;
  }

  delay(MXL_STARTUP_DELAY);
  sensor_power_on();

  bool mlx_ok = blocking_mlx_init_and_check(5);
  flag_sensor_ok = mlx_ok;
  prob_status = mlx_ok ? PROB_READY : PROB_PREPARING;

  if (mlx_ok)
  {
    for (int i = 0; i < 10; i++)
    {
      probe_loop_mlx();
      delay(5);
    }
  }
}

void loop_sensor_core0()
{
  if (flag_sensor_ok)
  {
    probe_loop_mlx();
  }
  delay(1);
}

void vTaskSensorCore0(void *pvParameters)
{
  setup_sensor_core0();
  for (;;)
  {
    loop_sensor_core0();
  }
}

void vTaskUploadCore1(void *pvParameters)
{
  delay(2500);
  wifi_begin_main();

  for (;;)
  {
    if (upload_worker_enabled)
    {
      wifi_loop_main();

      if (upload_request_pending && !upload_in_progress)
      {
        upload_request_pending = false;
        const char *trigger = "button";
        if (!g_hot_trigger_armed)
        {
          trigger = "auto_hot";
        }
        Serial.printf("[UploadTask] Processing pending upload request... trigger=%s\n", trigger);
        trigger_snapshot_upload_once(trigger);
      }
    }
    delay(20);
  }
}

void setup()
{
  serial_start();

  bat_init();
  button_init();
  screen_init();

  camera_init();
  delay(1500);

  xTaskCreatePinnedToCore(
      vTaskSensorCore0,
      "TaskSensorCore0",
      20000,
      NULL,
      1,
      &TaskSensorCore0,
      0);

  xTaskCreatePinnedToCore(
      vTaskUploadCore1,
      "TaskUploadCore1",
      12000,
      NULL,
      1,
      &TaskUploadCore1,
      1);

  current_display_mode = MODE_THERMAL_OVERLAY;
  current_filter_mode = FILTER_CLASSIC;
  init_fusion_sprite();

  bool cam_ok = camera_ok;
  if (!flag_sensor_ok || !cam_ok)
  {
    Serial.println("[Setup] Overlay needs both MLX and camera.");
  }

  delay(100);
  unsigned long wait_start = millis();
  while ((!camera_ok || !flag_sensor_ok) && millis() - wait_start < 3000)
  {
    preparing_loop();
    delay(10);
  }

  smooth_off();
  smooth_on();
  Serial.println("[Setup] Ready. Filter locked to Classic. Button triggers upload.");
}

void loop()
{
  current_filter_mode = FILTER_CLASSIC;

  serial_loop();
  bat_loop();
  button_loop();

  if (flag_sensor_ok)
  {
    float tmax = T_max_fp;
    unsigned long now = millis();

    if (isfinite(tmax))
    {
      bool overTrigger = (tmax >= HOT_TRIGGER_TEMP_C);

      if (overTrigger)
      {
        if (g_hot_trigger_confirm_count < 255)
        {
          g_hot_trigger_confirm_count++;
        }

        if (g_hot_trigger_armed && !upload_in_progress && !upload_request_pending &&
            g_hot_trigger_confirm_count >= HOT_TRIGGER_CONFIRM_FRAMES &&
            (g_last_hot_trigger_ms == 0 || now - g_last_hot_trigger_ms >= HOT_TRIGGER_COOLDOWN_MS))
        {
          upload_request_pending = true;
          g_last_hot_trigger_ms = now;
          g_hot_trigger_armed = false;
          Serial.printf("[HotTrigger] Auto upload triggered: T_max=%.2f C, confirm=%u\n", tmax, g_hot_trigger_confirm_count);
        }
      }
      else
      {
        g_hot_trigger_confirm_count = 0;
        if (tmax <= HOT_TRIGGER_RESET_C)
        {
          if (!g_hot_trigger_armed)
          {
            g_hot_trigger_armed = true;
            Serial.printf("[HotTrigger] Rearmed: T_max=%.2f C\n", tmax);
          }
        }
      }
    }
  }

  if (!upload_in_progress)
  {
    screen_loop();
  }

  delay(1);
  yield();
}
