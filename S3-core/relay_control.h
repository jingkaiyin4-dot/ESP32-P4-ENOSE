/**
 * relay_control.h — UV紫外线灯继电器控制
 * 
 * 控制优先级: P4命令 > 本地自动判断(离线模式)
 * 
 * - P4在线时: UV完全由P4命令控制，E-Nose本地自动判断被禁用
 * - P4离线时: 自动启用本地 air→fruit 判断（离线自控模式）
 * - P4在线判定: 收到P4命令即标记在线，超时(P4_OFFLINE_TIMEOUT)未收到则判定离线
 * 
 * 接线:
 *   ESP32-S3 GPIO1 (D1) → 继电器 IN
 *   继电器 VCC → 5V, GND → GND
 *   继电器 COM → 12V电源正极, NO → UV灯正极
 *   UV灯负极 → 12V电源GND
 */
#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include "config.h"
#include "servo_control.h"  // 舵机控制模块

// 前向声明 (ble_manager.h中定义，避免循环依赖)
void espnowSendCmdToSender(const String& cmd);
void sendWarmupToP4(uint16_t remainingSec);
#include "model_runner.h"

// ==================== P4 心跳超时配置 ====================
#define P4_OFFLINE_TIMEOUT  60000   // P4离线超时: 60秒无命令则判定离线

// ==================== 全局状态 ====================
bool          uvRelayOn        = false;    // 当前UV灯状态
bool          uvAutoMode       = true;     // 自动模式: 空气→水果时触发
unsigned long uvStartTime      = 0;        // UV开启时间戳(ms)
unsigned long uvOnDuration     = UV_ON_DURATION;  // UV开启时长(ms)
int           uvFreshThreshold = FRESH_THRESHOLD; // (保留,暂不用)
bool          uvLastWasAir     = true;     // 上一帧是否为空气(用于边沿检测)

// P4在线状态
bool          p4Online         = false;    // P4是否在线
unsigned long p4LastHeartbeat  = 0;        // P4最后心跳时间(ms)

// ==================== 初始化 ====================
void initRelay() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  uvRelayOn    = false;
  uvLastWasAir = true;
  p4Online     = false;
  p4LastHeartbeat = 0;
  Serial.printf("[UV] Relay init on GPIO%d, active-low=%s, auto=%s, duration=%lus\n",
                RELAY_PIN,
                RELAY_ACTIVE_LOW ? "yes" : "no",
                uvAutoMode ? "on" : "off",
                uvOnDuration / 1000);
  Serial.println("[UV] Control mode: P4 online=P4 cmd priority, P4 offline=local auto");
}

// ==================== P4 心跳管理 ====================
void p4Heartbeat() {
  p4LastHeartbeat = millis();
  if (!p4Online) {
    p4Online = true;
    Serial.println("[UV-P4] P4 online → P4 command priority");
  }
}

bool isP4Online() {
  if (!p4Online) return false;
  // 超时判定离线
  if (millis() - p4LastHeartbeat > P4_OFFLINE_TIMEOUT) {
    p4Online = false;
    Serial.println("[UV-P4] P4 offline (timeout) → local auto mode");
    return false;
  }
  return true;
}

// ==================== 开启UV灯 ====================
void uvRelayOn_action() {
  if (uvRelayOn) {
    Serial.println("[UV] Already ON");
    return;
  }
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW : HIGH);
  uvRelayOn    = true;
  uvStartTime  = millis();
  Serial.printf("[UV] >>> ON <<< Auto-off in %lus\n", uvOnDuration / 1000);
}

// ==================== 关闭UV灯 ====================
void uvRelayOff() {
  if (!uvRelayOn) {
    Serial.println("[UV] Already OFF");
    return;
  }
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  uvRelayOn = false;
  Serial.println("[UV] >>> OFF <<<");
}

// ==================== 自动消毒判断(离线模式) ====================
// 仅在P4离线时生效，P4在线时由P4命令控制
void uvAutoCheck(int freshness, const char* className) {
  // P4在线时不做本地自动判断
  if (isP4Online()) return;

  bool curIsAir = isAirClass(className);

  if (uvAutoMode && uvLastWasAir && !curIsAir && !uvRelayOn) {
    Serial.printf("[UV-LOCAL] Auto trigger: air→%s (P4 offline, local auto)\n", className);
    uvRelayOn_action();
  } else if (uvAutoMode && !uvLastWasAir && curIsAir && uvRelayOn) {
    Serial.println("[UV-LOCAL] Air detected while UV ON → turning off");
    uvRelayOff();
  }

  uvLastWasAir = curIsAir;
}

