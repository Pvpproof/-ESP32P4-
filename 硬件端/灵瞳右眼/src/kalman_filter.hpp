#ifndef _KALMAN_H_
#define _KALMAN_H_

#include <Arduino.h>
#include <stdint.h>
#define PIXEL_ROWS 32
#define PIXEL_COLS 32

// 增益和比例因子缩放（避免浮点）
#define KF_ONE 1000

// 系统与测量噪声（单位：dK）
u_int8_t KF_Q = 2;     // 系统噪声方差（e.g. 0.5K）
u_int8_t KF_R = 20;    // 测量噪声方差（e.g. 1.0K）

typedef struct {
    int32_t P;       // 协方差（单位 dK）
    int32_t G;       // 卡尔曼增益（G × 1000）
    int32_t Output;  // 滤波器输出（单位 dK）
} KalmanDK;


void Kalman_Init(KalmanDK* kfp, int16_t init_val_dK, int16_t init_P_dK);
int16_t Kalman_Update(KalmanDK* kfp, int16_t input_dK);

void Kalman_Init(KalmanDK* kfp, int16_t init_val_dK, int16_t init_P_dK)
{
    kfp->Output = init_val_dK;
    kfp->P = init_P_dK;
    kfp->G = 0;
}

int16_t Kalman_Update(KalmanDK* kfp, int16_t input_dK)
{
    // 1. 预测协方差
    kfp->P += KF_Q;

    // 2. 计算卡尔曼增益 G = P / (P + R)
    int32_t denom = kfp->P + KF_R;
    kfp->G = (kfp->P * KF_ONE) / denom;  // G × 1000

    // 3. 更新输出值
    int32_t delta = input_dK - kfp->Output;
    kfp->Output += (kfp->G * delta) / KF_ONE;

    // 4. 更新协方差
    kfp->P = ((KF_ONE - kfp->G) * kfp->P) / KF_ONE;

    return (int16_t)(kfp->Output);
}

void KalmanMatrix_Init(KalmanDK matrix[PIXEL_ROWS][PIXEL_COLS], const unsigned short init_data[PIXEL_ROWS][PIXEL_COLS])
{
    for (int i = 0; i < PIXEL_ROWS; ++i) {
        for (int j = 0; j < PIXEL_COLS; ++j) {
            matrix[i][j].Output = init_data[i][j];
            matrix[i][j].P = 20;  // 初始协方差 = 2.0K
            matrix[i][j].G = 0;
        }
    }
}

bool is_q_valid(int q)
{
    return (q >= 0 && q <= 100);  // 假设有效范围为 0 到 100
}
bool is_r_valid(int r)
{
    return (r >= 0 && r <= 100);  // 假设有效范围为 0 到 100
}


// ==========================================
// 2. 浮点版本 (MLX 专用, 单位 °C)
// ==========================================

// 浮点版参数 (可以根据效果调整)
// Q: 过程噪声 (越小越平滑，但响应越慢)
// R: 测量噪声 (越大越平滑，但响应越慢)
static float KF_Q_FLOAT = 0.05f; 
static float KF_R_FLOAT = 0.5f; 

typedef struct {
    float P;      // 协方差
    float G;      // 卡尔曼增益
    float Output; // 滤波器输出
} KFPTypeS;

// 浮点版：单点更新函数
inline float KalmanFilter(KFPTypeS* kfp, float input) {
    // 1. 预测协方差
    kfp->P = kfp->P + KF_Q_FLOAT;

    // 2. 计算卡尔曼增益
    kfp->G = kfp->P / (kfp->P + KF_R_FLOAT);

    // 3. 更新输出值
    kfp->Output = kfp->Output + kfp->G * (input - kfp->Output);

    // 4. 更新协方差
    kfp->P = (1.0f - kfp->G) * kfp->P;

    return kfp->Output;
}

// 浮点版：初始化函数
inline void KalmanFloat_Init(KFPTypeS* kfp, float init_p, float init_g, float init_o) {
    kfp->Output = init_o;
    kfp->P = init_p; // 初始协方差
    kfp->G = init_g;
}


void kalman_cli(String cmd){
    static int setq = 0.;
    static int setr = 0.;
    if (cmd.startsWith("-q")){
        Serial.printf("Kalman_Q: %d (dK)\nKalman_R: %d (dK)\n", KF_Q, KF_R);
    }else if (cmd.startsWith("set Q ")){
        setq = cmd.substring(6).toInt();
        if (is_q_valid(setq)){
            KF_Q = setq;
            Serial.printf("Kalman_Q set to: %d (dK)\n", KF_Q);
        }else{
            Serial.printf("Kalman_Q can't to: %d (dK)\n", setq);
        }
    }else if (cmd.startsWith("set R")){
        setr = cmd.substring(6).toInt();
        if (is_q_valid(setr)){
            KF_R = setr;
            Serial.printf("Kalman_R set to: %d (dK)\n", KF_R);
        }else{
            Serial.printf("Kalman_R can't to: %d (dK)\n", setr);
        }
    }
}


#endif