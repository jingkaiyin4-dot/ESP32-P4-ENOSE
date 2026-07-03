/**
 * tdm_test.h — TDM 共存压力测试模块
 * 测试 ESP-NOW (Sender/Receiver/Bridge) + WiFi + TFLite 共存性能
 *
 * 测试维度:
 *   1. ESP-NOW 吞吐量 (Sender->Receiver 接收速率, Receiver->Bridge 发送速率)
 *   2. 内存压力 (heap/PSRAM 在模型加载+推理+BLE 同时运行时的变化)
 *   3. 重连稳定性 (Sender BLE 断连/重连频率)
 *   4. WiFi+BLE 共存 (WiFi 模式下 BLE 丢包/延迟)
 */
#ifndef TDM_TEST_H
#define TDM_TEST_H

#include "config.h"

// ==================== 测试状态 ====================
static bool     tdmActive        = false;
static uint32_t tdmStartMs       = 0;
static uint32_t tdmLastReportMs  = 0;
#define TDM_REPORT_INTERVAL 5000

// ==================== 包统计 ====================
static uint32_t tdmP4SentCount     = 0;
static uint32_t tdmP4LastCount     = 0;
static uint32_t tdmSenderReconnects = 0;

// ==================== 内存快照 ====================
static uint32_t tdmMinHeap        = 0xFFFFFFFF;
static uint32_t tdmMinPsram       = 0xFFFFFFFF;
static uint32_t tdmMaxHeapUsed    = 0;
static uint32_t tdmMaxPsramUsed   = 0;

// ==================== 推理统计 ====================
static uint32_t tdmInferenceCount  = 0;
static uint32_t tdmInferenceLastCount = 0;
static uint32_t tdmInferenceOk     = 0;
static uint32_t tdmInferenceFail   = 0;

// ==================== 启动 TDM 测试 ====================
void tdmTestStart() {
  if (tdmActive) {
    Serial.println("[TDM] Test already running! Use 'tdm stop' first.");
    return;
  }

  tdmActive          = true;
  tdmStartMs         = millis();
  tdmLastReportMs    = tdmStartMs;
  tdmP4SentCount     = 0;
  tdmP4LastCount     = 0;
  tdmSenderReconnects = 0;
  tdmMinHeap         = 0xFFFFFFFF;
  tdmMinPsram        = 0xFFFFFFFF;
  tdmMaxHeapUsed     = 0;
  tdmMaxPsramUsed    = 0;
  tdmInferenceCount  = 0;
  tdmInferenceLastCount = 0;
  tdmInferenceOk     = 0;
  tdmInferenceFail   = 0;

  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t psramFree = ESP.getFreePsram();

  Serial.println("\n===== TDM Coexistence Test Started =====");
  Serial.printf("  Heap free:  %u bytes\n", heapFree);
  Serial.printf("  PSRAM free: %u bytes\n", psramFree);
  Serial.printf("  Model: %s\n", model_loaded ? currentModelFile : "not loaded");
  Serial.printf("  ESP-NOW Sender: %s\n", espnowReady ? "ready" : "down");
  Serial.printf("  ESP-NOW Bridge: %s\n", espnowReady ? "ready" : "down");
  Serial.println("  Report interval: 5 sec");
  Serial.println("  Commands: tdm status | tdm mem | tdm stop");
  Serial.println("==========================================\n");
}

// ==================== 停止 TDM 测试 ====================
void tdmTestStop() {
  if (!tdmActive) {
    Serial.println("[TDM] No test running.");
    return;
  }

  tdmActive = false;
  uint32_t elapsed = (millis() - tdmStartMs) / 1000;

  Serial.println("\n===== TDM Test Stopped =====");
  Serial.printf("  Duration: %u sec\n", elapsed);
  Serial.printf("  P4 sends: %u (%.1f/sec)\n",
                tdmP4SentCount,
                elapsed > 0 ? (float)tdmP4SentCount / elapsed : 0);
  Serial.printf("  Inferences: %u total, %u OK, %u fail\n",
                tdmInferenceCount, tdmInferenceOk, tdmInferenceFail);
  Serial.printf("  Sender reconnects: %u\n", tdmSenderReconnects);
  Serial.printf("  Min heap free: %u bytes\n",
                tdmMinHeap == 0xFFFFFFFF ? 0 : tdmMinHeap);
  Serial.printf("  Min PSRAM free: %u bytes\n",
                tdmMinPsram == 0xFFFFFFFF ? 0 : tdmMinPsram);
  Serial.printf("  Heap delta: +%u bytes (max used)\n", tdmMaxHeapUsed);
  Serial.printf("  PSRAM delta: +%u bytes (max used)\n", tdmMaxPsramUsed);

  float dropRate = (rxDataCount > 0)
    ? (1.0f - (float)tdmP4SentCount / rxDataCount) * 100.0f
    : 0.0f;
  Serial.printf("  P4 delivery rate: %.1f%% (drop: %.1f%%)\n",
                100.0f - dropRate, dropRate);
  Serial.println("============================\n");
}

