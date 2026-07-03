#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <lvgl.h>
#include "lvgl_v8_port.h"

#include <Audio.h>
#include <SD_MMC.h>
#include <FS.h>
#include <es8311.h>
#include <Wire.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ==========================================
// 引脚定义
// ==========================================
#define SERVO_STEER_PIN 22
#define SERVO_PAN_PIN 21
#define SERVO_TILT_PIN 20

#define STBY 2
#define PWMA 3
#define AIN1 4
#define AIN2 5
#define PWMB 32
#define BIN1 33
#define BIN2 36

#define OPENMV_RX 48
#define OPENMV_TX 47

#define I2S_DOUT 9
#define I2S_BCLK 12
#define I2S_LRC 10
#define I2S_MCLK 13
#define I2C_SDA 7
#define I2C_SCL 8
#define PA_ENABLE 53

#define SDMMC_CLK 43
#define SDMMC_CMD 44
#define SDMMC_D0 39
#define SDMMC_D1 40
#define SDMMC_D2 41
#define SDMMC_D3 42

// ==========================================
// 网络配置
// ==========================================
const char *WIFI_SSID = "Pvpproof";
const char *WIFI_PASSWORD = "psy13162516512";
const char *CLOUD_BASE_URL = "http://101.132.116.112/car_api";
const char *CLOUD_DEVICE_TOKEN = "smartcar_p4_cloud_001";
const char *DEVICE_NAME = "smartcar-p4-01";

// ==========================================
// 系统状态
// ==========================================
enum SystemMode {
  SYS_IDLE = 0,
  SYS_LINE_FOLLOW,
  SYS_BRAKE_FOR_SCAN,
  SYS_GIMBAL_SCAN,
  SYS_WAIT_ROUTE_DECISION,
  SYS_MISSION_COMPLETE,
};

enum GimbalMode {
  GIMBAL_CENTER = 0,
  GIMBAL_SCAN_LEFT,
  GIMBAL_SCAN_RIGHT,
  GIMBAL_MANUAL,
};

enum ScanMode {
  SCAN_NONE = 0,
  SCAN_RIGHT_ONLY,
  SCAN_BOTH_SIDES,
};

enum WorkPointId {
  WORK_START_END = 0,
  WORK_A,
  WORK_B,
  WORK_C,
  WORK_D,
  WORK_FINISH,
};

enum ScanSequenceState {
  SCAN_SEQ_IDLE = 0,
  SCAN_SEQ_RIGHT,
  SCAN_SEQ_LEFT,
  SCAN_SEQ_RETURN_CENTER,
  SCAN_SEQ_DONE,
};

struct VisionState {
  bool line_seen = false;
  int line_error = 0;
  bool t_node_seen = false;
  uint32_t last_update_ms = 0;
};

struct SteeringState {
  int current_angle = 90;
  int target_angle = 90;
};

struct GimbalState {
  int pan_current = 90;
  int pan_target = 90;
  int tilt_current = 25;
  int tilt_target = 25;
  bool scan_done = false;
  GimbalMode mode = GIMBAL_CENTER;
};

struct MotionState {
  bool enabled = false;
  int left_pwm = 0;
  int right_pwm = 0;
};

struct RouteTask {
  int order = 0;
  WorkPointId work_id = WORK_START_END;
  ScanMode scan_mode = SCAN_NONE;
  bool visited = false;
  bool done = false;
};

struct ScanResult {
  bool right_checked = false;
  bool left_checked = false;
};

// ==========================================
// 全局对象
// ==========================================
esp_panel::board::Board *board = nullptr;
Audio audio;
ES8311 es;
HardwareSerial OpenMVSerial(1);

SemaphoreHandle_t g_state_mutex = nullptr;
QueueHandle_t g_audio_queue = nullptr;

VisionState g_vision;
SteeringState g_steering;
GimbalState g_gimbal;
MotionState g_motion;

static RouteTask g_route_tasks[] = {
  { 1, WORK_A, SCAN_RIGHT_ONLY, false, false },
  { 2, WORK_B, SCAN_BOTH_SIDES, false, false },
  { 3, WORK_C, SCAN_BOTH_SIDES, false, false },
  { 4, WORK_D, SCAN_RIGHT_ONLY, false, false },
  { 5, WORK_FINISH, SCAN_NONE, false, false },
};

static const int g_route_task_count = sizeof(g_route_tasks) / sizeof(g_route_tasks[0]);
static int g_route_progress = 0;
static int g_active_task_index = -1;
static ScanSequenceState g_scan_sequence_state = SCAN_SEQ_IDLE;
static ScanMode g_pending_scan_mode = SCAN_NONE;
static ScanResult g_last_scan_result;
static bool g_plan_enabled[4] = { true, true, true, true };
static bool g_plan_running = false;

volatile SystemMode g_system_mode = SYS_IDLE;
volatile bool g_audio_busy = false;
volatile bool g_wifi_connected = false;
volatile bool g_cloud_push_ok = false;
volatile bool g_cloud_pull_ok = false;

// UI
lv_obj_t *main_scr = nullptr;
lv_obj_t *hardware_scr = nullptr;
lv_obj_t *test_scr = nullptr;
lv_obj_t *mission_scr = nullptr;

lv_obj_t *mode_label = nullptr;
lv_obj_t *vision_label = nullptr;
lv_obj_t *steer_label = nullptr;
lv_obj_t *gimbal_label = nullptr;

lv_obj_t *hardware_mode_label = nullptr;
lv_obj_t *hardware_vision_label = nullptr;
lv_obj_t *hardware_steer_label = nullptr;
lv_obj_t *hardware_gimbal_label = nullptr;
lv_obj_t *test_status_label = nullptr;
lv_obj_t *mission_status_label = nullptr;
lv_obj_t *mission_plan_label = nullptr;
lv_obj_t *mission_select_btns[4] = { nullptr, nullptr, nullptr, nullptr };

static char ui_mode_buf[64] = "";
static char ui_vision_buf[64] = "";
static char ui_steer_buf[64] = "";
static char ui_gimbal_buf[64] = "";
static char ui_hw_mode_buf[64] = "";
static char ui_hw_vision_buf[96] = "";
static char ui_hw_steer_buf[96] = "";
static char ui_hw_gimbal_buf[96] = "";
static char ui_test_buf[160] = "";
static char ui_mission_buf[160] = "";
static char ui_plan_buf[128] = "";

// ==========================================
// 工具函数
// ==========================================
const char *system_mode_name(SystemMode mode) {
  switch (mode) {
    case SYS_IDLE: return "IDLE";
    case SYS_LINE_FOLLOW: return "LINE_FOLLOW";
    case SYS_BRAKE_FOR_SCAN: return "BRAKE_FOR_SCAN";
    case SYS_GIMBAL_SCAN: return "GIMBAL_SCAN";
    case SYS_WAIT_ROUTE_DECISION: return "WAIT_ROUTE_DECISION";
    case SYS_MISSION_COMPLETE: return "MISSION_COMPLETE";
    default: return "UNKNOWN";
  }
}

