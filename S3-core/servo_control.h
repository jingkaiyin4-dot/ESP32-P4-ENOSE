/**
 * servo_control.h — 舵机开盖换气控制
 * 
 * 功能:
 *   - P4命令控制: lid_on / lid_off / lid_auto_on / lid_auto_off
 *   - 本地自动逻辑: 新鲜度≤阈值时自动开盖换气，延时后自动关盖
 *   - P4在线时由P4命令控制，离线时本地自动判断
 * 
 * 接线:
 *   ESP32-S3 GPIO4 (D2) → 舵机信号线(橙/黄)
 *   舵机 VCC → 5V, GND → GND
 * 
 * 舵机脉宽:
 *   CLOSE = 500us (0°, 盖子关闭)
 *   OPEN  = 2450us (最大角度, 盖子打开)
 */
#ifndef SERVO_CONTROL_H
#define SERVO_CONTROL_H

#include "config.h"
#include <ESP32Servo.h>

// 前向声明 (定义在 relay_control.h，避免循环依赖)
bool isP4Online();
void p4Heartbeat();

#include "model_runner.h"

// ==================== 全局状态 ====================
bool          lidOpen        = false;    // 当前盖子状态
bool          lidAutoMode    = true;     // 自动换气模式
unsigned long lidOpenTime    = 0;        // 开盖时间戳(ms)
Servo         lidServo;                  // 舵机对象

// ==================== 初始化 ====================
void initServo() {
  ESP32PWM::allocateTimer(1);  // 用Timer1，避让UV等
  lidServo.setPeriodHertz(50);
  lidServo.attach(SERVO_PIN, SERVO_CLOSE_PULSE, SERVO_OPEN_PULSE);
  lidServo.writeMicroseconds(SERVO_CLOSE_PULSE);  // 初始关闭
  lidOpen     = false;
  lidAutoMode = true;
  Serial.printf("[LID] Servo init on GPIO%d, pulse=%d(close)/%d(open)us, auto=%s\n",
                SERVO_PIN, SERVO_CLOSE_PULSE, SERVO_OPEN_PULSE,
                lidAutoMode ? "on" : "off");
}

// ==================== 开盖 ====================
void lidOpen_action() {
  if (lidOpen) {
    Serial.println("[LID] Already OPEN");
    return;
  }
  lidServo.writeMicroseconds(SERVO_OPEN_PULSE);
  lidOpen     = true;
  lidOpenTime = millis();
  Serial.printf("[LID] >>> OPEN <<< Auto-close in %lus\n",
                (unsigned long)(LID_AUTO_CLOSE_DELAY / 1000));
}

// ==================== 关盖 ====================
void lidClose_action() {
  if (!lidOpen) {
    Serial.println("[LID] Already CLOSED");
    return;
  }
  lidServo.writeMicroseconds(SERVO_CLOSE_PULSE);
  lidOpen = false;
  Serial.println("[LID] >>> CLOSED <<<");
}

// ==================== 自动换气判断(离线模式) ====================
// 仅在P4离线时生效，新鲜度低于阈值时开盖换气
void lidAutoCheck(int freshness, const char* className) {
  // P4在线时不做本地自动判断
  if (isP4Online()) return;
  // 空气不需要换气
  if (isAirClass(className)) return;
  // 非自动模式不触发
  if (!lidAutoMode) return;

  if (freshness <= LID_AUTO_OPEN_THRESHOLD && !lidOpen) {
    Serial.printf("[LID-LOCAL] Auto open: freshness=%d ≤ %d (P4 offline)\n",
                  freshness, LID_AUTO_OPEN_THRESHOLD);
    lidOpen_action();
  }
}

// ==================== 循环中调用: 自动关盖定时 ====================
void lidLoop() {
  if (!lidOpen) return;
  // 自动关盖: 开盖超过LID_AUTO_CLOSE_DELAY后关盖
  if (millis() - lidOpenTime >= LID_AUTO_CLOSE_DELAY) {
    lidClose_action();
    Serial.println("[LID] Auto-close timer expired");
  }
}

// ==================== 设置自动换气模式 ====================
void lidSetAuto(bool on) {
  lidAutoMode = on;
  Serial.printf("[LID] Auto mode: %s (freshness≤%d → open)\n",
                lidAutoMode ? "ON" : "OFF", LID_AUTO_OPEN_THRESHOLD);
}

// ==================== 舵机状态查询 ====================
void lidPrintStatus() {
  Serial.println("\n===== Lid Servo Status =====");
  Serial.printf("State:       %s\n", lidOpen ? "OPEN 🔓" : "CLOSED 🔒");
  Serial.printf("Auto mode:   %s\n", lidAutoMode ? "ON" : "OFF");
  Serial.printf("Threshold:   freshness ≤ %d → open\n", LID_AUTO_OPEN_THRESHOLD);
  Serial.printf("Auto-close:  %lus\n", (unsigned long)(LID_AUTO_CLOSE_DELAY / 1000));
  Serial.printf("GPIO:        %d\n", SERVO_PIN);
  if (lidOpen) {
    unsigned long remain = 0;
    if (millis() > lidOpenTime) {
      remain = (LID_AUTO_CLOSE_DELAY - (millis() - lidOpenTime)) / 1000;
    }
    Serial.printf("Remaining:   %lus\n", remain);
  }
  Serial.println("============================");
}

// ==================== P4舵机命令执行 ====================
void executeLidP4Action(const String& actionJson) {
  if (actionJson.indexOf("\"lid_on\"") >= 0) {
    lidOpen_action();
    Serial.println("[LID-P4] OK: lid_on");
  } else if (actionJson.indexOf("\"lid_off\"") >= 0) {
    lidClose_action();
    Serial.println("[LID-P4] OK: lid_off");
  } else if (actionJson.indexOf("\"lid_auto_on\"") >= 0) {
    lidSetAuto(true);
    Serial.println("[LID-P4] OK: lid_auto_on");
  } else if (actionJson.indexOf("\"lid_status\"") >= 0) {
    lidPrintStatus();
    Serial.println("[LID-P4] OK: lid_status");
  }
}

#endif
