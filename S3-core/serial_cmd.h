/**
 * serial_cmd.h — 串口命令处理
 */
#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include "config.h"
#include "sd_manager.h"
#include "model_runner.h"
#include "ble_manager.h"
#include "relay_control.h"

void handleSerialCommand(const String& cmd) {
  if (cmd.equalsIgnoreCase("info")) {
    Serial.println("\n===== Model Info =====");
    if (model_loaded && interpreter) {
      TfLiteTensor* input_tensor  = interpreter->input(0);
      TfLiteTensor* output_tensor = interpreter->output(0);
      Serial.printf("Model: %s\n", currentModelFile);
      Serial.printf("Input dims: [");
      for (int i = 0; i < input_tensor->dims->size; i++) {
        Serial.printf("%d", input_tensor->dims->data[i]);
        if (i < input_tensor->dims->size - 1) Serial.printf(", ");
      }
      Serial.printf("]\n");
      Serial.printf("Output dims: [");
      for (int i = 0; i < output_tensor->dims->size; i++) {
        Serial.printf("%d", output_tensor->dims->data[i]);
        if (i < output_tensor->dims->size - 1) Serial.printf(", ");
      }
      Serial.printf("]\n");
      Serial.printf("Classes: %d\n", numClasses);
      Serial.printf("Norm params: %s\n", normParams.loaded ? "Loaded" : "Not loaded");
    } else {
      Serial.println("No model loaded");
    }
    Serial.println("======================");

  } else if (cmd.equalsIgnoreCase("model list") || cmd.equalsIgnoreCase("models")) {
    scanModels();
    listModels();

  } else if (cmd.startsWith("model load ")) {
    int idx = cmd.substring(11).toInt();
    Serial.printf("Loading model #%d...\n", idx);
    model_loaded = false;
    if (loadModelByIndex(idx)) {
      model_loaded = true;
      Serial.printf("Switched to model [%d]: %s\n", idx, modelList[idx].filename);
      pendingModelInfo = true;  // 切换后自动上报
    }

  } else if (cmd.startsWith("model switch ")) {
    int idx = cmd.substring(13).toInt();
    Serial.printf("Switching to model #%d...\n", idx);
    model_loaded = false;
    if (idx >= 0 && idx < modelCount && loadModelByIndex(idx)) {
      model_loaded = true;
      Serial.printf("Switched to model [%d]: %s\n", idx, modelList[idx].filename);
      pendingModelInfo = true;
    } else {
      Serial.printf("Invalid model index: %d (0-%d)\n", idx, modelCount - 1);
      pendingModelInfo = true;
    }

  } else if (cmd.startsWith("model delete ")) {
    int idx = cmd.substring(13).toInt();
    if (idx >= 0 && idx < modelCount) {
      if (idx == currentModelIndex) {
        Serial.printf("Cannot delete active model [%d]\n", idx);
      } else {
        String filepath = String(modelList[idx].filename);
        if (SD.remove(filepath.c_str())) {
          Serial.printf("Deleted model [%d]: %s\n", idx, filepath.c_str());
          scanModels();
          if (idx < currentModelIndex) currentModelIndex--;
          pendingModelList = true;
        } else {
          Serial.printf("Delete FAILED: [%d]\n", idx);
        }
      }
    } else {
      Serial.printf("Invalid delete index: %d\n", idx);
    }

  } else if (cmd.equalsIgnoreCase("model rescan")) {
    scanModels();
    listModels();

  } else if (cmd.equalsIgnoreCase("classes")) {
    Serial.printf("\n===== Classes (%d) =====\n", numClasses);
    for (int i = 0; i < numClasses; i++) {
      Serial.printf("  [%d] %s\n", i, classNames[i]);
    }
    Serial.println("======================");

  } else if (cmd.equalsIgnoreCase("status")) {
    Serial.println("\n===== Status =====");
    Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
    Serial.printf("Model Loaded: %s\n", model_loaded ? "Yes" : "No");
    if (model_loaded) {
      Serial.printf("Current Model: [%d] %s\n", currentModelIndex, currentModelFile);
    }
    Serial.printf("Models on SD: %d\n", modelCount);
    Serial.printf("SD Card: %s\n", sd_ready ? "OK" : "Failed");
    Serial.printf("ESP-NOW: %s (rxPkt=%lu rxData=%lu rxWarmup=%lu)\n",
                  espnowReady ? "Ready" : "Not ready",
                  rxPktCount, rxDataCount, rxWarmupCount);
    Serial.printf("Free Heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("===================");

  } else if (cmd.equalsIgnoreCase("connect")) {
    Serial.println("[ESPNOW] Already using ESP-NOW, no reconnect needed");

  // ---- UV 继电器命令 ----
  } else if (cmd.equalsIgnoreCase("uv on")) {
    uvRelayOn_action();
  } else if (cmd.equalsIgnoreCase("uv off")) {
    uvRelayOff();
  } else if (cmd.equalsIgnoreCase("uv auto on") || cmd.equalsIgnoreCase("uv auto")) {
    uvSetAuto(true);
  } else if (cmd.equalsIgnoreCase("uv auto off")) {
    uvSetAuto(false);
  } else if (cmd.equalsIgnoreCase("uv status")) {
    uvPrintStatus();
  } else if (cmd.startsWith("uv dur ")) {
    unsigned long sec = cmd.substring(7).toInt();
    if (sec >= 1 && sec <= 600) {
      uvSetDuration(sec);
    } else {
      Serial.println("[UV] Duration range: 1-600 seconds");
    }
  } else if (cmd.startsWith("uv thresh ")) {
    int t = cmd.substring(10).toInt();
    if (t >= 0 && t <= 100) {
      uvSetThreshold(t);
    } else {
      Serial.println("[UV] Threshold range: 0-100");
    }

  // ---- 舵机(开盖换气)命令 ----
  } else if (cmd.equalsIgnoreCase("lid on") || cmd.equalsIgnoreCase("lid_on")) {
    lidOpen_action();
  } else if (cmd.equalsIgnoreCase("lid off") || cmd.equalsIgnoreCase("lid_off")) {
    lidClose_action();
  } else if (cmd.equalsIgnoreCase("lid auto on") || cmd.equalsIgnoreCase("lid auto")) {
    lidSetAuto(true);
  } else if (cmd.equalsIgnoreCase("lid auto off")) {
    lidSetAuto(false);
  } else if (cmd.equalsIgnoreCase("lid status")) {
    lidPrintStatus();

  // ---- 雾化加湿器命令 ----
  } else if (cmd.equalsIgnoreCase("fog on")) {
    fogOn_action();
  } else if (cmd.equalsIgnoreCase("fog off")) {
    fogOff_action();
  } else if (cmd.equalsIgnoreCase("fog auto on") || cmd.equalsIgnoreCase("fog auto")) {
    fogSetAuto(true);
  } else if (cmd.equalsIgnoreCase("fog auto off")) {
    fogSetAuto(false);
  } else if (cmd.equalsIgnoreCase("fog status")) {
    fogPrintStatus();
  } else if (cmd.startsWith("fog dur ")) {
    unsigned long sec = cmd.substring(8).toInt();
    if (sec >= 1 && sec <= 3600) {
      fogSetDuration(sec);
    } else {
      Serial.println("[FOG] Duration range: 1-3600 seconds");
    }

  // ---- 风扇控制命令 ----
  } else if (cmd.equalsIgnoreCase("fan on")) {
    fanOn_action();
  } else if (cmd.equalsIgnoreCase("fan off")) {
    fanOff_action();
  } else if (cmd.equalsIgnoreCase("fan auto on") || cmd.equalsIgnoreCase("fan auto")) {
    fanSetAuto(true);
  } else if (cmd.equalsIgnoreCase("fan auto off")) {
    fanSetAuto(false);
  } else if (cmd.equalsIgnoreCase("fan status")) {
    fanPrintStatus();
  } else if (cmd.startsWith("fan dur ")) {
    unsigned long sec = cmd.substring(8).toInt();
    if (sec >= 1 && sec <= 3600) {
      fanSetDuration(sec);
    } else {
      Serial.println("[FAN] Duration range: 1-3600 seconds");
    }

  } else if (cmd.equalsIgnoreCase("mu status")) {
    muPrintStatus();
  } else if (cmd.equalsIgnoreCase("mu cancel")) {
    if (muCtx.state != MUS_IDLE) {
      Serial.println("[MU] Manual cancel");
      muSendAck("model_fail", -1);
      muCleanup();
    } else {
      Serial.println("[MU] No update in progress");
    }

  // ---- 模型 OTA 命令（串口直通，与 P4 下发的 handleP4UvCommand 路径一致）----
  } else if (cmd.startsWith("model update ") || cmd.startsWith("model_update_")) {
    // 串口发起的 model update，直接交给 muHandleCommand
    // 注意：串口不受 C6 Bridge 空格转下划线影响，但保留两种格式兼容
    String muCmd = cmd;
    if (muCmd.startsWith("model_update_")) {
      // 下划线格式还原（与 handleP4UvCommand 逻辑一致）
      String payload = muCmd.substring(13);
      String version = "0.0.0", sizeStr = "", chunksStr = "", nameStr = "";
      int lastUs = payload.lastIndexOf('_');
      if (lastUs > 0) { version = payload.substring(lastUs + 1); payload = payload.substring(0, lastUs); }
      lastUs = payload.lastIndexOf('_');
      if (lastUs > 0) { sizeStr = payload.substring(lastUs + 1); payload = payload.substring(0, lastUs); }
      lastUs = payload.lastIndexOf('_');
      if (lastUs > 0) { chunksStr = payload.substring(lastUs + 1); nameStr = payload.substring(0, lastUs); }
      else { nameStr = payload; }
      muCmd = "model update " + nameStr + " " + chunksStr + " " + sizeStr + " " + version;
      Serial.printf("[SERIAL→MU] Converted: %s\n", muCmd.c_str());
    }
    muHandleCommand(muCmd);
  } else if (cmd.startsWith("model chunk ") || cmd.startsWith("model_chunk_")) {
    String muCmd = cmd;
    if (muCmd.startsWith("model_chunk_")) {
      String payload = muCmd.substring(12);
      int firstUs = payload.indexOf('_');
      if (firstUs > 0) {
        muCmd = "model chunk " + payload.substring(0, firstUs) + " " + payload.substring(firstUs + 1);
      } else {
        muCmd = "model chunk " + payload;
      }
    }
    muHandleCommand(muCmd);
  } else if (cmd.equalsIgnoreCase("model cancel") || cmd.equalsIgnoreCase("model_cancel")) {
    muHandleCommand("model cancel");

  } else if (cmd.length() > 0) {
    // 通过 ESP-NOW 发送命令到 Sender
    if (espnowReady) {
      espnowSendCmdToSender(cmd);
    } else {
      Serial.println("[CMD] ESP-NOW not ready, cannot send command");
    }
  }
}

#endif
