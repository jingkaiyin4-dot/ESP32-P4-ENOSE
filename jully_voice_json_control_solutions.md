# Jully (小智) 语音 AI - 硬件控制 JSON 回复落地与实现方案设计报告

本报告针对“如何让 AI 语音助手在文本/协议层面回复结构化 JSON，并以此实现精准的硬件开关灯控制”的需求，设计并规划了 **3 套在物联网与大模型集成中最主流、最高可靠性的工程落地解决方案**。

---

## 方案一：System Prompt 强约束纯文本 JSON 注入法 (最快落地)

### 1. 底层原理
此方案无需更改任何现有的网络协议栈或服务端接口。直接在大模型（LLM）的 **System Prompt（系统提示词人设）** 中施加绝对的格式铁律，强制模型在感知到硬件操作意图时，**必须以且只能以标准 JSON 结构作为文本内容进行回复**。

### 2. 云端大模型 System Prompt 配置示范
在您的小智大模型后台人设提示词的最尾部，追加如下约束规范：
```markdown
## ⚠️ 绝对控制铁律 (JSON Output Rules)
当且仅当检测到主人要控制紫外线灯（开启、关闭、设置消杀时间、设置新鲜度阈值）时，你必须且只能使用标准的 JSON 格式进行文本回复。严禁包含任何 JSON 之外的问候语、解释句、拼音或 Markdown 标记。

### 回复 JSON 标准格式：
{
  "text": "<你对主人说的话，用极具亲和力的拟人化语言，如：好的主人，Jully 这就为您开启紫外消杀>",
  "cmd": "<控制硬件的指令字符串，只允许输出下表中的一个：uv_on / uv_off>"
}

### 触发示例：
- 主人说：“打开紫外线消毒”
- 你的唯一文本回复为：
{
  "text": "好的主人，Jully 已经为您闭合继电器，紫外线消毒灯已点亮。",
  "cmd": "uv_on"
}
```