// ==================== 循环中调用: 定时关闭 + P4超时检测 + 安全提醒 ====================
void uvLoop() {
  // P4超时检测
  isP4Online();

  if (!uvRelayOn) return;

  unsigned long elapsed = millis() - uvStartTime;

  // 定时关闭
  if (elapsed >= uvOnDuration) {
    uvRelayOff();
    Serial.println("[UV] Auto-off timer expired");
    return;
  }

  // 每10秒提醒一次
  static unsigned long lastWarnMs = 0;
  if (millis() - lastWarnMs >= 10000) {
    lastWarnMs = millis();
    unsigned long remain = (uvOnDuration - elapsed) / 1000;
    Serial.printf("[UV] ⚠ UV ON - %lus remaining (ctrl: %s)\n", 
                  remain, isP4Online() ? "P4" : "local");
  }
}

// ==================== 设置自动模式 ====================
void uvSetAuto(bool on) {
  uvAutoMode = on;
  if (uvAutoMode && !uvRelayOn) {
    Serial.println("[UV] Auto ON → immediate UV start");
    uvRelayOn_action();
  }
  Serial.printf("[UV] Auto mode: %s (air→fruit trigger)\n", uvAutoMode ? "ON" : "OFF");
}

// ==================== 设置消毒时长 ====================
void uvSetDuration(unsigned long seconds) {
  uvOnDuration = seconds * 1000;
  Serial.printf("[UV] Duration set: %lus\n", seconds);
}

// ==================== 设置新鲜度阈值(保留接口) ====================
void uvSetThreshold(int threshold) {
  uvFreshThreshold = threshold;
  Serial.printf("[UV] Freshness threshold: %d (reserved)\n", threshold);
}

// ==================== UV状态查询 ====================
void uvPrintStatus() {
  Serial.println("\n===== UV Relay Status =====");
  Serial.printf("State:       %s\n", uvRelayOn ? "ON ⚡" : "OFF");
  Serial.printf("P4 online:   %s\n", isP4Online() ? "YES (P4 ctrl)" : "NO (local auto)");
  Serial.printf("Auto mode:   %s (air→fruit)\n", uvAutoMode ? "ON" : "OFF");
  Serial.printf("Last state:  %s\n", uvLastWasAir ? "Air" : "Fruit");
  Serial.printf("Duration:    %lus\n", uvOnDuration / 1000);
  Serial.printf("GPIO:        %d (active-%s)\n", RELAY_PIN, RELAY_ACTIVE_LOW ? "low" : "high");
  if (uvRelayOn) {
    unsigned long remain = 0;
    if (millis() > uvStartTime) {
      remain = (uvOnDuration - (millis() - uvStartTime)) / 1000;
    }
    Serial.printf("Remaining:   %lus\n", remain);
  }
  if (p4Online) {
    unsigned long sinceLast = (millis() - p4LastHeartbeat) / 1000;
    Serial.printf("P4 last cmd: %lus ago\n", sinceLast);
  }
  Serial.println("==========================");
}

