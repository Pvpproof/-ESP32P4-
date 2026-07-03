#ifndef BAT_H
#define BAT_H

#include <Arduino.h>
#include "shared_val.hpp"
#define BAT_ADC  32
float bat_v = 0.;

void bat_init(){
    pinMode(BAT_ADC, INPUT);
}

// 将任意范围的浮点数映射到指定范围的整数
inline int map_float_to_int(float value, float in_min, float in_max, int out_min, int out_max) {
    return (int)((value - in_min) * (out_max - out_min) / (in_max - in_min) + out_min);
}

// 将3.55到4.2的浮点数映射到0-100的整形
inline int map_voltage_to_percentage(float voltage) {
    return map_float_to_int(voltage, 3.55, 4.2, 0, 100);
}

float get_bat_v(){
    static float r1 = 680.;
    static float r2 = 300.;
    static float coef = (r1+r2) / r2;
    bat_v = (float)analogRead(BAT_ADC) / 4096. * 3.3 * coef; 
    return bat_v;
}

int get_bat_percent(){
    float voltage = get_bat_v();
    return map_voltage_to_percentage(voltage);
}

void bat_loop(){
    static unsigned long lastMillis = 0;
    static const long xWait = 5000;
    if(millis() - lastMillis >= xWait){
      vbat_percent = get_bat_percent();
      lastMillis = millis();
    }
}



#endif