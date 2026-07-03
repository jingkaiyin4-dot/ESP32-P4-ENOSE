# P4 Gateway Cloud Protocol Extension Guide: Fogger, Fan & Lid Controls
> P4 网关云端协议扩展对接指南：雾化加湿器、12V 排风风扇与舱门舵机控制

This document defines the extended HTTP APIs and JSON payload formats for controlling the peripherals: **Fogger (雾化加湿器)**, **Fan (12V排风风扇)**, and **Lid Servo (舱门舵机)**. It is written to align the server-side implementations with the P4 Gateway's firmware logic.

---

## 1. Periodic Telemetry Upload (周期遥测数据上报)
The gateway POSTs sensor readings and device states periodically to the server. The payload now includes the real-time ON/OFF state of the UV lamp, Fogger, Fan, and Lid Servo.

* **Method**: `POST`
* **Path**: `/api/sensor/data?key=bigboss`
* **Headers**: `Content-Type: application/json`
* **Payload Structure**:
```json
{
  "t": 23.5,
  "hu": 45.2,
  "co2": 450,
  "o": 5.2,
  "h": 0.05,
  "c": 12.1,
  "v": 0.8,
  "cls": "air",
  "conf": 1.0,
  "fr": 98,
  "uv_on": false,
  "fog_on": true,
  "fan_on": false,
  "lid_on": false
}
```
### Parameters Table:
| Key | Type | Description |
| :--- | :--- | :--- |
| `uv_on` | Boolean | True if the UV lamp is currently ON. |
| `fog_on` | Boolean | True if the Fogger is currently ON. |
| `fan_on` | Boolean | True if the Fan is currently ON. |
| `lid_on` | Boolean | True if the Lid Servo is currently open (ON). |

---

## 2. Server Command Queue Poll (云端命令轮询)
The gateway polls the server every 5 seconds via `GET` to fetch pending control commands. The server responds with command frames from the queue.

* **Method**: `GET`
* **Path**: `/api/p4/poll?key=bigboss`
* **Response (No command in queue)**:
```json
{
  "id": null
}
```

* **Response (With control commands in queue)**:
Below are the format templates for the actions.

### 2.1 Toggle Fogger (开关雾化加湿器)
* **Action Name**: `set_fog`
* **Params**: `{"on": 0/1}`
```json
{
  "id": "cmd_1780302494480",
  "action": "set_fog",
  "params": {
    "on": 1
  }
}
```

### 2.2 Toggle Fan (开关12V排风扇)
* **Action Name**: `set_fan`
* **Params**: `{"on": 0/1}`
```json
{
  "id": "cmd_1780302494481",
  "action": "set_fan",
  "params": {
    "on": 0
  }
}
```

### 2.3 Toggle Lid Servo (控制舱门舵机开关)
* **Action Name**: `set_lid`
* **Params**: `{"on": 0/1}`
```json
{
  "id": "cmd_1780302494485",
  "action": "set_lid",
  "params": {
    "on": 1
  }
}
```

### 2.4 Set Fogger Duration (设置加湿器定时工作时间)
* **Action Name**: `fog_dur`
* **Params**: `{"duration": N}` (N is in **minutes**)
```json
{
  "id": "cmd_1780302494482",
  "action": "fog_dur",
  "params": {
    "duration": 5
  }
}
```

### 2.5 Set Fan Duration (设置风扇定时工作时间)
* **Action Name**: `fan_dur`
* **Params**: `{"duration": N}` (N is in **minutes**)
```json
{
  "id": "cmd_1780302494483",
  "action": "fan_dur",
  "params": {
    "duration": 10
  }
}
```

### 2.6 Toggle Automatic Triggers (控制智能联动逻辑)
* Automatically trigger Fogger (humidity < 40%) or Fan (freshness <= 50).
* Automatically trigger Lid Servo ventilation (freshness <= 40).
* **Action Names**: 
  - Humidifier: `fog_auto_on`, `fog_auto_off`
  - Fan: `fan_auto_on`, `fan_auto_off`
  - Lid Servo: `lid_auto_on`, `lid_auto_off`
* **Params**: None (empty or omit)
```json
{
  "id": "cmd_1780302494484",
  "action": "lid_auto_on",
  "params": {}
}
```

### 2.7 Query Peripheral Status (查询外设状态)
* Send query packet down to the S3 node.
* **Action Names**: `lid_status`
* **Params**: None
```json
{
  "id": "cmd_1780302494486",
  "action": "lid_status",
  "params": {}
}
```

---

## 3. Command Acknowledgement (指令确认反馈 ACK)
After executing a command, the gateway POSTs the execution result back to the server. The server must mark the corresponding `id` as completed.

* **Method**: `POST`
* **Path**: `/api/p4/ack?key=bigboss`
* **Headers**: `Content-Type: application/json`

### 3.1 Toggle Fogger ACK
```json
{
  "id": "cmd_1780302494480",
  "accept": true,
  "result": {
    "fog_on": true
  },
  "reason": ""
}
```

### 3.2 Toggle Fan ACK
```json
{
  "id": "cmd_1780302494481",
  "accept": true,
  "result": {
    "fan_on": false
  },
  "reason": ""
}
```

### 3.3 Toggle Lid Servo ACK
```json
{
  "id": "cmd_1780302494485",
  "accept": true,
  "result": {
    "lid_on": true
  },
  "reason": ""
}
```

### 3.4 General Timeout / Offline Failure ACK (e.g. if S3 node is unreachable)
```json
{
  "id": "cmd_1780302494480",
  "accept": false,
  "result": {},
  "reason": "S3 Receiver offline / ESP-NOW command transmission timeout"
}
```

---
## Summary of Supported Actions
| action | params | Description |
| :--- | :--- | :--- |
| `set_fog` | `{"on": 0/1}` | Turn ON/OFF the fogger humidifier. |
| `set_fan` | `{"on": 0/1}` | Turn ON/OFF the 12V fan. |
| `set_lid` | `{"on": 0/1}` | Open/Close the lid servo. |
| `fog_dur` | `{"duration": minutes}` | Set the timer for fogger humidifier auto-shutdown. |
| `fan_dur` | `{"duration": minutes}` | Set the timer for 12V fan auto-shutdown. |
| `fog_auto_on` / `fog_auto_off` | None | Toggle automatic humidifier trigger (linked to humidity < 40.0%). |
| `fan_auto_on` / `fan_auto_off` | None | Toggle automatic fan exhaust trigger (linked to freshness <= 50). |
| `lid_auto_on` / `lid_auto_off` | None | Toggle automatic lid ventilation trigger (linked to freshness <= 40). |
| `lid_status` | None | Query current lid status from S3 Receiver. |
