#pragma once

#include <Arduino.h>

#include "MLX90640_API.hpp"
#include "MLX90640_I2C_Driver.hpp"
#include "MLX90641_API.hpp"
#include "MLX90641_I2C_Driver.hpp"
#include "../shared_val.hpp"
#include "../kalman_filter.hpp"


#define MLX90640_PIXELS 768
#define MLX90641_PIXELS 192
#define MLX_MAX_PIXELS MLX90640_PIXELS
#define MLX90640_COLS 32
#define MLX90640_ROWS 24
#define MLX90641_COLS 16
#define MLX90641_ROWS 12

// 缩放相关：基础缩放和最大缩放. MLX90640 使用 BASE_SCALE，MLX90641 需要放大 2 倍以匹配显示效果
#define BASE_SCALE 10
#define MAX_SCALE (BASE_SCALE * 2)

// ================= 配置区域 =================
#define MXL_STARTUP_DELAY 1500
#define TA_SHIFT 8 //Default shift for MLX90640 in open air
const byte MLX90640_address = 0x33;
// 定义驱动所需的参数结构体
static paramsMLX90640 mlx90640;
static paramsMLX90641 mlx90641;

// ================= 卡尔曼滤波相关 =================
// 定义滤波器数组，每个像素一个滤波器
static KFPTypeS kfpVar3Array[MLX_MAX_PIXELS]; 
static bool kfp_inited = false; // 初始化标志
const static float init_P = 0.1;
const static float init_G = 0.0;
const static float init_O = 26;
// 初始化卡尔曼数组
inline void KalmanArrayInit(int pixel_count) {
   for (int i = 0; i < pixel_count; ++i) {
        // 使用第一帧的数据作为初始值，防止启动时数值跳变
        KalmanFloat_Init(&kfpVar3Array[i], init_P, init_G, init_O);
    }
    kfp_inited = true;
    Serial.println(">> MLX Kalman Filter Array Initialized.");
}

inline unsigned short CelsiusToDeciKelvin(float temp_c) {
    float k = temp_c + 273.15f;
    if (k < 0) k = 0; // 防止负数变成极是大整数
    return (unsigned short)(k * 10.0f + 0.5f);
}
void sensor_power_on(){
  pinMode(MLX_VDD, OUTPUT);
  digitalWrite(MLX_VDD, LOW);
  prob_status = PROB_CONNECTING;
}
/**
 * @brief 为 MLX90640/90641 动态申请内存
 * 大小固定为 834 个 float (约 3.3KB)，足够容纳 90640 的 768 个点以及潜在的溢出缓冲区
 */
void alloc_mlx_memory() {
    if (mlxBufferA != nullptr) return;

    // 分配两个 float 缓冲区 (双缓冲)
    size_t size_float = 834 * sizeof(float);
    size_t total_bytes = size_float * 2 + 834 * sizeof(uint16_t); // Buffer A + Buffer B + IntBuffer
    
    Serial.printf(">> MLX Memory: Allocating %d Bytes... ", (int)total_bytes);

    mlxBufferA = new float[834];
    mlxBufferB = new float[834];
    mlx90640To_buffer = new uint16_t[834]; // 颜色映射缓存

    if (mlxBufferA && mlxBufferB && mlx90640To_buffer) {
        memset(mlxBufferA, 0, size_float);
        memset(mlxBufferB, 0, size_float);
        memset(mlx90640To_buffer, 0, 834 * sizeof(uint16_t));

        // 初始化指针
        pWriteBuffer = mlxBufferA;
        pReadBuffer  = mlxBufferB;
        
        // 双缓冲初始化成功 - 不使用互斥锁，改用内存屏障方案
        Serial.println("SUCCESS (Double Buffer Ready)");
    } else {
        Serial.println("FAILED!");
    }
}

// ================= 辅助函数 =================
inline uint16_t mlx_pixel_count(){ return is_90640 ? MLX90640_PIXELS : MLX90641_PIXELS; }
inline uint8_t mlx_cols(){ return is_90640 ? MLX90640_COLS : MLX90641_COLS; }
inline uint8_t mlx_rows(){ return is_90640 ? MLX90640_ROWS : MLX90641_ROWS; }
inline uint8_t mlx_scale(){ return is_90640 ? BASE_SCALE : (BASE_SCALE * 2); }

