/**
 * atomization.h — 雾化加湿器控制
 *
 * 硬件: 雾化模块(按键式, GPIO3), LOW脉冲200ms模拟按键切换
 * 控制: 串口/P4命令 > 自动模式(湿度<40%触发)
 * 接线: GPIO3 → 雾化模块 SIG, VCC→3.3V/5V, GND→GND
 */
#ifndef ATOMIZATION_H
#define ATOMIZATION_H

#include "config.h"

#define FOG_PIN          3
#define FOG_PULSE_MS     200
#define FOG_AUTO_HUMIDITY 40

bool          fogOn          = false;
bool          fogAutoMode    = false;
unsigned long fogStartTime   = 0;
unsigned long fogDuration    = 0;

void initFog() {
  pinMode(FOG_PIN, OUTPUT);
  digitalWrite(FOG_PIN, HIGH);
  fogOn = false;
  fogAutoMode = false;
  fogDuration = 0;
  Serial.printf("[FOG] Init GPIO%d, pulse=%dms, auto<=%d%%\n",
                FOG_PIN, FOG_PULSE_MS, FOG_AUTO_HUMIDITY);
}

void fogPulse() {
  digitalWrite(FOG_PIN, LOW);
  delay(FOG_PULSE_MS);
  digitalWrite(FOG_PIN, HIGH);
}

void fogOn_action() {
  if (fogOn) { Serial.println("[FOG] Already ON"); return; }
  fogPulse();
  fogOn = true;
  fogStartTime = millis();
  Serial.println("[FOG] >>> ON <<<");
}

void fogOff_action() {
  if (!fogOn) { Serial.println("[FOG] Already OFF"); return; }
  fogPulse();
  fogOn = false;
  fogDuration = 0;
  Serial.println("[FOG] >>> OFF <<<");
}

void fogSetAuto(bool on) {
  fogAutoMode = on;
  Serial.printf("[FOG] Auto: %s (humidity<=%d%%)\n",
                on ? "ON" : "OFF", FOG_AUTO_HUMIDITY);
}

void fogSetDuration(unsigned long s) {
  fogDuration = s * 1000;
  Serial.printf("[FOG] Duration: %lus\n", s);
}

void fogAutoCheck(float humidity) {
  if (!fogAutoMode) return;
  if (humidity < FOG_AUTO_HUMIDITY && !fogOn) {
    Serial.printf("[FOG-AUTO] Trigger: humidity=%.1f%%\n", humidity);
    fogOn_action();
  }
}

void fogLoop() {
  if (!fogOn || fogDuration == 0) return;
  if (millis() - fogStartTime >= fogDuration) {
    fogOff_action();
    Serial.println("[FOG] Auto-off timer expired");
  }
}

void fogPrintStatus() {
  Serial.println("\n===== Fog Status =====");
  Serial.printf("State: %s\n", fogOn ? "ON" : "OFF");
  Serial.printf("Auto:  %s (humidity<=%d%%)\n",
                fogAutoMode ? "ON" : "OFF", FOG_AUTO_HUMIDITY);
  if (fogOn && fogDuration > 0) {
    unsigned long r = (fogDuration - (millis() - fogStartTime)) / 1000;
    Serial.printf("Timer: %lus remaining\n", r);
  }
  Serial.println("========================");
}

#endif