// ==================== TDM 状态快照 ====================
void tdmTestStatus() {
  if (!tdmActive) {
    Serial.println("[TDM] No test running. Use 'tdm test' to start.");
    return;
  }

  uint32_t elapsed = (millis() - tdmStartMs) / 1000;
  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t psramFree = ESP.getFreePsram();

  Serial.println("\n----- TDM Snapshot -----");
  Serial.printf("  Runtime: %u sec\n", elapsed);
  Serial.printf("  Rx packets: %lu (data=%lu warmup=%lu)\n",
                rxPktCount, rxDataCount, rxWarmupCount);
  Serial.printf("  P4 sends: %u\n", tdmP4SentCount);
  Serial.printf("  Inferences: %u (%u ok, %u fail)\n",
                tdmInferenceCount, tdmInferenceOk, tdmInferenceFail);
  Serial.printf("  Sender reconnects: %u\n", tdmSenderReconnects);
  Serial.printf("  Heap free: %u bytes\n", heapFree);
  Serial.printf("  PSRAM free: %u bytes\n", psramFree);
  Serial.printf("  ESP-NOW Sender: %s\n", espnowReady ? "OK" : "DOWN");
  Serial.printf("  ESP-NOW Bridge: %s\n", espnowReady ? "OK" : "DOWN");
  Serial.printf("  WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Off");
  Serial.println("------------------------");
}

// ==================== 内存详细报告 ====================
void tdmMemoryReport() {
  uint32_t heapFree   = ESP.getFreeHeap();
  uint32_t heapTotal  = ESP.getHeapSize();
  uint32_t psramFree  = ESP.getFreePsram();
  uint32_t psramTotal = ESP.getPsramSize();
  uint32_t heapUsed   = heapTotal - heapFree;
  uint32_t psramUsed  = psramTotal - psramFree;

  Serial.println("\n===== Memory Report =====");
  Serial.printf("  Heap:  %u / %u bytes free (%.1f%% used)\n",
                heapFree, heapTotal, (float)heapUsed / heapTotal * 100);
  Serial.printf("  PSRAM: %u / %u bytes free (%.1f%% used)\n",
                psramFree, psramTotal,
                psramTotal > 0 ? (float)psramUsed / psramTotal * 100 : 0);

  if (tdmActive) {
    Serial.printf("  Min heap (test): %u bytes\n",
                  tdmMinHeap == 0xFFFFFFFF ? 0 : tdmMinHeap);
    Serial.printf("  Min PSRAM (test): %u bytes\n",
                  tdmMinPsram == 0xFFFFFFFF ? 0 : tdmMinPsram);
  }

  if (model_buffer && model_loaded) {
    Serial.printf("  Model buffer: allocated (PSRAM)\n");
  }
  Serial.printf("  Tensor arena: %d bytes (static)\n", TENSOR_ARENA_SIZE);
  Serial.printf("  Model list: %d models, %zu bytes/entry\n",
                modelCount, sizeof(ModelInfo));
  Serial.println("=========================");
}

// ==================== TDM 循环（在 loop 中调用） ====================
void tdmTestLoop() {
  if (!tdmActive) return;

  uint32_t now = millis();

  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t psramFree = ESP.getFreePsram();
  if (heapFree  < tdmMinHeap)  tdmMinHeap  = heapFree;
  if (psramFree < tdmMinPsram) tdmMinPsram = psramFree;
  uint32_t heapUsed  = ESP.getHeapSize() - heapFree;
  uint32_t psramUsed = ESP.getPsramSize() - psramFree;
  if (heapUsed  > tdmMaxHeapUsed)  tdmMaxHeapUsed  = heapUsed;
  if (psramUsed > tdmMaxPsramUsed) tdmMaxPsramUsed = psramUsed;

  if (now - tdmLastReportMs >= TDM_REPORT_INTERVAL) {
    tdmLastReportMs = now;
    uint32_t p4Delta  = tdmP4SentCount - tdmP4LastCount;
    uint32_t infDelta = tdmInferenceCount - tdmInferenceLastCount;
    tdmP4LastCount    = tdmP4SentCount;
    tdmInferenceLastCount = tdmInferenceCount;
    float elapsed = TDM_REPORT_INTERVAL / 1000.0f;

    Serial.printf("[TDM] %u sec | P4: %u (%.1f/s) | Inf: %u (%.1f/s) | "
                  "Heap: %u | PSRAM: %u | Snd: %s | P4: %s\n",
                  (now - tdmStartMs) / 1000,
                  p4Delta, p4Delta / elapsed,
                  infDelta, infDelta / elapsed,
                  heapFree, psramFree,
                  espnowReady ? "OK" : "DOWN",
                  espnowReady ? "OK" : "DOWN");
  }
}

// ==================== 事件钩子 ====================
void tdmRecordP4Send() {
  if (tdmActive) tdmP4SentCount++;
}

void tdmRecordSenderReconnect() {
  if (tdmActive) tdmSenderReconnects++;
}

#endif