// ==================== 执行单条P4命令 ====================
void executeP4Action(const String& actionJson) {
  // P4命令 → 标记心跳
  p4Heartbeat();

  if (actionJson.indexOf("\"uv_on\"") >= 0) {
    uvRelayOn_action();
    Serial.println("[UV-P4] OK: uv_on");
  } else if (actionJson.indexOf("\"uv_off\"") >= 0) {
    uvRelayOff();
    Serial.println("[UV-P4] OK: uv_off");
  } else if (actionJson.indexOf("\"uv_auto_on\"") >= 0) {
    uvSetAuto(true);
    Serial.println("[UV-P4] OK: uv_auto_on");
  } else if (actionJson.indexOf("\"uv_auto_off\"") >= 0) {
    uvSetAuto(false);
    Serial.println("[UV-P4] OK: uv_auto_off");
  } else if (actionJson.indexOf("\"uv_dur\"") >= 0) {
    int valIdx = actionJson.indexOf("\"val\"");
    if (valIdx < 0) valIdx = actionJson.indexOf("\"duration\"");
    if (valIdx >= 0) {
      int colonIdx = actionJson.indexOf(':', valIdx);
      if (colonIdx >= 0) {
        int numStart = colonIdx + 1;
        while (numStart < (int)actionJson.length() && actionJson[numStart] == ' ') numStart++;
        float mins = actionJson.substring(numStart).toFloat();
        unsigned long sec = (unsigned long)(mins * 60);  // P4 val单位=分钟→秒
        if (mins > 0 && mins <= 600) {
          uvSetDuration(sec);
          Serial.printf("[UV-P4] OK: uv_dur=%.1fmin (%lus)\n", mins, sec);
        } else {
          Serial.printf("[UV-P4] ERR: uv_dur invalid (0.1-600 min), got: %.1f\n", mins);
        }
      }
    }
  } else if (actionJson.indexOf("\"uv_status\"") >= 0) {
    uvPrintStatus();
    Serial.println("[UV-P4] OK: uv_status");
  } else if (actionJson.indexOf("\"lid_on\"") >= 0) {
    lidOpen_action();
    Serial.println("[UV-P4] OK: lid_on");
  } else if (actionJson.indexOf("\"lid_off\"") >= 0) {
    lidClose_action();
    Serial.println("[UV-P4] OK: lid_off");
  } else if (actionJson.indexOf("\"lid_auto_on\"") >= 0) {
    lidSetAuto(true);
    Serial.println("[UV-P4] OK: lid_auto_on");
  } else if (actionJson.indexOf("\"lid_auto_off\"") >= 0) {
    lidSetAuto(false);
    Serial.println("[UV-P4] OK: lid_auto_off");
  } else if (actionJson.indexOf("\"lid_status\"") >= 0) {
    lidPrintStatus();
    Serial.println("[UV-P4] OK: lid_status");
  } else if (actionJson.indexOf("\"fog_on\"") >= 0) {
    fogOn_action();
    Serial.println("[UV-P4] OK: fog_on");
  } else if (actionJson.indexOf("\"fog_off\"") >= 0) {
    fogOff_action();
    Serial.println("[UV-P4] OK: fog_off");
  } else if (actionJson.indexOf("\"fog_auto_on\"") >= 0) {
    fogSetAuto(true);
    Serial.println("[UV-P4] OK: fog_auto_on");
  } else if (actionJson.indexOf("\"fog_auto_off\"") >= 0) {
    fogSetAuto(false);
    Serial.println("[UV-P4] OK: fog_auto_off");
  } else if (actionJson.indexOf("\"fog_status\"") >= 0) {
    fogPrintStatus();
    Serial.println("[UV-P4] OK: fog_status");
  } else if (actionJson.indexOf("\"fog_dur\"") >= 0) {
    int valIdx = actionJson.indexOf("\"val\"");
    if (valIdx >= 0) {
      int colonIdx = actionJson.indexOf(':', valIdx);
      if (colonIdx >= 0) {
        int numStart = colonIdx + 1;
        while (numStart < (int)actionJson.length() && actionJson[numStart] == ' ') numStart++;
        float secs = actionJson.substring(numStart).toFloat();
        if (secs >= 1 && secs <= 3600) {
          fogSetDuration((unsigned long)secs);
          Serial.printf("[UV-P4] OK: fog_dur=%.0fs\n", secs);
        } else {
          Serial.printf("[UV-P4] ERR: fog_dur invalid (1-3600s): %.0f\n", secs);
        }
      }
    }
  } else if (actionJson.indexOf("\"fan_on\"") >= 0) {
    fanOn_action();
    Serial.println("[UV-P4] OK: fan_on");
  } else if (actionJson.indexOf("\"fan_off\"") >= 0) {
    fanOff_action();
    Serial.println("[UV-P4] OK: fan_off");
  } else if (actionJson.indexOf("\"fan_auto_on\"") >= 0) {
    fanSetAuto(true);
    Serial.println("[UV-P4] OK: fan_auto_on");
  } else if (actionJson.indexOf("\"fan_auto_off\"") >= 0) {
    fanSetAuto(false);
    Serial.println("[UV-P4] OK: fan_auto_off");
  } else if (actionJson.indexOf("\"fan_status\"") >= 0) {
    fanPrintStatus();
    Serial.println("[UV-P4] OK: fan_status");
  } else if (actionJson.indexOf("\"fan_dur\"") >= 0) {
    int valIdx = actionJson.indexOf("\"val\"");
    if (valIdx >= 0) {
      int colonIdx = actionJson.indexOf(':', valIdx);
      if (colonIdx >= 0) {
        int numStart = colonIdx + 1;
        while (numStart < (int)actionJson.length() && actionJson[numStart] == ' ') numStart++;
        float secs = actionJson.substring(numStart).toFloat();
        if (secs >= 1 && secs <= 3600) {
          fanSetDuration((unsigned long)secs);
          Serial.printf("[UV-P4] OK: fan_dur=%.0fs\n", secs);
        } else {
          Serial.printf("[UV-P4] ERR: fan_dur invalid (1-3600s): %.0f\n", secs);
        }
      }
    }
  } else {
    Serial.printf("[UV-P4] WARN: unknown action: %s\n", actionJson.c_str());
  }
}

