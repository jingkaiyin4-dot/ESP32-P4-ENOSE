/**
 * model_runner.h — TFLite 推理、新鲜度评分
 * v5: 16维特征 (Base5 + Ratio6 + Norm5)，移除 airBaseline/Delta
 */
#ifndef MODEL_RUNNER_H
#define MODEL_RUNNER_H

#include "config.h"

// ==================== TFLite 全局 ====================
tflite::MicroErrorReporter           micro_error_reporter;
tflite::ops::micro::AllOpsResolver   resolver;
const Model*      tflite_model  = nullptr;
MicroInterpreter* interpreter   = nullptr;
alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];
uint8_t*          model_buffer  = nullptr;
bool              model_loaded  = false;
float             last_confidence = 0.0f;
int               last_predicted  = 0;

// ==================== 16维特征预处理 (Base + Ratio + Norm) ====================
// Base (5维): 原始传感器值
// Ratio (6维): 跨传感器比值 O/H, O/C, O/V, H/C, H/V, C/V → 浓度无关指纹
// Norm (5维): 样本内归一化 x/Σx → 消除浓度/数量差异
// 与 train_gui.py v5.0 完全对齐

void preprocessInput(const SensorData& data, float input[16]) {
  float odor = data.odor_ppm;
  float hcho = data.hcho_ppm;
  float co   = data.co_ppm;
  float voc  = data.voc_ppm;
  float co2  = (float)data.co2_ppm;
  const float eps = 1e-6f;

  // ---- Base (5维) ----
  input[0] = odor;
  input[1] = hcho;
  input[2] = co;
  input[3] = voc;
  input[4] = co2;

  // ---- Ratio (6维): 跨传感器比值 ----
  input[5]  = odor / max(hcho, eps);   // Ratio_O_H
  input[6]  = odor / max(co,   eps);   // Ratio_O_C
  input[7]  = odor / max(voc,  eps);   // Ratio_O_V
  input[8]  = hcho / max(co,   eps);   // Ratio_H_C
  input[9]  = hcho / max(voc,  eps);   // Ratio_H_V
  input[10] = co   / max(voc,  eps);   // Ratio_C_V

  // ---- Norm (5维): 样本内归一化 ----
  float total = odor + hcho + co + max(voc, eps) + max(co2, 1.0f);
  input[11] = odor / total;  // Norm_Odor
  input[12] = hcho / total;  // Norm_HCHO
  input[13] = co   / total;  // Norm_CO
  input[14] = voc  / total;  // Norm_VOC
  input[15] = co2  / total;  // Norm_CO2

  // MinMax 归一化 (16维)
  if (normParams.loaded) {
    for (int i = 0; i < 16; i++) {
      float range = normParams.max[i] - normParams.min[i];
      if (range > 0.0f) {
        input[i] = (input[i] - normParams.min[i]) / range;
        if (input[i] < 0.0f) input[i] = 0.0f;
        if (input[i] > 1.0f) input[i] = 1.0f;
      }
    }
  }
}

// ==================== 运行推理 ====================
int runInference(const SensorData& data) {
  if (!interpreter || !model_loaded) {
    last_confidence = 0.0f;
    last_predicted  = 0;
    return 0;
  }

  float input[16];
  preprocessInput(data, input);

  float* input_tensor = interpreter->input(0)->data.f;
  int input_size = interpreter->input(0)->dims->data[interpreter->input(0)->dims->size - 1];
  for (int i = 0; i < input_size && i < 16; i++) {
    input_tensor[i] = input[i];
  }

  TfLiteStatus invoke_status = interpreter->Invoke();
  if (invoke_status != kTfLiteOk) {
    Serial.println("Invoke failed");
    return 0;
  }

  float* output_tensor = interpreter->output(0)->data.f;

  TfLiteTensor* output     = interpreter->output(0);
  int           output_dims = output->dims->size;
  int           num_classes = output->dims->data[output_dims - 1];

  int   predictedClass = 0;
  float maxProb        = 0.0f;

  if (num_classes == 1) {
    // Binary classification: single logit output → apply sigmoid
    float logit = output_tensor[0];
    float prob  = 1.0f / (1.0f + expf(-logit));  // sigmoid
    if (prob > 0.5f) {
      predictedClass = 1;
      maxProb = prob;
    } else {
      predictedClass = 0;
      maxProb = 1.0f - prob;
    }
  } else {
    // Multi-class: argmax
    for (int i = 0; i < num_classes; i++) {
      if (output_tensor[i] > maxProb) {
        maxProb = output_tensor[i];
        predictedClass = i;
      }
    }
  }

  last_confidence = maxProb;
  last_predicted  = predictedClass;
  return predictedClass;
}

// ==================== 获取置信度 ====================
float getConfidence() {
  return last_confidence;
}

// ==================== 从类别名称解析食物种类（动态） ====================
// 支持的食材关键词表，可扩展
struct FoodKeyword { const char* keyword; const char* displayName; };
static const FoodKeyword foodKeywords[] = {
  {"air",       "Air"},
  {"apple",     "Apple"},
  {"banana",    "Banana"},
  {"orange",    "Orange"},
  {"chocolate", "Chocolate"},
  {"tomato",    "Tomato"},
  {"mango",     "Mango"},
  {"grape",     "Grape"},
  {"bread",     "Bread"},
  {"milk",      "Milk"},
  {"meat",      "Meat"},
  {"fish",      "Fish"},
};
static const int numFoodKeywords = sizeof(foodKeywords) / sizeof(foodKeywords[0]);

const char* getFoodDisplayName(const char* className) {
  for (int i = 0; i < numFoodKeywords; i++) {
    if (strstr(className, foodKeywords[i].keyword)) {
      return foodKeywords[i].displayName;
    }
  }
  // 不在关键词表里，直接用类名
  return className;
}

bool isAirClass(const char* className) {
  return (strstr(className, "air") != nullptr);
}

// ==================== 从类别名称解析新鲜度状态 ====================
int getFreshnessState(const char* className) {
  if (strstr(className, "fresh"))  return 0;
  if (strstr(className, "stale"))  return 1;
  if (strstr(className, "rotten")) return 2;
  // 类名里没有 fresh/stale/rotten 关键词（如 "air"），默认 fresh
  return 0;
}

// ==================== 计算新鲜度分数 ====================
int calculateFreshnessScore(float confidence, int predictedClass) {
  if (predictedClass < 0 || predictedClass >= numClasses) return 50;

  const char* className = classNames[predictedClass];

  if (isAirClass(className)) {
    return 50;
  }

  int state = getFreshnessState(className);

  switch (state) {
    case 0:
      return (int)(confidence * 100);
    case 2:
      return 100 - (int)(confidence * 100);
    case 1:
    default:
      if (confidence > 0.5f) {
        return 50 - (int)((confidence - 0.5f) * 40);
      } else {
        return 50 + (int)((0.5f - confidence) * 40);
      }
  }
}

#endif