// 根据屏幕坐标获取 MLX 温度值 (处理坐标翻转)
float get_mlx_temperature(int screen_x, int screen_y) {
   if (pReadBuffer == nullptr) return 0.0f;

   int cols = mlx_cols();
   int rows = mlx_rows();
   int scale = (int)mlx_scale();
   
   if (screen_x < 0) screen_x = 0;
   if (screen_y < 0) screen_y = 0;
   
   int sensor_col = screen_x / scale;
   if (sensor_col >= cols) sensor_col = cols - 1;

   int sensor_row = rows - 1 - (screen_y / scale);
   if (sensor_row < 0) sensor_row = 0;
   if (sensor_row >= rows) sensor_row = rows - 1;

   int idx = sensor_row * cols + sensor_col;
   
   // 从读缓冲区读取
   return ((float*)pReadBuffer)[idx]; 
}

// ================= 核心功能实现 =================

/**
 * @brief 阻塞式初始化并深度检查 MLX 传感器
 * @param max_retries 最大重试次数。-1 为无限重试。
 * @return true: 初始化并自检成功; false: 失败
 */
bool blocking_mlx_init_and_check(int max_retries = -1) {
    int retry_count = 0;
    if (current_sensor == SENSOR_MLX90640) {is_90640 = true;}
    else if (current_sensor == SENSOR_MLX90641) {is_90640 = false;}
    else {
        Serial.println("Error: blocking_mlx_init_and_check called but current_sensor is not MLX.");
        return false;
    }

    // 引用 shared_values.h 或 probe_mlx.h 中的全局变量
    // 确保 paramsMLX90640 mlx90640; 已经在外部定义
    probeWire->setPins(MLX_SDA, MLX_SCL);
    delay(MXL_STARTUP_DELAY); // 等待传感器上电稳定
    while (true) {
        retry_count++;
        Serial.printf("MLX Probe Attempt %d", retry_count);
        if (max_retries > 0) Serial.printf("/%d", max_retries);
        Serial.println("...");

        // ==========================================
        // 1. 硬件总线初始化 (MLX 需要高速 I2C)
        // ==========================================
        // probeWire->setSDA(MLX_SDA);
        // probeWire->setSCL(MLX_SCL);
        
        probeWire->begin();
        delay(5);
        // probeWire->setClock(800000); // MLX90640 推荐 800kHz - 1MHz 
        probeWire->setClock(8000000); // MLX90640 推荐 800kHz - 1MHz 
        delay(10); // 给一点总线稳定时间

        // ==========================================
        // 2. 第一步：基础 I2C 地址应答检查 (Ping)
        // ==========================================
        probeWire->beginTransmission((uint8_t)MLX90640_address);
        byte error = probeWire->endTransmission();
        
        bool step1_ok = (error == 0);
        bool step2_ok = false;
        bool step3_ok = false;

        if (step1_ok) {
             Serial.println(" >> I2C Address ACK: OK");
        } else {
             Serial.println(" >> I2C Address NACK (No Response)");
        }

        // ==========================================
        // 3. 第二步：读取状态/控制寄存器 (类似 ID 检查)
        // ==========================================
        // 尝试读取寄存器 0x800D (Control Register 1)，验证芯片逻辑是否响应
        if (step1_ok) {
            uint16_t controlRegister1 = 0;
            int status;
            
            // 使用 MLX 驱动自带的 I2C 读取函数
            // 注意：MLX90640_I2CRead 返回 0 表示成功
            if (current_sensor == SENSOR_MLX90640) {
                 status = MLX90640_I2CRead(MLX90640_address, 0x800D, 1, &controlRegister1);
            } else {
                 // MLX90641 也有类似的寄存器
                 status = MLX90641_I2CRead(MLX90640_address, 0x800D, 1, &controlRegister1);
            }

            if (status == 0) {
                Serial.printf(" >> Control Reg (0x800D) Read: 0x%04X\n", controlRegister1);
                step2_ok = true;
            } else {
                Serial.printf(" >> Register Read Failed. Err: %d\n", status);
            }
        }

        // ==========================================
        // 4. 第三步：EEPROM 完整性与参数提取 (最严格的检查)
        // ==========================================
        // 这相当于海曼的配置写入检查，但更深入：如果 EEPROM 坏了，传感器就废了
        if (step1_ok && step2_ok) {
            int status = -1;
            Serial.println(" >> Verifying EEPROM & Extracting Parameters...");
            
            if (current_sensor == SENSOR_MLX90640) {
                static uint16_t eeMLX90640[832];
                // 1. 读取整个 EEPROM
                int dumpStatus = MLX90640_DumpEE(MLX90640_address, eeMLX90640);
                if (dumpStatus == 0) {
                    // 2. 尝试解析参数 (这一步会校验 EEPROM 数据是否合法)
                    status = MLX90640_ExtractParameters(eeMLX90640, &mlx90640);
                }
            } else {
                static uint16_t eeMLX90641[832];
                int dumpStatus = MLX90641_DumpEE(MLX90640_address, eeMLX90641);
                if (dumpStatus == 0) {
                    status = MLX90641_ExtractParameters(eeMLX90641, &mlx90641);
                }
            }

            if (status == 0) {
                Serial.println(" >> Parameters Extracted: OK");
                step3_ok = true;
            } else {
                Serial.println(" >> EEPROM/Param Error: Data Corrupt.");
            }
        }

        // ==========================================
        // 5. 第四步：写入配置 (设置刷新率)
        // ==========================================
        if (step3_ok) {
            int setRateStatus;
            if (current_sensor == SENSOR_MLX90640) {
                // 0x04=8Hz, 0x05=16Hz, 0x06=32Hz, 0x07=64Hz
                setRateStatus = MLX90640_SetRefreshRate(MLX90640_address, 0x05); 
                // 设置分辨率 (18-bit)
                MLX90640_SetResolution(MLX90640_address, 0x03); 
            } else {
                // 90641 特殊使能指令
                MLX90641_I2CWrite(0x33, 0x800D, 6401); 
                setRateStatus = MLX90641_SetRefreshRate(MLX90640_address, 0x05);
            }
            
            if (setRateStatus == 0) {
                Serial.println(" >> Success: MLX Sensor Ready.");
                alloc_mlx_memory();
                KalmanArrayInit(mlx_pixel_count());
                return true; // === 成功返回 ===
            } else {
                 Serial.println(" >> Config Write Failed.");
            }
        }

        // ==========================================
        // 失败处理
        // ==========================================
        if (max_retries > 0 && retry_count >= max_retries) {
            Serial.println(" >> Maximum retries reached. Giving up.");
            return false;
        }

        Serial.println(" >> Retrying in 200ms...");
        delay(200); // 失败后等待2秒
    }
}



