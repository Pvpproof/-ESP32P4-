#ifndef SHARED_VAL_H
#define SHARED_VAL_H

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>

#define MLX90640_PIXELS 768
#define MLX90641_PIXELS 192
#define MLX_MAX_PIXELS MLX90640_PIXELS
#define MLX90640_COLS 32
#define MLX90640_ROWS 24
#define MLX90641_COLS 16
#define MLX90641_ROWS 12

#define MLX_VDD 19
#define MLX_SDA 23
#define MLX_SCL 18

enum
{
    SENSOR_MLX90640,
    SENSOR_MLX90641
};

uint8_t current_sensor = SENSOR_MLX90640;
bool is_90640 = true;
uint8_t SRC_WIDTH = 32;
uint8_t SRC_HEIGHT = 24;

enum
{
    PROB_CONNECTING,
    PROB_INITIALIZING,
    PROB_PREPARING,
    PROB_READY
};

enum
{
    TP_CONNECTING,
    TP_READY,
    TP_NOTFOUND
};

uint8_t brightness = 185;
unsigned short T_max = 0, T_min = 0;
unsigned long T_avg = 0;
float ft_max = 0, ft_min = 0;
volatile float T_min_fp = 0, T_max_fp = 0, T_avg_fp = 0;

int SENSOR_ROWS = 24;
int SENSOR_OFFSET = 0;

uint16_t *frameBuffer = nullptr;
float *mlxBufferA = nullptr;
float *mlxBufferB = nullptr;
uint16_t *mlx90640To_buffer = nullptr;

volatile float *pWriteBuffer = nullptr;
volatile float *pReadBuffer = nullptr;
volatile bool hasNewData = false;

uint16_t test_point[2] = {140, 120};
bool flag_use_kalman = false;
bool use_upsample = true;
bool flag_trace_max = true;
bool flag_in_photo_mode = false;
bool flag_show_cursor = true;
bool flag_clear_cursor = false;
volatile bool flag_sensor_ok = false;
uint8_t prob_status = PROB_CONNECTING;

TwoWire *probeWire = &Wire;
int x_max = 0;
int y_max = 0;
volatile bool prob_lock = false;
volatile bool pix_cp_lock = false;
volatile bool cmap_loading_lock = false;
int vbat_percent = 100;
int num_frames = 0;
bool color_reverse = true;

float align_tx = 0.0f;
float align_ty = 0.0f;
float align_sx = 1.0f;
float align_sy = 1.0f;
float align_ang = 0.0f;
uint8_t fusion_alpha = 128;
bool camera_vflip = false;
bool camera_hmirror = false;

enum DisplayMode
{
    MODE_THERMAL_OVERLAY = 4
};

DisplayMode current_display_mode = MODE_THERMAL_OVERLAY;

enum OverlayFilterMode
{
    FILTER_CLASSIC = 0,
    FILTER_WHITE_HOT = 1,
    FILTER_BLACK_HOT = 2,
    FILTER_EDGE_HOT = 3
};

uint8_t current_filter_mode = FILTER_CLASSIC;
volatile bool upload_in_progress = false;
volatile bool upload_request_pending = false;
volatile bool upload_worker_enabled = true;

#define EEPROM_ADDR_SENSOR_TYPE 20
#define EEPROM_ADDR_DISPLAY_MODE 21
#define EEPROM_ADDR_FILTER_MODE 22
#define DEFAULT_DISPLAY_MODE MODE_THERMAL_OVERLAY
#define DEFAULT_FILTER_MODE FILTER_CLASSIC

void print_heap_usage()
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    Serial.printf("Internal heap: %u / %u bytes (%.2f%% used)\n",
                  (unsigned int)(total_internal - free_internal), (unsigned int)total_internal,
                  total_internal ? (float)(total_internal - free_internal) * 100.0f / total_internal : 0.0f);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    Serial.printf("PSRAM: %u / %u bytes (%.2f%% used)\n",
                  (unsigned int)(total_psram - free_psram), (unsigned int)total_psram,
                  total_psram ? (float)(total_psram - free_psram) * 100.0f / total_psram : 0.0f);
}

#endif