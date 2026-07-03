// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

#ifndef AIOS_H_
#define AIOS_H_

/* include ------------------------------------------------------------------ */
#include "aios_config.h"
#include "volc_list.h"
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* default configuration ---------------------------------------------------- */

#ifndef AIOS_MAX_ACTORS
#define AIOS_MAX_ACTORS                          8       // 默认最多8个Actor
#endif

#ifndef AIOS_USE_ASSERT
#define AIOS_USE_ASSERT                          1       // 默认打开断言
#endif


#include "aios_def.h"

/* data struct -------------------------------------------------------------- */

// 状态返回值的定义
typedef enum aios_ret {
    AIOS_Ret_Null = 0,                       // 无效值
    AIOS_Ret_Handled,                        // 已处理，不产生跳转
    AIOS_Ret_NotHandled,                     // 未处理，不产生跳转
    AIOS_Ret_Tran,                           // 跳转
} aios_ret_t;

// 事件类
struct aios_event;
typedef void (*aios_event_free_handler)(const void *data);
typedef struct aios_event {
    volc_list_head_t node;
    uint32_t sub;                          // 订阅者
    aios_event_id_t id;                      // 事件主题
    void *data;                              // 事件数据
    aios_event_free_handler free_handler;    // 事件数据释放函数
} aios_event_t;

// 数据结构 - 行为树相关 --------------------------------------------------------
// 状态函数句柄的定义
struct aios_session;
typedef aios_ret_t (* aios_state_handler)(struct aios_session *const me, aios_event_t const * const e);

// 状态机类
typedef struct aios_session {
    uint32_t priority              : 5;    // 优先级，0-31，数值越大优先级越高
    uint32_t enabled               : 1;    // 是否使能
    uint32_t reserve               : 1;    // 保留位
    volatile aios_state_handler state;
} aios_session_t;

// api -------------------------------------------------------------------------
// 对框架进行初始化，在各状态机初始化之前调用。
void aios_init(int event_size);
// 启动框架，放在main函数的末尾。
void aios_run(void);
// 停止框架的运行（不常用）
// 停止框架后，框架会在执行完当前状态机的当前事件后，清空各状态机事件队列，清空事件池，
// 不再执行任何功能，直至框架被再次启动。
void aios_stop(void);

// 关于状态机 -----------------------------------------------
// 状态机初始化函数
void aios_session_init(   aios_session_t * const me,
                    uint8_t priority,
                    void const * const parameter);
void aios_session_start(aios_session_t * const me, aios_state_handler state_init);

aios_ret_t aios_tran(aios_session_t * const me, aios_state_handler state);

#define AIOS_TRAN(target)            aios_tran((aios_session_t * )me, (aios_state_handler)target)
#define AIOS_STATE_CAST(state)       ((aios_state_handler)(state))

// 关于事件 -------------------------------------------------
// 事件订阅
void aios_event_sub(aios_session_t * const me, aios_event_id_t id);
// 事件取消订阅
void aios_event_unsub(aios_session_t * const me, aios_event_id_t id);
// 事件订阅宏定义
#define AIOS_EVENT_SUB(_evt)               aios_event_sub(&(me->super), _evt)
// 事件取消订阅宏定义
#define AIOS_EVENT_UNSUB(_evt)             aios_event_unsub(&(me->super), _evt)

// 发布事件（携带数据）
int aios_event_pub(aios_event_id_t id, void *data, aios_event_free_handler free_handler);

// 发布周期性或延时事件
void* aios_event_time_pub(aios_event_id_t id, uint32_t timeout_ms, uint32_t period_ms);

// 取消延时事件或者周期事件的发布
void aios_event_time_cancel(void** timer);

/* port --------------------------------------------------------------------- */
// void* aios_malloc(uint32_t size);
// void aios_free(void* ptr);
// void* aios_mutex_create(void);
// void aios_mutex_lock(void* mutex);
// void aios_mutex_unlock(void* mutex);
// void aios_mutex_destroy(void** mutex);
void aios_port_assert(uint32_t error_id);

/* hook --------------------------------------------------------------------- */

// 结束AIOS Nano的运行的时候，所调用的回调函数。
void aios_hook_stop(void);

// 启动AIOS Nano的时候，所调用的回调函数
void aios_hook_start(void);

#ifdef __cplusplus
}
#endif

#endif