void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] connecting to %s\n", WIFI_SSID);

  uint32_t start_ms = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start_ms < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  g_wifi_connected = (WiFi.status() == WL_CONNECTED);

  if (g_wifi_connected) {
    Serial.printf("[WIFI] connected, ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WIFI] connect timeout");
  }
}

String build_cloud_status_json() {
  StaticJsonDocument<768> doc;
  VisionState local_vision;
  int route_progress = 0;
  bool plan_running = false;

  if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    local_vision = g_vision;
    route_progress = g_route_progress;
    plan_running = g_plan_running;
    xSemaphoreGive(g_state_mutex);
  }

  const char *active_task = "NONE";
  RouteTask *task = get_active_route_task();
  if (task != nullptr) {
    active_task = work_point_name(task->work_id);
  } else if (route_progress < g_route_task_count) {
    active_task = work_point_name(g_route_tasks[route_progress].work_id);
  }

  doc["device_id"] = DEVICE_NAME;
  doc["token"] = CLOUD_DEVICE_TOKEN;
  doc["ip"] = g_wifi_connected ? WiFi.localIP().toString() : "";
  doc["wifi_connected"] = g_wifi_connected;
  doc["mode"] = system_mode_name(g_system_mode);
  doc["plan_running"] = plan_running;
  doc["route_progress"] = route_progress;
  doc["route_task_count"] = g_route_task_count;
  doc["active_task"] = active_task;
  doc["line_seen"] = local_vision.line_seen;
  doc["line_error"] = local_vision.line_error;
  doc["t_node_seen"] = local_vision.t_node_seen;
  doc["uptime_ms"] = millis();
  doc["cloud_pull_ok"] = g_cloud_pull_ok;

  JsonObject plan = doc.createNestedObject("plan");
  plan["A"] = g_plan_enabled[0];
  plan["B"] = g_plan_enabled[1];
  plan["C"] = g_plan_enabled[2];
  plan["D"] = g_plan_enabled[3];

  String out;
  serializeJson(doc, out);
  return out;
}

void apply_remote_plan(bool a, bool b, bool c, bool d) {
  g_plan_enabled[0] = a;
  g_plan_enabled[1] = b;
  g_plan_enabled[2] = c;
  g_plan_enabled[3] = d;
}

bool start_remote_mission() {
  reset_route_progress();
  g_plan_running = true;
  g_system_mode = SYS_LINE_FOLLOW;
  return true;
}

void stop_remote_mission() {
  g_plan_running = false;
  g_system_mode = SYS_IDLE;
  stop_motors();
  g_gimbal.mode = GIMBAL_CENTER;
}

void ack_cloud_command(int command_id) {
  if (WiFi.status() != WL_CONNECTED || command_id <= 0) return;

  HTTPClient http;
  String url = String(CLOUD_BASE_URL) + "/car_ack_command.php";

  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_NAME;
  doc["token"] = CLOUD_DEVICE_TOKEN;
  doc["command_id"] = command_id;

  String payload;
  serializeJson(doc, payload);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  Serial.printf("[CLOUD] ack code=%d\n", code);
  http.end();
}

void process_cloud_command(const JsonDocument &doc) {
  bool has_command = doc["has_command"] | false;
  if (!has_command) return;

  int command_id = doc["command_id"] | 0;
  String command = doc["command"] | "";
  Serial.printf("[CLOUD] recv command id=%d cmd=%s\n", command_id, command.c_str());

  if (command == "set_plan") {
    JsonVariantConst plan = doc["plan"];
    apply_remote_plan(plan["A"] | false, plan["B"] | false, plan["C"] | false, plan["D"] | false);
    ack_cloud_command(command_id);
    return;
  }

  if (command == "start") {
    JsonVariantConst plan = doc["plan"];
    if (!plan.isNull()) {
      apply_remote_plan(plan["A"] | false, plan["B"] | false, plan["C"] | false, plan["D"] | false);
    }
    start_remote_mission();
    ack_cloud_command(command_id);
    return;
  }

  if (command == "stop") {
    stop_remote_mission();
    ack_cloud_command(command_id);
    return;
  }
}

void push_status_to_cloud() {
  if (WiFi.status() != WL_CONNECTED) {
    g_wifi_connected = false;
    g_cloud_push_ok = false;
    return;
  }

  g_wifi_connected = true;

  HTTPClient http;
  String url = String(CLOUD_BASE_URL) + "/car_push_status.php";
  String payload = build_cloud_status_json();

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);

  if (code > 0) {
    String body = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, body);
    g_cloud_push_ok = (!err && (doc["ok"] | false));
    Serial.printf("[CLOUD] push code=%d ok=%d\n", code, g_cloud_push_ok ? 1 : 0);
  } else {
    g_cloud_push_ok = false;
    Serial.printf("[CLOUD] push fail code=%d\n", code);
  }

  http.end();
}