void probe_error_detect(){
   static uint8_t error_count = 0;
   if (T_max_fp > 1500. or T_max_fp < -1000.){
      error_count++;
   }
   if (error_count > 5){ 
      error_count = 0;
      is_90640 = !is_90640;
      if (is_90640){
         current_sensor = SENSOR_MLX90640;
      } else {
         current_sensor = SENSOR_MLX90641;
      }
      Serial.println("Error: Probe type mismatch, rebooting...");
      EEPROM.write(20, current_sensor); // 存储高字节
      EEPROM.commit();
      // 禁用中断并延迟，确保 Core 1 安全退出当前操作
      portDISABLE_INTERRUPTS();
      delay(100);
      ESP.restart();
   }
}

void probe_loop_mlx(){
    if (flag_in_photo_mode){return;}
    // 静态数组：用于保存完整的画面（合成缓冲区）
    // 即使双缓冲切走了，这里的数据依然保留，用于"拼凑"下一帧
    static float mlx_composition_buffer[834];
    static bool is_buffer_inited = false; // 标记是否初始化过

    // 初始化一下合成缓冲区，防止第一帧乱码
    if (!is_buffer_inited) {
        memset(mlx_composition_buffer, 0, sizeof(mlx_composition_buffer));
        is_buffer_inited = true;
    }

    static uint16_t mlx90640Frame[834];
    static int status;
    static float vdd, Ta, tr, emissivity;
    
    // 获取当前双缓冲的写指针（这是给 UI 用的）
    float* currentWriteTarget = (float*)pWriteBuffer;
    if (currentWriteTarget == nullptr) return; 

    // ==========================================
    // 1. 读取数据并更新到【合成缓冲区】(static)
    // ==========================================
    if (current_sensor == SENSOR_MLX90640){
        // 只读一次！提高帧率的核心
        status = MLX90640_GetFrameData(MLX90640_address, mlx90640Frame);
        
        // 计算辅助参数
        vdd = MLX90640_GetVdd(mlx90640Frame, &mlx90640);
        Ta = MLX90640_GetTa(mlx90640Frame, &mlx90640);
        tr = Ta - TA_SHIFT;
        emissivity = 0.95;

        // 【关键】计算结果存入 mlx_composition_buffer (静态合成区)
        // 这样 Subpage 0 和 Subpage 1 就会在这个静态数组里自动互补
        MLX90640_CalculateTo(mlx90640Frame, &mlx90640, emissivity, tr, mlx_composition_buffer);

    } else {
        // MLX90641 同理
        status = MLX90641_GetFrameData(MLX90640_address, mlx90640Frame);
        vdd = MLX90641_GetVdd(mlx90640Frame, &mlx90641);
        Ta = MLX90641_GetTa(mlx90640Frame, &mlx90641);
        tr = Ta - TA_SHIFT;
        emissivity = 0.95;
        MLX90641_CalculateTo(mlx90640Frame, &mlx90641, emissivity, tr, mlx_composition_buffer);
    }

    // ==========================================
    // 2. 将合成好的完整画面【拷贝】到双缓冲写指针
    // ==========================================
    // 这一步解决了“一半是0”的问题，因为 composition_buffer 永远是满的
    memcpy(currentWriteTarget, mlx_composition_buffer, sizeof(float) * 834);
    // ==========================================
    // 3. 后处理 (Min/Max/Kalman)
    // ==========================================
    // 注意：现在我们是对 currentWriteTarget 进行处理
    uint16_t pix = mlx_pixel_count();
    uint8_t cols = mlx_cols();
    
    // 初始化统计变量
    float local_min = currentWriteTarget[0];
    float local_max = currentWriteTarget[0];
    double local_avg = 0;

    for (int i = 0; i < pix; i++){ // 从0开始遍历更安全
         // 简单的坏点过滤 (-41 ~ 301)
         if((currentWriteTarget[i] > -41) && (currentWriteTarget[i] < 301))
            {
               // 卡尔曼滤波 (原地更新)
               if (flag_use_kalman==true){
                  // 注意：滤波结果也要更新回 composition buffer，否则下一帧合成时用的旧数据是未滤波的，会造成数据震荡
                  currentWriteTarget[i] = KalmanFilter(&kfpVar3Array[i], currentWriteTarget[i]);
                  mlx_composition_buffer[i] = currentWriteTarget[i]; // 【同步回静态区】
               }
               
               // 统计 Min
               if(currentWriteTarget[i] < local_min) {
                  local_min = currentWriteTarget[i];
               }

               // 统计 Max
               if(currentWriteTarget[i] > local_max) {
                  local_max = currentWriteTarget[i];
                  x_max = i / cols;
                  y_max = i % cols;
               }
            }
         else if(i > 0){
               currentWriteTarget[i] = currentWriteTarget[i-1]; // 简单补点
            }
         else if (pix > 1){
               currentWriteTarget[i] = currentWriteTarget[i+1];
            }
            local_avg += currentWriteTarget[i];
    }

    // ==========================================
    // 4. 交换指针 (提交给 UI) 并更新温度统计变量
    // ==========================================

    prob_lock = true;
    volatile float* temp = pWriteBuffer;
    pWriteBuffer = pReadBuffer;
    pReadBuffer = temp;
    prob_lock = false;

    // 温度统计变量更新
    T_avg_fp = (float)(local_avg / pix);
    T_max_fp = local_max;
    T_min_fp = local_min;

    T_max = CelsiusToDeciKelvin(T_max_fp);
    T_min = CelsiusToDeciKelvin(T_min_fp);
    T_avg = CelsiusToDeciKelvin(T_avg_fp);

    hasNewData = true;
    probe_error_detect();
}