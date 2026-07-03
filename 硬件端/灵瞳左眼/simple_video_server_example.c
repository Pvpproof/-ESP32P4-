/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "example_video_common.h"
#include "esp_wifi.h"
#include "esp_rom_sys.h" // 🌟 新增：包含 esp_rom_delay_us 所需的头文件

#define EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER

// ==================== 🛠️ 核心配置区域 ====================
#define MACBOOK_IP "192.168.43.14" // 👈 改成你 MacBook 的实际 IP
#define UDP_PORT 9999              // 推流目的端口
#define TARGET_JPEG_QUALITY 15     // 初始画质

#define CHUNK_SIZE 1400 // 🎯 方案2核心：应用层分包大小，严格小于 1500 字节以绕过内核切片
// =======================================================

typedef struct web_cam_video
{
    int fd;
    example_encoder_handle_t encoder_handle;
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;
    uint8_t *buffer[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
} web_cam_video_t;

static const char *TAG = "udp_stream";
static web_cam_video_t g_video = {.fd = -1};

static esp_err_t init_web_cam_video(web_cam_video_t *video, const char *dev_name)
{
    struct v4l2_format format;
    struct v4l2_requestbuffers req;

    int fd = open(dev_name, O_RDWR);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_NOT_FOUND, TAG, "Open video device %s failed", dev_name);

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &format) != 0)
    {
        close(fd);
        return ESP_FAIL;
    }

    memset(&req, 0, sizeof(req));
    req.count = EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0)
    {
        close(fd);
        return ESP_FAIL;
    }

    for (int i = 0; i < EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER; i++)
    {
        struct v4l2_buffer buf = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0)
        {
            close(fd);
            return ESP_FAIL;
        }
        video->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (video->buffer[i] == MAP_FAILED)
        {
            close(fd);
            return ESP_ERR_NO_MEM;
        }
        video->buffer_size = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0)
        {
            close(fd);
            return ESP_FAIL;
        }
    }

    video->fd = fd;
    video->width = format.fmt.pix.width;
    video->height = format.fmt.pix.height;
    video->pixel_format = format.fmt.pix.pixelformat;

    ESP_LOGW(TAG, "检测到传感器格式: " V4L2_FMT_STR "，正在启动 P4 硬件外设 JPEG 编码加速...", V4L2_FMT_STR_ARG(video->pixel_format));
    example_encoder_config_t encoder_config = {
        .width = video->width, .height = video->height, .pixel_format = video->pixel_format, .quality = TARGET_JPEG_QUALITY};
    if (example_encoder_init(&encoder_config, &video->encoder_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "P4 硬件 JPEG 编码外设启动失败！");
        close(fd);
        return ESP_FAIL;
    }
    example_encoder_alloc_output_buffer(video->encoder_handle, &video->jpeg_out_buf, &video->jpeg_out_size);

    return ESP_OK;
}

// 🚀 核心：应用层分包发送任务
static void udp_stream_task(void *pvParameters)
{
    web_cam_video_t *video = (web_cam_video_t *)pvParameters;
    struct v4l2_buffer buf;
    uint32_t jpeg_encoded_size;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "创建 Socket 失败！");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(MACBOOK_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);

    int64_t last_fps_time = esp_timer_get_time();
    int frame_count = 0;
    uint16_t global_frame_id = 0; // 自增帧ID

    // 预分配发送缓冲区（CHUNK_SIZE + 6字节自定义协议包头）
    uint8_t *tx_buffer = (uint8_t *)malloc(CHUNK_SIZE + 6);
    if (tx_buffer == NULL)
    {
        ESP_LOGE(TAG, "内存分配失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "================== 方案 2：分包 UDP 推流任务启动 ==================");

    while (1)
    {
        int64_t t_start = esp_timer_get_time();

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0)
            continue;
        if (!(buf.flags & V4L2_BUF_FLAG_DONE))
        {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        // P4 硬件外设 JPEG 编码
        if (example_encoder_process(video->encoder_handle, video->buffer[buf.index], video->buffer_size,
                                    video->jpeg_out_buf, video->jpeg_out_size, &jpeg_encoded_size) != ESP_OK)
        {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        int64_t t_wifi_start = esp_timer_get_time();

        // 🎯 核心改动：应用层循环切片逻辑
        global_frame_id++;
        uint8_t total_chunks = (jpeg_encoded_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        for (uint8_t i = 0; i < total_chunks; i++)
        {
            uint32_t offset = i * CHUNK_SIZE;
            uint32_t current_chunk_len = (jpeg_encoded_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (jpeg_encoded_size - offset);

            // 组装 6 字节自定义包头
            tx_buffer[0] = (global_frame_id >> 8) & 0xFF;
            tx_buffer[1] = global_frame_id & 0xFF;
            tx_buffer[2] = i;
            tx_buffer[3] = total_chunks;
            tx_buffer[4] = (current_chunk_len >> 8) & 0xFF;
            tx_buffer[5] = current_chunk_len & 0xFF;

            // 拷贝图片局部负载
            memcpy(tx_buffer + 6, video->jpeg_out_buf + offset, current_chunk_len);

            // 独立发送小包，由于 1406 <= MTU，物理层绝对不拆包，有效避开弱网黑洞
            sendto(sock, tx_buffer, current_chunk_len + 6, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

            // 🌟 核心修复 1：流量整形 (Pacing)。每发完一个 UDP 小包，强制等待 500 微秒
            // 防止 ESP32-P4 瞬间连发 9 个包把路由器或 Mac 的底层 Wi-Fi 接收队列打爆
            esp_rom_delay_us(500);
        }

        int64_t t_end = esp_timer_get_time();
        ioctl(video->fd, VIDIOC_QBUF, &buf);

        // 打点统计输出
        int64_t total_time = (t_end - t_start) / 1000;
        int64_t wifi_time = (t_end - t_wifi_start) / 1000;
        ESP_LOGI(TAG, "UDP Frame #%d | Size: %" PRIu32 " B (Split into %d chunks) | Local Proc: %lld ms | Total UDP Send: %lld ms",
                 global_frame_id, jpeg_encoded_size, total_chunks, total_time - wifi_time, wifi_time);

        frame_count++;
        int64_t now = esp_timer_get_time();
        if (now - last_fps_time >= 2000000)
        {
            float fps = (float)frame_count / ((float)(now - last_fps_time) / 1000000.0f);
            ESP_LOGW("FPS_MONITOR", ">>>>>> ESP32-P4 实际发射帧率: %.2f FPS <<<<<<", fps);
            frame_count = 0;
            last_fps_time = now;
        }

        // 🌟 核心修复 2：限制总发射帧率
        // 将原本的 10ms 改为 30ms。稳定输出约 25~30 FPS，降低网络整体负载。
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    free(tx_buffer);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        nvs_flash_init();
    }

    example_video_init();
    esp_netif_init();
    esp_event_loop_create_default();
    example_connect();

    esp_wifi_set_ps(WIFI_PS_NONE);

    const char *device_name =
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
        ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
#else
        ESP_VIDEO_DVP_DEVICE_NAME;
#endif

    if (init_web_cam_video(&g_video, device_name) == ESP_OK)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_video.fd, VIDIOC_STREAMON, &type);

        xTaskCreatePinnedToCore(udp_stream_task, "udp_stream_task", 1024 * 6, &g_video, 20, NULL, 1);
    }
    else
    {
        ESP_LOGE(TAG, "设备初始化失败");
    }
}