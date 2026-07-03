/**
 * fan_control.h — 12V风扇控制 (3V继电器驱动)
 *
 * 硬件: 3V继电器模块, GPIO5控制, 继电器切换12V风扇电源
 * 控制: 串口/P4命令 > 自动模式(新鲜度<50触发排风)
 * 接线: GPIO5 → 继电器 IN, 继电器 COM→12V+, NO→风扇+, 风扇-→GND
 */
#ifndef FAN_CONTROL_H
#define FAN_CONTROL_H

#include "config.h"

#define FAN_PIN          5
#define FAN_ACTIVE_LOW   true
#define FAN_AUTO_FRESH   50

bool          fanOn          = false;
bool          fanAutoMode    = false;
unsigned long fanStartTime   = 0;
unsigned long fanDuration    = 0;

void initFan() {
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, FAN_ACTIVE_LOW ? HIGH : LOW);
  fanOn = false;
  fanAutoMode = false;
  fanDuration = 0;
  Serial.printf("[FAN] Init GPIO%d, active-low=%s, auto<=fresh%d\n",
                FAN_PIN, FAN_ACTIVE_LOW ? "yes" : "no", FAN_AUTO_FRESH);
}

void fanOn_action() {
  if (fanOn) { Serial.println("[FAN] Already ON"); return; }
  digitalWrite(FAN_PIN, FAN_ACTIVE_LOW ? LOW : HIGH);
  fanOn = true;
  fanStartTime = millis();
  Serial.println("[FAN] >>> ON <<<");
}

void fanOff_action() {
  if (!fanOn) { Serial.println("[FAN] Already OFF"); return; }
  digitalWrite(FAN_PIN, FAN_ACTIVE_LOW ? HIGH : LOW);
  fanOn = false;
  fanDuration = 0;
  Serial.println("[FAN] >>> OFF <<<");
}

void fanSetAuto(bool on) {
  fanAutoMode = on;
  Serial.printf("[FAN] Auto: %s (freshness<=%d)\n",
                on ? "ON" : "OFF", FAN_AUTO_FRESH);
}

void fanSetDuration(unsigned long s) {
  fanDuration = s * 1000;
  Serial.printf("[FAN] Duration: %lus\n", s);
}

void fanAutoCheck(int freshness, const char* className) {
  if (!fanAutoMode) return;
  // Air不需要排风
  if (strstr(className, "air")) return;
  if (freshness <= FAN_AUTO_FRESH && !fanOn) {
    Serial.printf("[FAN-AUTO] Trigger: freshness=%d\n", freshness);
    fanOn_action();
  }
}

void fanLoop() {
  if (!fanOn || fanDuration == 0) return;
  if (millis() - fanStartTime >= fanDuration) {
    fanOff_action();
    Serial.println("[FAN] Auto-off timer expired");
  }
}

void fanPrintStatus() {
  Serial.println("\n===== Fan Status =====");
  Serial.printf("State: %s\n", fanOn ? "ON" : "OFF");
  Serial.printf("Auto:  %s (freshness<=%d)\n",
                fanAutoMode ? "ON" : "OFF", FAN_AUTO_FRESH);
  if (fanOn && fanDuration > 0) {
    unsigned long r = (fanDuration - (millis() - fanStartTime)) / 1000;
    Serial.printf("Timer: %lus remaining\n", r);
  }
  Serial.println("========================");
}

#endif