void pull_command_from_cloud() {
  if (WiFi.status() != WL_CONNECTED) {
    g_wifi_connected = false;
    g_cloud_pull_ok = false;
    return;
  }

  g_wifi_connected = true;

  HTTPClient http;
  String url = String(CLOUD_BASE_URL) + "/car_pull_command.php?device_id=" + DEVICE_NAME + "&token=" + CLOUD_DEVICE_TOKEN;
  http.begin(url);
  int code = http.GET();

  if (code <= 0) {
    g_cloud_pull_ok = false;
    Serial.printf("[CLOUD] pull fail code=%d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    g_cloud_pull_ok = false;
    Serial.printf("[CLOUD] pull json error: %s\n", err.c_str());
    return;
  }

  g_cloud_pull_ok = (doc["ok"] | false);
  process_cloud_command(doc);
}

void task_cloud_status_push(void *arg) {
  while (1) {
    push_status_to_cloud();
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void task_cloud_command_pull(void *arg) {
  while (1) {
    pull_command_from_cloud();
    vTaskDelay(pdMS_TO_TICKS(800));
  }
}

static inline int clamp_int(int value, int min_v, int max_v) {
  if (value < min_v) return min_v;
  if (value > max_v) return max_v;
  return value;
}

void write_servo_angle(int pin, int angle) {
  ledcWrite(pin, map(angle, 0, 180, 102, 512));
}

void detach_servo_pwm(int pin) {
  ledcWrite(pin, 0);
}

void motor_init() {
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(PWMB, OUTPUT);
  pinMode(STBY, OUTPUT);
  digitalWrite(STBY, HIGH);
}

void set_motor_pwm(int motor, int pwm) {
  pwm = clamp_int(pwm, -255, 255);

  if (motor == 1) {
    if (pwm >= 0) {
      digitalWrite(AIN1, LOW);
      digitalWrite(AIN2, HIGH);
      analogWrite(PWMA, pwm);
    } else {
      digitalWrite(AIN1, HIGH);
      digitalWrite(AIN2, LOW);
      analogWrite(PWMA, -pwm);
    }
  }

  if (motor == 2) {
    if (pwm >= 0) {
      digitalWrite(BIN1, LOW);
      digitalWrite(BIN2, HIGH);
      analogWrite(PWMB, pwm);
    } else {
      digitalWrite(BIN1, HIGH);
      digitalWrite(BIN2, LOW);
      analogWrite(PWMB, -pwm);
    }
  }
}

void stop_motors() {
  set_motor_pwm(1, 0);
  set_motor_pwm(2, 0);
  g_motion.enabled = false;
  g_motion.left_pwm = 0;
  g_motion.right_pwm = 0;
}

void set_cruise_speed(int pwm) {
  set_motor_pwm(1, pwm);
  set_motor_pwm(2, pwm);
  g_motion.enabled = true;
  g_motion.left_pwm = pwm;
  g_motion.right_pwm = pwm;
}

const char *work_point_name(WorkPointId id) {
  switch (id) {
    case WORK_A: return "A";
    case WORK_B: return "B";
    case WORK_C: return "C";
    case WORK_D: return "D";
    case WORK_FINISH: return "FINISH";
    case WORK_START_END:
    default: return "START_END";
  }
}

RouteTask *get_active_route_task() {
  if (g_active_task_index < 0 || g_active_task_index >= g_route_task_count) return nullptr;
  return &g_route_tasks[g_active_task_index];
}

bool is_work_point_selected(WorkPointId id) {
  switch (id) {
    case WORK_A: return g_plan_enabled[0];
    case WORK_B: return g_plan_enabled[1];
    case WORK_C: return g_plan_enabled[2];
    case WORK_D: return g_plan_enabled[3];
    default: return false;
  }
}

void reset_route_progress() {
  g_route_progress = 0;
  g_active_task_index = -1;
  g_pending_scan_mode = SCAN_NONE;
  g_scan_sequence_state = SCAN_SEQ_IDLE;
  g_last_scan_result = {};
  g_plan_running = false;
  for (int i = 0; i < g_route_task_count; ++i) {
    g_route_tasks[i].visited = false;
    g_route_tasks[i].done = false;
  }
}

bool gimbal_is_centered() {
  return abs(g_gimbal.pan_current - 90) <= 1 && abs(g_gimbal.tilt_current - 25) <= 1;
}

void begin_task_scan(RouteTask &task) {
  task.visited = true;
  g_pending_scan_mode = task.scan_mode;
  g_last_scan_result = {};
  g_gimbal.scan_done = false;

  if (task.scan_mode == SCAN_RIGHT_ONLY) {
    g_scan_sequence_state = SCAN_SEQ_RIGHT;
    g_gimbal.mode = GIMBAL_SCAN_RIGHT;
    Serial.printf("[MISSION] Reached %s -> scan right\n", work_point_name(task.work_id));
  } else if (task.scan_mode == SCAN_BOTH_SIDES) {
    g_scan_sequence_state = SCAN_SEQ_LEFT;
    g_gimbal.mode = GIMBAL_SCAN_LEFT;
    Serial.printf("[MISSION] Reached %s -> scan left then right\n", work_point_name(task.work_id));
  } else {
    g_scan_sequence_state = SCAN_SEQ_DONE;
    g_gimbal.mode = GIMBAL_CENTER;
    g_gimbal.scan_done = false;
  }
}

// ==========================================
// OpenMV 协议解析
// 例如：L1,E12,T0
// ==========================================
bool parse_openmv_packet(const String &line, VisionState &out) {
  int e_idx = line.indexOf(",E");
  int t_idx = line.indexOf(",T");
  if (!line.startsWith("L") || e_idx < 0 || t_idx < 0) return false;

  out.line_seen = line.substring(1, e_idx).toInt() != 0;
  out.line_error = line.substring(e_idx + 2, t_idx).toInt();
  out.t_node_seen = line.substring(t_idx + 2).toInt() != 0;
  out.last_update_ms = millis();
  return true;
}

// ==========================================
// 音频
// ==========================================
void audio_eof_mp3(const char *info) {
  g_audio_busy = false;
}

// ==========================================
// 任务：OpenMV 接收
// ==========================================
void task_openmv_rx(void *arg) {
  static char rx_buf[128];
  int rx_idx = 0;

  while (1) {
    while (OpenMVSerial.available()) {
      char c = OpenMVSerial.read();
      if (c == '\n') {
        rx_buf[rx_idx] = '\0';
        VisionState parsed;
        if (parse_openmv_packet(String(rx_buf), parsed)) {
          if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_vision = parsed;
            xSemaphoreGive(g_state_mutex);
          }
        }
        rx_idx = 0;
      } else if (rx_idx < (int)sizeof(rx_buf) - 1) {
        rx_buf[rx_idx++] = c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================
// 任务：舵机统一管理
// 重点：降低云台抽动
// ==========================================
void task_servo_manager(void *arg) {
  const uint32_t pan_interval_center_ms = 35;  //芜哈哈哈
  const uint32_t pan_interval_scan_ms = 35;    //
  uint32_t last_steer_ms = 0;
  uint32_t last_pan_ms = 0;
  uint32_t last_tilt_ms = 0;
  uint32_t last_steer_write_ms = 0;
  uint32_t last_pan_write_ms = 0;
  uint32_t last_tilt_write_ms = 0;
  bool steer_holding = true;
  bool pan_holding = true;
  bool tilt_holding = true;

  while (1) {
    // 前轮转向：提速，优先恢复巡线响应
    if (millis() - last_steer_ms >= 12) {
      last_steer_ms = millis();
      int delta = g_steering.target_angle - g_steering.current_angle;
      if (delta != 0) {
        int step = (abs(delta) >= 12) ? 3 : ((abs(delta) >= 6) ? 2 : 1);
        g_steering.current_angle += (delta > 0) ? step : -step;
        if ((delta > 0 && g_steering.current_angle > g_steering.target_angle) || (delta < 0 && g_steering.current_angle < g_steering.target_angle)) {
          g_steering.current_angle = g_steering.target_angle;
        }
        write_servo_angle(SERVO_STEER_PIN, g_steering.current_angle);
        last_steer_write_ms = millis();
        steer_holding = true;
      } else if (steer_holding && millis() - last_steer_write_ms > 120) {
        detach_servo_pwm(SERVO_STEER_PIN);
        steer_holding = false;
      }
    }

    // 云台模式机：默认保守角度，减少顶位抖动
    if (g_gimbal.mode == GIMBAL_CENTER) {
      g_gimbal.pan_target = 90;
      g_gimbal.tilt_target = 25;
      g_gimbal.scan_done = false;
    } else if (g_gimbal.mode == GIMBAL_SCAN_LEFT) {
      g_gimbal.tilt_target = 25;  // servo2 固定，不参与转动
      g_gimbal.pan_target = 180;
      if (g_gimbal.pan_current >= 179) {
        g_gimbal.scan_done = true;
      }
    } else if (g_gimbal.mode == GIMBAL_SCAN_RIGHT) {
      g_gimbal.tilt_target = 25;  // servo2 固定，不参与转动
      g_gimbal.pan_target = 0;
      if (g_gimbal.pan_current <= 1) {
        g_gimbal.scan_done = true;
      }
    }

    // pan：扫描时单独放慢，减少到 A/B/C/D 点位后云台转动过猛导致的画面卡顿
    uint32_t pan_interval_ms = (g_gimbal.mode == GIMBAL_SCAN_LEFT || g_gimbal.mode == GIMBAL_SCAN_RIGHT)
                                 ? pan_interval_scan_ms
                                 : pan_interval_center_ms;
    if (millis() - last_pan_ms >= pan_interval_ms) {
      last_pan_ms = millis();
      int delta = g_gimbal.pan_target - g_gimbal.pan_current;
      if (delta != 0) {
        g_gimbal.pan_current += (delta > 0) ? 1 : -1;
        write_servo_angle(SERVO_PAN_PIN, g_gimbal.pan_current);
        last_pan_write_ms = millis();
        pan_holding = true;
      } else if (pan_holding && millis() - last_pan_write_ms > 180) {
        detach_servo_pwm(SERVO_PAN_PIN);
        pan_holding = false;
      }
    }

    // tilt：30ms 一步；精确到目标角
    if (millis() - last_tilt_ms >= 30) {
      last_tilt_ms = millis();
      int delta = g_gimbal.tilt_target - g_gimbal.tilt_current;
      if (delta != 0) {
        g_gimbal.tilt_current += (delta > 0) ? 1 : -1;
        write_servo_angle(SERVO_TILT_PIN, g_gimbal.tilt_current);
        last_tilt_write_ms = millis();
        tilt_holding = true;
      } else if (tilt_holding && millis() - last_tilt_write_ms > 180) {
        detach_servo_pwm(SERVO_TILT_PIN);
        tilt_holding = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(12));
  }
}

// ==========================================
// 任务：车辆巡线控制
// 前轮跟误差，后轮匀速
// ==========================================
void task_vehicle_control(void *arg) {
  const int cruise_pwm = 80;
  const float steer_gain = 2.0f;
  const uint32_t vision_timeout_ms = 300;

  while (1) {
    VisionState local_vision;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      local_vision = g_vision;
      xSemaphoreGive(g_state_mutex);
    }

    bool vision_fresh = (millis() - local_vision.last_update_ms) < vision_timeout_ms;

    if (g_system_mode == SYS_LINE_FOLLOW) {
      if (local_vision.line_seen && vision_fresh) {
        int target_angle = 90 - (int)(local_vision.line_error * steer_gain);
        g_steering.target_angle = clamp_int(target_angle, 55, 125);
        set_cruise_speed(cruise_pwm);
      } else {
        stop_motors();
        g_steering.target_angle = 90;
      }
    } else {
      stop_motors();
      g_steering.target_angle = 90;
    }

    vTaskDelay(pdMS_TO_TICKS(25));
  }
}

// ==========================================
// 任务：高层状态机
// 巡线 -> T字确认 -> 停车 -> 云台扫描 -> 等待后续地图逻辑
// ==========================================
void task_state_machine(void *arg) {
  int t_seen_frames = 0;
  const int t_confirm_frames = 3;
  bool t_latched = false;

  while (1) {
    VisionState local_vision;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      local_vision = g_vision;
      xSemaphoreGive(g_state_mutex);
    }

    switch (g_system_mode) {
      case SYS_IDLE:
        stop_motors();
        g_steering.target_angle = 90;
        g_gimbal.mode = GIMBAL_CENTER;
        break;

      case SYS_LINE_FOLLOW:
        g_gimbal.mode = GIMBAL_CENTER;
        if (local_vision.t_node_seen) {
          if (t_seen_frames < t_confirm_frames) {
            t_seen_frames++;
          }
          if (!t_latched && t_seen_frames >= t_confirm_frames) {
            g_system_mode = SYS_BRAKE_FOR_SCAN;
            t_latched = true;
          }
        } else {
          t_seen_frames = 0;
          t_latched = false;
        }
        break;

      case SYS_BRAKE_FOR_SCAN:
        {
          stop_motors();
          g_steering.target_angle = 90;
          g_route_progress++;

          if (g_route_progress > g_route_task_count) {
            g_system_mode = SYS_MISSION_COMPLETE;
            break;
          }

          g_active_task_index = g_route_progress - 1;
          RouteTask *task = get_active_route_task();
          if (task == nullptr) {
            g_system_mode = SYS_MISSION_COMPLETE;
            break;
          }

          if (task->work_id == WORK_FINISH) {
            task->visited = true;
            task->done = true;
            Serial.println("[MISSION] Reached FINISH node, mission complete");
            g_system_mode = SYS_MISSION_COMPLETE;
            break;
          }

          if (!g_plan_running || is_work_point_selected(task->work_id)) {
            begin_task_scan(*task);
            g_system_mode = SYS_GIMBAL_SCAN;
          } else {
            task->visited = true;
            task->done = true;
            Serial.printf("[MISSION] Skip %s (not selected)\n", work_point_name(task->work_id));
            g_active_task_index = -1;
            g_system_mode = SYS_LINE_FOLLOW;
          }
          break;
        }

      case SYS_GIMBAL_SCAN:
        stop_motors();
        {
          RouteTask *task = get_active_route_task();
          if (task == nullptr) {
            g_system_mode = SYS_WAIT_ROUTE_DECISION;
            break;
          }

          if (g_scan_sequence_state == SCAN_SEQ_RETURN_CENTER) {
            if (gimbal_is_centered()) {
              g_scan_sequence_state = SCAN_SEQ_DONE;
              g_system_mode = SYS_WAIT_ROUTE_DECISION;
              Serial.printf("[MISSION] %s gimbal centered\n", work_point_name(task->work_id));
            }
            break;
          }

          if (g_gimbal.scan_done) {
            if (g_scan_sequence_state == SCAN_SEQ_LEFT) {
              g_last_scan_result.left_checked = true;
              g_gimbal.scan_done = false;
              g_scan_sequence_state = SCAN_SEQ_RIGHT;
              g_gimbal.mode = GIMBAL_SCAN_RIGHT;
              Serial.printf("[MISSION] %s left scan done -> turn right\n", work_point_name(task->work_id));
            } else if (g_scan_sequence_state == SCAN_SEQ_RIGHT) {
              g_last_scan_result.right_checked = true;
              g_gimbal.scan_done = false;
              g_scan_sequence_state = SCAN_SEQ_RETURN_CENTER;
              g_gimbal.mode = GIMBAL_CENTER;
              Serial.printf("[MISSION] %s right scan done -> return center\n", work_point_name(task->work_id));
            } else if (g_scan_sequence_state == SCAN_SEQ_DONE) {
              g_system_mode = SYS_WAIT_ROUTE_DECISION;
            }
          }
        }
        break;

      case SYS_WAIT_ROUTE_DECISION:
        {
          stop_motors();
          RouteTask *task = get_active_route_task();
          if (task != nullptr) {
            task->done = true;
            Serial.printf("[MISSION] %s task complete (L=%d R=%d)\n",
                          work_point_name(task->work_id),
                          g_last_scan_result.left_checked,
                          g_last_scan_result.right_checked);
            g_active_task_index = -1;
          }

          g_pending_scan_mode = SCAN_NONE;
          g_scan_sequence_state = SCAN_SEQ_IDLE;
          g_last_scan_result = {};

          if (g_route_progress >= g_route_task_count) {
            g_system_mode = SYS_MISSION_COMPLETE;
          } else {
            g_system_mode = SYS_LINE_FOLLOW;
          }
          break;
        }

      case SYS_MISSION_COMPLETE:
        stop_motors();
        g_steering.target_angle = 90;
        g_gimbal.mode = GIMBAL_CENTER;
        t_seen_frames = 0;
        t_latched = false;
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ==========================================
// 任务：音频任务（基础骨架）
// ==========================================
void task_audio(void *arg) {
  char path[32];
  while (1) {
    if (xQueueReceive(g_audio_queue, &path, pdMS_TO_TICKS(10)) == pdPASS) {
      audio.connecttoFS(SD_MMC, path);
      g_audio_busy = true;
      while (g_audio_busy) {
        audio.loop();
        vTaskDelay(pdMS_TO_TICKS(2));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ==========================================
// UI
// ==========================================
static void apply_page_bg(lv_obj_t *obj) {
  lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x4F4F98), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0xBF9797), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(obj, 240, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_HOR, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void apply_card_btn_style(lv_obj_t *btn) {
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x4F458C), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0xB46B6B), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(btn, lv_color_hex(0xDA7E7E), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(btn, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_color(btn, lv_color_hex(0x9D85E1), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_spread(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_color(btn, lv_color_hex(0xFAA3B8), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(btn, &lv_font_montserrat_26, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_align(btn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(btn, 0, LV_STATE_DEFAULT);
}

static void apply_info_panel_style(lv_obj_t *panel) {
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x463D82), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(panel, 210, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_color(panel, lv_color_hex(0xD99AAE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(panel, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(panel, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_all(panel, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void back_to_main_event_cb(lv_event_t *e) {
  lv_scr_load_anim(main_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void goto_hardware_event_cb(lv_event_t *e) {
  lv_scr_load_anim(hardware_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void goto_test_event_cb(lv_event_t *e) {
  lv_scr_load_anim(test_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void goto_mission_event_cb(lv_event_t *e) {
  lv_scr_load_anim(mission_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
}

static void start_line_follow_event_cb(lv_event_t *e) {
  if (g_system_mode == SYS_IDLE || g_system_mode == SYS_MISSION_COMPLETE) {
    reset_route_progress();
  }
  g_plan_running = false;
  g_system_mode = SYS_LINE_FOLLOW;
}

static void mission_toggle_plan_event_cb(lv_event_t *e) {
  intptr_t idx = (intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < 4) {
    g_plan_enabled[idx] = !g_plan_enabled[idx];
  }
}

static void mission_start_plan_event_cb(lv_event_t *e) {
  reset_route_progress();
  g_plan_running = true;
  g_system_mode = SYS_LINE_FOLLOW;
}

static void stop_vehicle_event_cb(lv_event_t *e) {
  g_system_mode = SYS_IDLE;
  stop_motors();
  g_gimbal.mode = GIMBAL_CENTER;
}

static void start_scan_left_event_cb(lv_event_t *e) {
  g_gimbal.scan_done = false;
  g_gimbal.mode = GIMBAL_SCAN_LEFT;
  g_system_mode = SYS_GIMBAL_SCAN;
}

static void start_scan_right_event_cb(lv_event_t *e) {
  g_gimbal.scan_done = false;
  g_gimbal.mode = GIMBAL_SCAN_RIGHT;
  g_system_mode = SYS_GIMBAL_SCAN;
}

static void gimbal_center_event_cb(lv_event_t *e) {
  g_gimbal.mode = GIMBAL_CENTER;
  g_gimbal.scan_done = false;
}

static void gimbal_scan_left_event_cb(lv_event_t *e) {
  g_gimbal.scan_done = false;
  g_gimbal.mode = GIMBAL_SCAN_LEFT;
}

static void gimbal_scan_right_event_cb(lv_event_t *e) {
  g_gimbal.scan_done = false;
  g_gimbal.mode = GIMBAL_SCAN_RIGHT;
}

static void steer_center_event_cb(lv_event_t *e) {
  g_steering.target_angle = 90;
}

static void motor_stop_only_event_cb(lv_event_t *e) {
  stop_motors();
}

void build_main_screen() {
  main_scr = lv_obj_create(NULL);
  apply_page_bg(main_scr);

  lv_obj_t *title = lv_label_create(main_scr);
  lv_label_set_text(title, "SmartCar P4");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDDC674), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_34, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  lv_obj_t *sub = lv_label_create(main_scr);
  lv_label_set_text(sub, "Control Center");
  lv_obj_set_style_text_color(sub, lv_color_hex(0xF8DDE5), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

  lv_obj_t *btn_hw = lv_btn_create(main_scr);
  lv_obj_set_size(btn_hw, 150, 68);
  lv_obj_set_pos(btn_hw, 70, 170);
  apply_card_btn_style(btn_hw);
  lv_obj_add_event_cb(btn_hw, goto_hardware_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_hw_label = lv_label_create(btn_hw);
  lv_label_set_text(btn_hw_label, "Hardware");
  lv_obj_center(btn_hw_label);

  lv_obj_t *btn_test = lv_btn_create(main_scr);
  lv_obj_set_size(btn_test, 150, 68);
  lv_obj_set_pos(btn_test, 280, 170);
  apply_card_btn_style(btn_test);
  lv_obj_add_event_cb(btn_test, goto_test_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_test_label = lv_label_create(btn_test);
  lv_label_set_text(btn_test_label, "Test");
  lv_obj_center(btn_test_label);

  lv_obj_t *btn_mission = lv_btn_create(main_scr);
  lv_obj_set_size(btn_mission, 150, 68);
  lv_obj_set_pos(btn_mission, 490, 170);
  apply_card_btn_style(btn_mission);
  lv_obj_add_event_cb(btn_mission, goto_mission_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_mission_label = lv_label_create(btn_mission);
  lv_label_set_text(btn_mission_label, "Mission");
  lv_obj_center(btn_mission_label);

  mode_label = lv_label_create(main_scr);
  lv_label_set_text(mode_label, "Mode: booting");
  lv_obj_set_style_text_color(mode_label, lv_color_hex(0xFCE5EC), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_20, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void build_hardware_screen() {
  hardware_scr = lv_obj_create(NULL);
  apply_page_bg(hardware_scr);

  lv_obj_t *title = lv_label_create(hardware_scr);
  lv_label_set_text(title, "Hardware Monitor");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDDC674), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *back_btn = lv_btn_create(hardware_scr);
  lv_obj_set_size(back_btn, 150, 68);
  lv_obj_set_pos(back_btn, 36, 34);
  apply_card_btn_style(back_btn);
  lv_obj_add_event_cb(back_btn, back_to_main_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);

  lv_obj_t *start_btn = lv_btn_create(hardware_scr);
  lv_obj_set_size(start_btn, 170, 68);
  lv_obj_set_pos(start_btn, 770, 34);
  apply_card_btn_style(start_btn);
  lv_obj_add_event_cb(start_btn, start_line_follow_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *start_label = lv_label_create(start_btn);
  lv_label_set_text(start_label, "Start");
  lv_obj_center(start_label);

  lv_obj_t *stop_btn = lv_btn_create(hardware_scr);
  lv_obj_set_size(stop_btn, 170, 68);
  lv_obj_set_pos(stop_btn, 960, 34);
  apply_card_btn_style(stop_btn);
  lv_obj_add_event_cb(stop_btn, stop_vehicle_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *stop_label = lv_label_create(stop_btn);
  lv_label_set_text(stop_label, "Stop");
  lv_obj_center(stop_label);

  lv_obj_t *panel = lv_obj_create(hardware_scr);
  lv_obj_set_size(panel, 980, 260);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -28);
  apply_info_panel_style(panel);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *manual_panel = lv_obj_create(hardware_scr);
  lv_obj_set_size(manual_panel, 980, 150);
  lv_obj_align(manual_panel, LV_ALIGN_TOP_MID, 0, 122);
  apply_info_panel_style(manual_panel);
  lv_obj_set_scrollbar_mode(manual_panel, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *manual_title = lv_label_create(manual_panel);
  lv_label_set_text(manual_title, "Manual Test");
  lv_obj_set_style_text_color(manual_title, lv_color_hex(0xFFF2F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(manual_title, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(manual_title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t *btn_center_gimbal = lv_btn_create(manual_panel);
  lv_obj_set_size(btn_center_gimbal, 170, 58);
  lv_obj_set_pos(btn_center_gimbal, 0, 52);
  apply_card_btn_style(btn_center_gimbal);
  lv_obj_add_event_cb(btn_center_gimbal, gimbal_center_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_center_gimbal_label = lv_label_create(btn_center_gimbal);
  lv_label_set_text(btn_center_gimbal_label, "Gimbal Center");
  lv_obj_center(btn_center_gimbal_label);

  lv_obj_t *btn_scan_gimbal = lv_btn_create(manual_panel);
  lv_obj_set_size(btn_scan_gimbal, 170, 58);
  lv_obj_set_pos(btn_scan_gimbal, 200, 52);
  apply_card_btn_style(btn_scan_gimbal);
  lv_obj_add_event_cb(btn_scan_gimbal, gimbal_scan_left_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_scan_gimbal_label = lv_label_create(btn_scan_gimbal);
  lv_label_set_text(btn_scan_gimbal_label, "T-ScanL");
  lv_obj_center(btn_scan_gimbal_label);

  lv_obj_t *btn_scan_gimbal_r = lv_btn_create(manual_panel);
  lv_obj_set_size(btn_scan_gimbal_r, 170, 58);
  lv_obj_set_pos(btn_scan_gimbal_r, 400, 52);
  apply_card_btn_style(btn_scan_gimbal_r);
  lv_obj_add_event_cb(btn_scan_gimbal_r, gimbal_scan_right_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_scan_gimbal_r_label = lv_label_create(btn_scan_gimbal_r);
  lv_label_set_text(btn_scan_gimbal_r_label, "T-ScanR");
  lv_obj_center(btn_scan_gimbal_r_label);

  lv_obj_t *btn_center_steer = lv_btn_create(manual_panel);
  lv_obj_set_size(btn_center_steer, 170, 58);
  lv_obj_set_pos(btn_center_steer, 600, 52);
  apply_card_btn_style(btn_center_steer);
  lv_obj_add_event_cb(btn_center_steer, steer_center_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_center_steer_label = lv_label_create(btn_center_steer);
  lv_label_set_text(btn_center_steer_label, "Steer Center");
  lv_obj_center(btn_center_steer_label);

  lv_obj_t *btn_stop_motor = lv_btn_create(manual_panel);
  lv_obj_set_size(btn_stop_motor, 170, 58);
  lv_obj_set_pos(btn_stop_motor, 800, 52);
  apply_card_btn_style(btn_stop_motor);
  lv_obj_add_event_cb(btn_stop_motor, motor_stop_only_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *btn_stop_motor_label = lv_label_create(btn_stop_motor);
  lv_label_set_text(btn_stop_motor_label, "Motor Stop");
  lv_obj_center(btn_stop_motor_label);

  hardware_mode_label = lv_label_create(panel);
  lv_label_set_text(hardware_mode_label, "Mode: --");
  lv_obj_set_style_text_color(hardware_mode_label, lv_color_hex(0xFFF2F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(hardware_mode_label, &lv_font_montserrat_28, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(hardware_mode_label, LV_ALIGN_TOP_LEFT, 0, 0);

  hardware_vision_label = lv_label_create(panel);
  lv_label_set_text(hardware_vision_label, "Vision: --");
  lv_obj_set_style_text_color(hardware_vision_label, lv_color_hex(0xFADBE4), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(hardware_vision_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align_to(hardware_vision_label, hardware_mode_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 24);

  hardware_steer_label = lv_label_create(panel);
  lv_label_set_text(hardware_steer_label, "Steer: --");
  lv_obj_set_style_text_color(hardware_steer_label, lv_color_hex(0xFADBE4), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(hardware_steer_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align_to(hardware_steer_label, hardware_vision_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 22);

  hardware_gimbal_label = lv_label_create(panel);
  lv_label_set_text(hardware_gimbal_label, "Gimbal: --");
  lv_obj_set_style_text_color(hardware_gimbal_label, lv_color_hex(0xFADBE4), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(hardware_gimbal_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align_to(hardware_gimbal_label, hardware_steer_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 22);
}

void build_test_screen() {
  test_scr = lv_obj_create(NULL);
  apply_page_bg(test_scr);

  lv_obj_t *title = lv_label_create(test_scr);
  lv_label_set_text(title, "Test Control");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDDC674), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *back_btn = lv_btn_create(test_scr);
  lv_obj_set_size(back_btn, 150, 68);
  lv_obj_set_pos(back_btn, 36, 34);
  apply_card_btn_style(back_btn);
  lv_obj_add_event_cb(back_btn, back_to_main_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);

  lv_obj_t *line_btn = lv_btn_create(test_scr);
  lv_obj_set_size(line_btn, 180, 72);
  lv_obj_set_pos(line_btn, 80, 190);
  apply_card_btn_style(line_btn);
  lv_obj_add_event_cb(line_btn, start_line_follow_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *line_label = lv_label_create(line_btn);
  lv_label_set_text(line_label, "Line Follow");
  lv_obj_center(line_label);

  lv_obj_t *scan_btn = lv_btn_create(test_scr);
  lv_obj_set_size(scan_btn, 180, 72);
  lv_obj_set_pos(scan_btn, 310, 190);
  apply_card_btn_style(scan_btn);
  lv_obj_add_event_cb(scan_btn, start_scan_left_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *scan_label = lv_label_create(scan_btn);
  lv_label_set_text(scan_label, "T-ScanL");
  lv_obj_center(scan_label);

  lv_obj_t *scan_btn_r = lv_btn_create(test_scr);
  lv_obj_set_size(scan_btn_r, 180, 72);
  lv_obj_set_pos(scan_btn_r, 540, 190);
  apply_card_btn_style(scan_btn_r);
  lv_obj_add_event_cb(scan_btn_r, start_scan_right_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *scan_label_r = lv_label_create(scan_btn_r);
  lv_label_set_text(scan_label_r, "T-ScanR");
  lv_obj_center(scan_label_r);

  lv_obj_t *stop_btn = lv_btn_create(test_scr);
  lv_obj_set_size(stop_btn, 180, 72);
  lv_obj_set_pos(stop_btn, 770, 190);
  apply_card_btn_style(stop_btn);
  lv_obj_add_event_cb(stop_btn, stop_vehicle_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *stop_label = lv_label_create(stop_btn);
  lv_label_set_text(stop_label, "Stop All");
  lv_obj_center(stop_label);

  lv_obj_t *panel = lv_obj_create(test_scr);
  lv_obj_set_size(panel, 980, 170);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -42);
  apply_info_panel_style(panel);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

  test_status_label = lv_label_create(panel);
  lv_label_set_text(test_status_label, "Test status: fixed route mode");
  lv_obj_set_style_text_color(test_status_label, lv_color_hex(0xFFF2F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(test_status_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(test_status_label, LV_ALIGN_CENTER, 0, 0);
}

void build_mission_screen() {
  mission_scr = lv_obj_create(NULL);
  apply_page_bg(mission_scr);

  lv_obj_t *title = lv_label_create(mission_scr);
  lv_label_set_text(title, "Mission Planner");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDDC674), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_36, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *back_btn = lv_btn_create(mission_scr);
  lv_obj_set_size(back_btn, 150, 68);
  lv_obj_set_pos(back_btn, 36, 34);
  apply_card_btn_style(back_btn);
  lv_obj_add_event_cb(back_btn, back_to_main_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Back");
  lv_obj_center(back_label);

  lv_obj_t *start_btn = lv_btn_create(mission_scr);
  lv_obj_set_size(start_btn, 170, 68);
  lv_obj_set_pos(start_btn, 970, 34);
  apply_card_btn_style(start_btn);
  lv_obj_add_event_cb(start_btn, mission_start_plan_event_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *start_label = lv_label_create(start_btn);
  lv_label_set_text(start_label, "Start");
  lv_obj_center(start_label);

  const char *names[4] = { "A", "B", "C", "D" };
  for (int i = 0; i < 4; ++i) {
    lv_obj_t *btn = lv_btn_create(mission_scr);
    mission_select_btns[i] = btn;
    lv_obj_set_size(btn, 200, 92);
    lv_obj_set_pos(btn, 110 + i * 240, 210);
    apply_card_btn_style(btn);
    lv_obj_add_event_cb(btn, mission_toggle_plan_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, names[i]);
    lv_obj_center(label);
  }

  lv_obj_t *panel = lv_obj_create(mission_scr);
  lv_obj_set_size(panel, 980, 170);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -42);
  apply_info_panel_style(panel);
  lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);

  mission_plan_label = lv_label_create(panel);
  lv_label_set_text(mission_plan_label, "Plan: A -> B -> C -> D -> END");
  lv_obj_set_style_text_color(mission_plan_label, lv_color_hex(0xFFF2F6), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(mission_plan_label, &lv_font_montserrat_24, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(mission_plan_label, LV_ALIGN_TOP_LEFT, 20, 18);

  mission_status_label = lv_label_create(panel);
  lv_label_set_text(mission_status_label, "Mission: planner ready");
  lv_obj_set_style_text_color(mission_status_label, lv_color_hex(0xFADBE4), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(mission_status_label, &lv_font_montserrat_22, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_align(mission_status_label, LV_ALIGN_BOTTOM_LEFT, 20, -18);
}

void task_ui(void *arg) {
  while (1) {
    VisionState local_vision;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      local_vision = g_vision;
      xSemaphoreGive(g_state_mutex);
    }

    const char *mode_text = "UNKNOWN";
    switch (g_system_mode) {
      case SYS_IDLE: mode_text = "IDLE"; break;
      case SYS_LINE_FOLLOW: mode_text = "LINE_FOLLOW"; break;
      case SYS_BRAKE_FOR_SCAN: mode_text = "BRAKE_FOR_SCAN"; break;
      case SYS_GIMBAL_SCAN: mode_text = "GIMBAL_SCAN"; break;
      case SYS_WAIT_ROUTE_DECISION: mode_text = "WAIT_ROUTE_DECISION"; break;
      case SYS_MISSION_COMPLETE: mode_text = "MISSION_COMPLETE"; break;
    }

    snprintf(ui_mode_buf, sizeof(ui_mode_buf), "Mode: %s", mode_text);
    snprintf(ui_vision_buf, sizeof(ui_vision_buf), "L=%d  E=%d  T=%d",
             local_vision.line_seen, local_vision.line_error, local_vision.t_node_seen);
    snprintf(ui_steer_buf, sizeof(ui_steer_buf), "Steer: target %d current %d",
             g_steering.target_angle, g_steering.current_angle);
    snprintf(ui_gimbal_buf, sizeof(ui_gimbal_buf), "Gimbal: pan %d tilt %d scan=%d",
             g_gimbal.pan_current, g_gimbal.tilt_current, g_gimbal.scan_done);
    snprintf(ui_hw_mode_buf, sizeof(ui_hw_mode_buf), "Mode: %s", mode_text);
    snprintf(ui_hw_vision_buf, sizeof(ui_hw_vision_buf), "Vision: line=%d  error=%d  t=%d",
             local_vision.line_seen, local_vision.line_error, local_vision.t_node_seen);
    snprintf(ui_hw_steer_buf, sizeof(ui_hw_steer_buf), "Steer servo: target=%d current=%d",
             g_steering.target_angle, g_steering.current_angle);
    snprintf(ui_hw_gimbal_buf, sizeof(ui_hw_gimbal_buf), "Gimbal: pan=%d tilt=%d done=%d",
             g_gimbal.pan_current, g_gimbal.tilt_current, g_gimbal.scan_done);

    snprintf(ui_test_buf, sizeof(ui_test_buf),
             "Test: %s | next=%s | step=%d/%d | line=%d err=%d t=%d",
             mode_text,
             (g_route_progress < g_route_task_count) ? work_point_name(g_route_tasks[g_route_progress].work_id) : "DONE",
             g_route_progress,
             g_route_task_count,
             local_vision.line_seen,
             local_vision.line_error,
             local_vision.t_node_seen);

    const char *task_name = "NONE";
    RouteTask *ui_task = get_active_route_task();
    if (ui_task != nullptr) {
      task_name = work_point_name(ui_task->work_id);
    } else if (g_route_progress < g_route_task_count) {
      task_name = work_point_name(g_route_tasks[g_route_progress].work_id);
    }

    snprintf(ui_mission_buf, sizeof(ui_mission_buf),
             "Mission: %s | next=%s | plan=%s | line=%d err=%d t=%d",
             mode_text,
             task_name,
             g_plan_running ? "RUNNING" : "READY",
             local_vision.line_seen,
             local_vision.line_error,
             local_vision.t_node_seen);

    int pos = snprintf(ui_plan_buf, sizeof(ui_plan_buf), "Plan: ");
    bool has_any = false;
    const char *plan_names[4] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4 && pos < (int)sizeof(ui_plan_buf) - 1; ++i) {
      if (g_plan_enabled[i]) {
        pos += snprintf(ui_plan_buf + pos, sizeof(ui_plan_buf) - pos, "%s%s", has_any ? " -> " : "", plan_names[i]);
        has_any = true;
      }
    }
    if (!has_any) {
      snprintf(ui_plan_buf, sizeof(ui_plan_buf), "Plan: END only");
    } else {
      snprintf(ui_plan_buf + pos, sizeof(ui_plan_buf) - pos, " -> END");
    }

    if (mode_label && strcmp(lv_label_get_text(mode_label), ui_mode_buf) != 0) {
      lv_label_set_text(mode_label, ui_mode_buf);
    }
    if (vision_label && strcmp(lv_label_get_text(vision_label), ui_vision_buf) != 0) {
      lv_label_set_text(vision_label, ui_vision_buf);
    }
    if (steer_label && strcmp(lv_label_get_text(steer_label), ui_steer_buf) != 0) {
      lv_label_set_text(steer_label, ui_steer_buf);
    }
    if (gimbal_label && strcmp(lv_label_get_text(gimbal_label), ui_gimbal_buf) != 0) {
      lv_label_set_text(gimbal_label, ui_gimbal_buf);
    }
    if (hardware_mode_label && strcmp(lv_label_get_text(hardware_mode_label), ui_hw_mode_buf) != 0) {
      lv_label_set_text(hardware_mode_label, ui_hw_mode_buf);
    }
    if (hardware_vision_label && strcmp(lv_label_get_text(hardware_vision_label), ui_hw_vision_buf) != 0) {
      lv_label_set_text(hardware_vision_label, ui_hw_vision_buf);
    }
    if (hardware_steer_label && strcmp(lv_label_get_text(hardware_steer_label), ui_hw_steer_buf) != 0) {
      lv_label_set_text(hardware_steer_label, ui_hw_steer_buf);
    }
    if (hardware_gimbal_label && strcmp(lv_label_get_text(hardware_gimbal_label), ui_hw_gimbal_buf) != 0) {
      lv_label_set_text(hardware_gimbal_label, ui_hw_gimbal_buf);
    }
    if (test_status_label && strcmp(lv_label_get_text(test_status_label), ui_test_buf) != 0) {
      lv_label_set_text(test_status_label, ui_test_buf);
    }
    if (mission_status_label && strcmp(lv_label_get_text(mission_status_label), ui_mission_buf) != 0) {
      lv_label_set_text(mission_status_label, ui_mission_buf);
    }
    if (mission_plan_label && strcmp(lv_label_get_text(mission_plan_label), ui_plan_buf) != 0) {
      lv_label_set_text(mission_plan_label, ui_plan_buf);
    }
    for (int i = 0; i < 4; ++i) {
      if (mission_select_btns[i] != nullptr) {
        lv_color_t bg = g_plan_enabled[i] ? lv_color_hex(0x7A4FD1) : lv_color_hex(0x4F458C);
        lv_obj_set_style_bg_color(mission_select_btns[i], bg, LV_PART_MAIN | LV_STATE_DEFAULT);
      }
    }

    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

// ==========================================
// setup / loop
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PA_ENABLE, OUTPUT);
  digitalWrite(PA_ENABLE, HIGH);

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  OpenMVSerial.begin(115200, SERIAL_8N1, OPENMV_RX, OPENMV_TX);

  motor_init();

  ledcAttachChannel(SERVO_STEER_PIN, 50, 12, 5);
  ledcAttachChannel(SERVO_PAN_PIN, 50, 12, 6);
  ledcAttachChannel(SERVO_TILT_PIN, 50, 12, 7);

  write_servo_angle(SERVO_STEER_PIN, 90);
  write_servo_angle(SERVO_PAN_PIN, 90);
  write_servo_angle(SERVO_TILT_PIN, 25);

  board = new esp_panel::board::Board();
  board->init();
  board->begin();
  if (board->getBacklight()) board->getBacklight()->setBrightness(100);

  es.begin(I2C_SDA, I2C_SCL, 400000);
  es.setVolume(90);

  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0, SDMMC_D1, SDMMC_D2, SDMMC_D3);
  SD_MMC.begin("/sdcard", false);
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT, I2S_MCLK);
  audio.setVolume(18);

  lvgl_port_init(board->getLCD(), board->getTouch());
  build_main_screen();
  build_hardware_screen();
  build_test_screen();
  build_mission_screen();
  lv_scr_load(main_scr);

  g_state_mutex = xSemaphoreCreateMutex();
  g_audio_queue = xQueueCreate(4, sizeof(char[32]));

  reset_route_progress();
  g_system_mode = SYS_IDLE;
  g_steering.current_angle = 90;
  g_steering.target_angle = 90;
  g_gimbal.pan_current = 90;
  g_gimbal.pan_target = 90;
  g_gimbal.tilt_current = 25;
  g_gimbal.tilt_target = 25;
  g_gimbal.mode = GIMBAL_CENTER;
  stop_motors();

  connect_wifi();

  xTaskCreatePinnedToCore(task_openmv_rx, "openmv_rx", 4096, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(task_servo_manager, "servo_mgr", 4096, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(task_vehicle_control, "vehicle_ctrl", 4096, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(task_state_machine, "state_machine", 4096, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(task_audio, "audio_task", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(task_ui, "ui_task", 8192, NULL, 5, NULL, 1);
  xTaskCreatePinnedToCore(task_cloud_status_push, "cloud_push", 6144, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(task_cloud_command_pull, "cloud_pull", 6144, NULL, 2, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