// ==================== 处理P4发来的完整消息 ====================
// 支持三种格式:
//   1. 结构化JSON: {"status":"aging","actions":[{"cmd":"uv_auto_on"},{"cmd":"uv_dur","val":30}]}
//   2. 单条命令JSON: {"cmd":"uv_on"}
//   3. 纯文本命令: uv_on (向后兼容)
void handleP4UvCommand(const String& raw) {
  String json = raw;
  json.trim();
  Serial.printf("[UV-P4] <<<< Raw: %s\n", json.c_str());

  // ---- 先检查是否是需要转发给Sender的命令 ----
  String cmdToSender = "";
  if (!json.startsWith("{")) {
    // ---- 模型相关命令 ----
    // C6 Bridge 会将空格替换为下划线，所以需要同时匹配两种格式
    // "model info" 或 "model_info"
    if (json.equalsIgnoreCase("model info") || json.equalsIgnoreCase("model_info")) {
      p4Heartbeat();
      pendingModelInfo = true;
      Serial.println("[MODEL] Info requested by P4");
      return;
    }
    // "model list" 或 "model_list"
    if (json.equalsIgnoreCase("model list") || json.equalsIgnoreCase("model_list")) {
      p4Heartbeat();
      pendingModelList = true;
      Serial.println("[MODEL] List requested by P4");
      return;
    }

    // "model switch N" 或 "model_switch_N"
    if (json.startsWith("model switch ") || json.startsWith("model_switch_")) {
      p4Heartbeat();
      int idx;
      if (json.startsWith("model_switch_")) {
        idx = json.substring(13).toInt();  // "model_switch_".length() = 13
      } else {
        idx = json.substring(13).toInt();   // "model switch ".length() = 13
      }
      if (idx >= 0 && idx < modelCount) {
        model_loaded = false;
        if (loadModelByIndex(idx)) {
          model_loaded = true;
          modelReady = true;
          Serial.printf("[MODEL] Switched to [%d]: %s\n", idx, modelList[idx].filename);
          pendingModelInfo = true;  // 切换后自动上报新模型信息
        } else {
          Serial.printf("[MODEL] Switch FAILED: [%d]\n", idx);
          pendingModelInfo = true;  // 上报当前模型信息（切换失败）
        }
      } else {
        Serial.printf("[MODEL] Invalid index: %d (0-%d)\n", idx, modelCount - 1);
        pendingModelInfo = true;
      }
      // 切换后重新上报模型列表
      pendingModelList = true;
      return;
    }
    // "model load N" 或 "model_load_N"
    if (json.startsWith("model load ") || json.startsWith("model_load_")) {
      p4Heartbeat();
      int idx;
      if (json.startsWith("model_load_")) {
        idx = json.substring(11).toInt();  // "model_load_".length() = 11
      } else {
        idx = json.substring(11).toInt();   // "model load ".length() = 11
      }
      if (idx >= 0 && idx < modelCount) {
        model_loaded = false;
        if (loadModelByIndex(idx)) {
          model_loaded = true;
          modelReady = true;
          Serial.printf("[MODEL] Loaded [%d]: %s\n", idx, modelList[idx].filename);
          pendingModelInfo = true;
        } else {
          Serial.printf("[MODEL] Load FAILED: [%d]\n", idx);
          pendingModelInfo = true;
        }
      } else {
        Serial.printf("[MODEL] Invalid load index: %d (0-%d)\n", idx, modelCount - 1);
        pendingModelInfo = true;
      }
      pendingModelList = true;
      return;
    }
    // "model delete N" 或 "model_delete_N"
    if (json.startsWith("model delete ") || json.startsWith("model_delete_")) {
      p4Heartbeat();
      int idx;
      if (json.startsWith("model_delete_")) {
        idx = json.substring(13).toInt();  // "model_delete_".length() = 13
      } else {
        idx = json.substring(13).toInt();   // "model delete ".length() = 13
      }
      if (idx >= 0 && idx < modelCount) {
        // 不允许删除当前活跃模型
        if (idx == currentModelIndex) {
          Serial.printf("[MODEL] Cannot delete active model [%d]\n", idx);
        } else {
          String filepath = String(modelList[idx].filename);
          if (SD.remove(filepath.c_str())) {
            Serial.printf("[MODEL] Deleted [%d]: %s\n", idx, filepath.c_str());
            scanModels();  // 重新扫描
            // 如果删除的索引在当前索引之前，调整 currentModelIndex
            if (idx < currentModelIndex) {
              currentModelIndex--;
            }
          } else {
            Serial.printf("[MODEL] Delete FAILED: [%d]\n", idx);
          }
        }
      } else {
        Serial.printf("[MODEL] Invalid delete index: %d\n", idx);
      }
      pendingModelList = true;  // 删除后上报列表
      return;
    }
    // "model update ...", "model chunk ...", "model cancel"
    // "model config ...", "model norm ...", "model classes ..."
    // 以及下划线格式: "model_update_...", "model_chunk_...", "model_cancel" ... 等
    if (json.startsWith("model update ") || json.startsWith("model_update_") ||
        json.startsWith("model chunk ") || json.startsWith("model_chunk_") ||
        json.startsWith("model config ") || json.startsWith("model_config_") ||
        json.startsWith("model norm ") || json.startsWith("model_norm_") ||
        json.startsWith("model classes ") || json.startsWith("model_classes_") ||
        json.equalsIgnoreCase("model cancel") || json.equalsIgnoreCase("model_cancel")) {
      p4Heartbeat();
      // 将下划线格式还原为空格格式，保持 muHandleCommand 兼容
      // C6 Bridge 会把所有空格转下划线，包括参数间的空格
      // 所以 model_update_name_chunks_size_ver 中 name 本身可能含下划线
      String muCmd = json;
      if (muCmd.startsWith("model_update_")) {
        // 从尾部解析: 找最后3个下划线分割的参数(version, size, chunks)
        // 格式: model_update_{name}_{chunks}_{size}_{version}
        // version: 数字+点号 (如 1.0.0)
        // size: 纯数字 (如 9736)
        // chunks: 纯数字 (如 49)
        String payload = muCmd.substring(13);  // 去掉 "model_update_"
        String version = "0.0.0";
        String sizeStr = "";
        String chunksStr = "";
        String nameStr = "";
        
        // 从后往前找3个参数段
        int pos = payload.length() - 1;
        
        // 第1段: version (最后一个下划线后)
        int lastUs = payload.lastIndexOf('_');
        if (lastUs > 0) {
          version = payload.substring(lastUs + 1);
          payload = payload.substring(0, lastUs);
        }
        
        // 第2段: size (倒数第二个下划线后)
        lastUs = payload.lastIndexOf('_');
        if (lastUs > 0) {
          sizeStr = payload.substring(lastUs + 1);
          payload = payload.substring(0, lastUs);
        }
        
        // 第3段: chunks (倒数第三个下划线后)
        lastUs = payload.lastIndexOf('_');
        if (lastUs > 0) {
          chunksStr = payload.substring(lastUs + 1);
          nameStr = payload.substring(0, lastUs);
        } else {
          // 没有3段参数，可能格式不对
          nameStr = payload;
        }
        
        // 还原: name中的下划线保持不变(如 banana_fresh)
        muCmd = "model update " + nameStr + " " + chunksStr + " " + sizeStr + " " + version;
        Serial.printf("[P4→MU] Converted: %s\n", muCmd.c_str());
      } else if (muCmd.startsWith("model_chunk_")) {
        // "model_chunk_id_hexdata" → "model chunk id hexdata"
        // 注意: hexdata不含空格/下划线，只需替换第一个下划线
        String payload = muCmd.substring(12);  // 去掉 "model_chunk_"
        int firstUs = payload.indexOf('_');
        if (firstUs > 0) {
          muCmd = "model chunk " + payload.substring(0, firstUs) + " " + payload.substring(firstUs + 1);
        } else {
          muCmd = "model chunk " + payload;
        }
      } else if (muCmd.equalsIgnoreCase("model_cancel")) {
        muCmd = "model cancel";
      }
      // ---- 新增: model_norm_ 下划线格式还原 ----
      else if (muCmd.startsWith("model_norm_")) {
        // 格式: model_norm_<name>_<min_csv>_<max_csv> (min/max 纯数字逗号无下划线)
        String payload = muCmd.substring(11);  // 去掉 "model_norm_"
        // 找最后一个 _ → max 分隔符
        int maxSep = payload.lastIndexOf('_');
        if (maxSep > 0) {
          String maxPart = payload.substring(maxSep + 1);
          String restPart = payload.substring(0, maxSep);
          // restPart = <name>_<min_csv>, min_csv 无下划线
          // 从右往左数15个逗号，再往前找 _ 即 name 分隔符
          int commaCount = 0, pos = restPart.length() - 1;
          while (pos >= 0 && commaCount < 15) {
            if (restPart.charAt(pos) == ',') commaCount++;
            pos--;
          }
          while (pos >= 0 && restPart.charAt(pos) != '_') pos--;
          if (pos >= 0) {
            String namePart = restPart.substring(0, pos);
            String minPart = restPart.substring(pos + 1);
            muCmd = "model norm " + namePart + " " + minPart + " " + maxPart;
            Serial.printf("[P4→MU] Converted: %s\n", muCmd.c_str());
          }
        }
      }
      // ---- 新增: model_classes_ 下划线格式还原 ----
      else if (muCmd.startsWith("model_classes_")) {
        String payload = muCmd.substring(14);  // 去掉 "model_classes_"
        int sep = payload.lastIndexOf('_');
        if (sep > 0) {
          muCmd = "model classes " + payload.substring(0, sep) + " " + payload.substring(sep + 1);
          Serial.printf("[P4→MU] Converted: %s\n", muCmd.c_str());
        }
      }
      // ---- 新增: model_config_ 下划线格式还原 ----
      else if (muCmd.startsWith("model_config_")) {
        String payload = muCmd.substring(13);  // 去掉 "model_config_"
        // JSON 以 { 开头，找 { 前最后一个 _
        int brace = payload.indexOf('{');
        if (brace > 0) {
          int sep = payload.lastIndexOf('_', brace);
          if (sep > 0) {
            muCmd = "model config " + payload.substring(0, sep) + " " + payload.substring(sep + 1);
            Serial.printf("[P4→MU] Converted: %s\n", muCmd.c_str());
          }
        }
      }
      muHandleCommand(muCmd);
      return;
    }

if (json.equalsIgnoreCase("skip warmup") || json.equalsIgnoreCase("skip_warmup")) {
      cmdToSender = "skip warmup";
    } else if (json.startsWith("uv_dur_") || json.startsWith("uv_dur ")) {
      // P4发送 uv_dur_N (C6空格转下划线), 单位=分钟
      // 支持 uv_dur_1, uv_dur_1.5, uv_dur 2 等
      p4Heartbeat();
      String numStr = json.substring(7);  // 跳过 "uv_dur_" 或 "uv_dur "
      float mins = numStr.toFloat();
      if (mins > 0 && mins <= 600) {
        unsigned long sec = (unsigned long)(mins * 60);
        uvSetDuration(sec);
        Serial.printf("[UV-P4] OK: uv_dur=%.1fmin (%lus)", mins, sec);
      } else {
        Serial.printf("[UV-P4] ERR: uv_dur invalid (0.1-600 min), got: %s\n", numStr.c_str());
      }
      return;
    } else if (json.startsWith("fog_dur_") || json.startsWith("fog_dur ")) {
      // C6 Bridge: fog_dur_30 → fog dur 30s
      p4Heartbeat();
      String numStr = json.substring(8);
      float secs = numStr.toFloat();
      if (secs >= 1 && secs <= 3600) {
        fogSetDuration((unsigned long)secs);
        Serial.printf("[FOG-P4] OK: fog_dur=%.0fs\n", secs);
      } else {
        Serial.printf("[FOG-P4] ERR: fog_dur invalid (1-3600s): %.0f\n", secs);
      }
      return;
    } else if (json.startsWith("fan_dur_") || json.startsWith("fan_dur ")) {
      // C6 Bridge: fan_dur_30 → fan dur 30s
      p4Heartbeat();
      String numStr = json.substring(8);
      float secs = numStr.toFloat();
      if (secs >= 1 && secs <= 3600) {
        fanSetDuration((unsigned long)secs);
        Serial.printf("[FAN-P4] OK: fan_dur=%.0fs\n", secs);
      } else {
        Serial.printf("[FAN-P4] ERR: fan_dur invalid (1-3600s): %.0f\n", secs);
      }
      return;
    } else {
      // 其他纯文本命令: P4心跳 + 本地执行
      p4Heartbeat();
      String wrapped = "{\"cmd\":\"" + json + "\"}";
      executeP4Action(wrapped);
      return;
    }
  } else {
    // JSON格式: 检查skip_warmup和UV命令
    if (json.indexOf("\"skip_warmup\"") >= 0 || json.indexOf("\"skip warmup\"") >= 0) {
      if (json.indexOf("\"cmd\"") >= 0 || json.indexOf("\"cmd\"") < 0) {
        cmdToSender = "skip warmup";
      }
    }
  }
  
  if (cmdToSender.length() > 0) {
    p4Heartbeat();  // P4发了命令，标记在线
    espnowSendCmdToSender(cmdToSender);
    Serial.printf("[P4→Sender] Forwarded: %s\n", cmdToSender.c_str());

    // skip_warmup: E-Nose也要响应——清除自身warmup状态，通知P4已完成
    if (cmdToSender == "skip warmup") {
      warmupPending = false;
      lastWarmupRemaining = 0;
      sendWarmupToP4(0);  // 通知P4预热已跳过(remaining=0)
      Serial.println("[Warmup] Skipped by P4 command");
    }
    return;
  }

  // 格式2: 单条命令JSON(无actions数组)
  if (json.indexOf("\"actions\"") < 0) {
    if (json.indexOf("\"cmd\"") >= 0) {
      executeP4Action(json);
    } else {
      // P4状态反馈也算心跳
      p4Heartbeat();
      Serial.printf("[UV-P4] <<<< Status only (no actions/cmd)\n");
    }
    return;
  }

  // 格式1: 结构化JSON, 解析actions数组
  // 状态反馈也算心跳
  p4Heartbeat();

  int statusIdx = json.indexOf("\"status\"");
  if (statusIdx >= 0) {
    int colonIdx = json.indexOf(':', statusIdx);
    int q1 = json.indexOf('"', colonIdx + 1);
    int q2 = json.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 >= 0) {
      Serial.printf("[UV-P4] Status: %s\n", json.substring(q1 + 1, q2).c_str());
    }
  }
  int analysisIdx = json.indexOf("\"analysis\"");
  if (analysisIdx >= 0) {
    int colonIdx = json.indexOf(':', analysisIdx);
    int q1 = json.indexOf('"', colonIdx + 1);
    int q2 = json.indexOf('"', q1 + 1);
    if (q1 >= 0 && q2 >= 0) {
      Serial.printf("[UV-P4] Analysis: %s\n", json.substring(q1 + 1, q2).c_str());
    }
  }

  int arrStart = json.indexOf('[');
  int arrEnd   = json.lastIndexOf(']');
  if (arrStart < 0) {
    Serial.println("[UV-P4] WARN: actions array not found");
    return;
  }

  String arr;
  if (arrEnd > arrStart) {
    arr = json.substring(arrStart + 1, arrEnd);
  } else {
    arr = json.substring(arrStart + 1);
    Serial.println("[UV-P4] WARN: JSON truncated, extracting partial actions");
  }
  int actionCount = 0;
  int pos = 0;
  while (pos < (int)arr.length()) {
    int objStart = arr.indexOf('{', pos);
    if (objStart < 0) break;
    int objEnd = arr.indexOf('}', objStart);
    if (objEnd < 0) break;

    String actionObj = arr.substring(objStart, objEnd + 1);
    actionCount++;
    Serial.printf("[UV-P4] Action[%d]: %s\n", actionCount, actionObj.c_str());
    executeP4Action(actionObj);
    pos = objEnd + 1;
  }

  if (actionCount == 0) {
    Serial.println("[UV-P4] No actions in array");
  } else {
    Serial.printf("[UV-P4] Executed %d action(s)\n", actionCount);
  }
}

#endif