### 3. P4 端侧 C 语言极速解析与安全过滤代码
在 P4 网关侧的 [main.cpp](file:///d:/ESP32-P4/esp_brookesia_phone/main/main.cpp) 中，直接对 `xiaozhi_text_callback` 的文本进行检测解析：

```cpp
static void xiaozhi_text_callback(const char* text)
{
    if (!text) return;

    // 1. 判断是否是大模型格式化输出的 JSON 帧
    if (text[0] == '{') {
        cJSON *root = cJSON_Parse(text);
        if (root) {
            cJSON *text_item = cJSON_GetObjectItem(root, "text");
            cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");

            // 提取对主人说的话，送去打字机和 TTS 语音播放
            if (text_item && text_item->valuestring) {
                ESP_LOGI("JULLY", "Jully Speech: %s", text_item->valuestring);
                ui_update_xiaozhi_text(text_item->valuestring);
            }

            // 提取结构化硬件指令，直接派发至外接 C6 
            if (cmd_item && cmd_item->valuestring) {
                char cmd_json[64];
                snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"%s\"}", cmd_item->valuestring);
                ESP_LOGI("JULLY", "Jully Control Triggered: %s", cmd_json);
                uart_receiver_send(cmd_json); // 串口极速发送
            }

            cJSON_Delete(root);
            return; // 拦截成功，退出
        }
    }

    // 2. 兜底：如果不是以 '{' 开头的 JSON，回退至普通文本处理
    ESP_LOGI("JULLY", "Jully Regular Text: %s", text);
    ui_update_xiaozhi_text(text);
}
```

---

## 方案二：端侧 Function Calling / Tool Use 动作下发 (最稳健、工业级)

### 1. 底层原理
本方案是现代 AI Agent 最标准的控制范式。在语音交互网关层（如小智后台、Dify、Coze 等），为大模型注册一个控制物理外设的 **Tool (工具/函数)**。大模型识别到意图后，云端不急于回复文本，而是首先向端侧下发一个标准的 **Tool Call 协议帧**，参数高度数字化，支持复杂时段与模式控制。

### 2. 云端工具 (Tool/Function) 定义
在大模型平台为 Jully 注册的函数描述如下：
- **名称 (Name)**：`control_uv_relay`
- **描述 (Description)**：`用于控制设备上的物理紫外杀菌灯的通断、消杀持续时间或自动判定阈值。`
- **参数架构 (Parameters - JSON Schema)**：
  ```json
  {
    "type": "object",
    "properties": {
      "action": {
        "type": "string",
        "enum": ["on", "off", "auto"],
        "description": "控制动作：on为强制点亮，off为强制熄灭，auto为进入新鲜度联动自动判定"
      },
      "duration": {
        "type": "integer",
        "description": "本次消杀的设定持续时间，单位：秒，非必填"
      }
    },
    "required": ["action"]
  }
  ```

### 3. 网络协议与 P4 底层解析
当大模型决定调用此工具时，WebSocket 链路会向 P4 下发包含 type 为 `"action"` 的 JSON 帧。
在 `xiaozhi_ai_service.c` 的 `handle_server_msg` 中建立工具接收解析：

```cpp
// 在 handle_server_msg 协议接收解析器中增加：
cJSON *type_item = cJSON_GetObjectItem(root, "type");
if (type_item && strcmp(type_item->valuestring, "tool_call") == 0) {
    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (name_item && strcmp(name_item->valuestring, "control_uv_relay") == 0) {
        cJSON *args = cJSON_GetObjectItem(root, "arguments");
        if (args) {
            cJSON *act = cJSON_GetObjectItem(args, "action");
            cJSON *dur = cJSON_GetObjectItem(args, "duration");
            
            if (act && act->valuestring) {
                char cmd_json[128];
                if (dur && cJSON_IsNumber(dur)) {
                    // 带消杀时长的控制：{"cmd":"uv_on", "dur":30}
                    snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"uv_%s\",\"dur\":%d}", act->valuestring, dur->valueint);
                } else {
                    snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"uv_%s\"}", act->valuestring);
                }
                ESP_LOGI("JULLY_TOOL", "Tool Call Dispatched: %s", cmd_json);
                uart_receiver_send(cmd_json); // 派发至外置 C6
            }
        }
    }
}
```

---

## 方案三：回复文本“动作插桩标签 (Action Tag)” 混合提取法 (体验最极致)

### 1. 底层原理
此方案具有最极致的用户体验。它巧妙地规避了“大模型为了输出 JSON 而强迫其无法生成富有人格特质的生动对话”的缺陷。我们让大模型像普通人一样说话，仅在其自然语言文本的**最后**，自动拼接一串硬件控制的“插桩标签”（如 `[ACTION: uv_on]`）。

### 2. 意图插桩示范
- **主人说**：“Jully，把紫外灯打开消毒吧。”
- **大模型正常回复文本**：“好的主人，已为您开启紫外杀菌。单次设定消杀时长为 30 秒，请注意不要用眼睛直视光源哦！[ACTION: uv_on][ACTION: uv_dur_30]”

### 3. P4 端侧 C 语言高效正则过滤与提取器
在 P4 的 `xiaozhi_text_callback` 中建立提取器。**该提取器会将 `[ACTION: ...]` 完美过滤出来发送给串口，并将这些长相丑陋的标签从字符串中彻底剔除干净**，只把最纯净的人类自然语言送去打字机和 TTS 音频播放，防范喇叭念出“方括号 action”的尴尬现象：

```cpp
static void xiaozhi_text_callback(const char* text)
{
    if (!text) return;

    // 拷贝一份用于提取和剔除
    char *clean_text = strdup(text);
    if (!clean_text) return;

    char *pos = clean_text;
    while ((pos = strstr(pos, "[ACTION: "))) {
        char *end = strchr(pos, ']');
        if (end) {
            // 提取动作内容，例如: "uv_on"
            size_t act_len = end - (pos + 9);
            char action[64] = {0};
            if (act_len < sizeof(action)) {
                memcpy(action, pos + 9, act_len);
                
                // 执行串口派发
                char cmd_json[128];
                if (strncmp(action, "uv_dur_", 7) == 0) {
                    snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"uv_dur\",\"val\":%d}", atoi(action + 7));
                } else {
                    snprintf(cmd_json, sizeof(cmd_json), "{\"cmd\":\"%s\"}", action);
                }
                ESP_LOGI("JULLY_TAG", "Extracted Action Tag: %s -> Sending: %s", action, cmd_json);
                uart_receiver_send(cmd_json); // 串口发送给外置 C6
            }
            
            // 将 [ACTION: ...] 标签从 clean_text 中擦除 (填充为空格，防止 TTS 念出来)
            memset(pos, ' ', end - pos + 1);
            pos = end + 1;
        } else {
            break;
        }
    }

    // 更新 UI 文字并播放 TTS 语音 (已洗净动作标签)
    ESP_LOGI("JULLY_TAG", "TTS Clean Text: %s", clean_text);
    ui_update_xiaozhi_text(clean_text);
    
    free(clean_text);
}
```
