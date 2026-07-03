// Copyright (2025) Beijing Volcano Engine Technology Ltd.
// SPDX-License-Identifier: Apache-2.0

// include ---------------------------------------------------------------------
#include "../inc/aios.h"
#include <string.h>
#include <stdbool.h>
#include "heap-inl.h"

//socket
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
// time
#include <sys/time.h>
#include "volc_osal.h"
#ifdef __cplusplus
extern "C" {
#endif

/* assert ------------------------------------------------------------------- */
#if (AIOS_USE_ASSERT != 0)
#define AIOS_ASSERT(test_) do { if (!(test_)) {                                 \
        aios_hook_stop();                                                       \
        aios_port_assert(__LINE__);                                             \
    } } while (0)
#else
#define AIOS_ASSERT(test_)               ((void)0)
#endif

// aios define ------------------------------------------------------------------

// **aios** ---------------------------------------------------------------------
enum {
    EosRun_OK                               = 0,
    EosRun_NotEnabled,
    EosRun_NoEvent,
    EosRun_NoActor,
    EosRun_NoActorSub,

    // Timer
    EosTimer_Empty,
    EosTimer_NotTimeout,
    EosTimer_ChangeToEmpty,

    EosRunErr_NotInitEnd                    = -1,
    EosRunErr_ActorNotSub                   = -2,
    EosRunErr_MallocFail                    = -3,
    EosRunErr_SubTableNull                  = -4,
    EosRunErr_InvalidEventData              = -5,
    EosRunErr_HeapMemoryNotEnough           = -6,
    EosRunErr_TimerRepeated                 = -7,
};

typedef struct timer_item {
    struct heap_node node;
    aios_event_id_t id;
    int64_t timeout_ms;
    uint32_t period_ms;
} aios_timer_t;

typedef struct aios_tag {
    uint32_t *sub_table;                                     // event sub table

    uint32_t session_exist;
    uint32_t session_enabled;
    aios_session_t * sessions[AIOS_MAX_ACTORS];

    uint32_t sub_general;
    void* event_mutex;
    volc_list_head_t event_list;

    uint32_t time;
    void* timer_mutex;
    struct heap timer_heap;
    int fd[2];

    uint8_t enabled                        : 1;
    uint8_t running                        : 1;
    uint8_t init_end                       : 1;
    uint8_t reserved                       : 5;
} aios_t;

static aios_t aios;

// data ------------------------------------------------------------------------
static const aios_event_t aios_event_table[Event_User] = {
    {{ 0 }, 0, Event_Null, 0, NULL},
    {{ 0 }, 0, Event_Enter, 0, NULL},
    {{ 0 }, 0, Event_Exit, 0, NULL},
};

// macro -----------------------------------------------------------------------
#define HSM_TRIG_(state_, topic_)                                              \
    ((*(state_))(me, &aios_event_table[topic_]))

// static function -------------------------------------------------------------
static void aios_session_dispath(aios_session_t * const me, aios_event_t const * const e);

#ifndef VOLC_OFFSET
#define VOLC_OFFSET(type, member) ((size_t) &((type *)0)->member)
#endif

#ifndef VOLC_CONTAINER_OF
#define VOLC_CONTAINER_OF(ptr, type, member)                                     \
    ((type *)((char *)(ptr) - VOLC_OFFSET(type, member)))
#endif

static int __heap_node_less(const struct heap_node* a, const struct heap_node* b) {
    aios_timer_t* item_a = VOLC_CONTAINER_OF(a, aios_timer_t, node);
    aios_timer_t* item_b = VOLC_CONTAINER_OF(b, aios_timer_t, node);
    if (item_a->timeout_ms < item_b->timeout_ms) {
        return 1;
    }
    if (item_a->timeout_ms > item_b->timeout_ms) {
        return 0;
    }

    return item_a->id > item_b->id;
}

static int __set_block_mode(int sockfd, int block) {
  int flags = fcntl(sockfd, F_GETFL);
  if (flags < 0) {
    return -1;
  }
  if (block) {
    if ((flags & O_NONBLOCK) != 0) {
      flags ^= O_NONBLOCK;
    }
  } else {
    flags |= O_NONBLOCK;
  }
  if (fcntl(sockfd, F_SETFL, flags) < 0) {
    return -1;
  }
  return 0;
}

static int __pipe(int fds[2]) {
    int ret = 0;
    fds[0] = -1;
    fds[1] = -1;
    int err = 0,opt = 0,write_fd = -1,read_fd = -1;
    read_fd =  socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (read_fd < 0) {
        ret = -1;
        goto err_out_label;
    }

    struct sockaddr_in servaddr;
    uint32_t addr_len = sizeof(servaddr);
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    servaddr.sin_port = 0;

    if (bind(read_fd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        ret = -1;
        goto err_out_label;
    }

    // get port bind by system
    if (getsockname(read_fd, (struct sockaddr*)&servaddr, &addr_len) < 0) {
        ret = -1;
        goto err_out_label;
    }

    if(servaddr.sin_addr.s_addr == htonl(INADDR_ANY))
        servaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    write_fd = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    int conn_ret = connect(write_fd, (struct sockaddr*)&servaddr, addr_len);

    __set_block_mode(write_fd, 0);
    __set_block_mode(read_fd, 0);

    fds[0] = read_fd;
    fds[1] = write_fd;
err_out_label:  
    if(ret < 0) {
        if (read_fd > 0) {
            close(read_fd);
        }
        if (write_fd > 0) {
            close(write_fd);
        }
    }
    return ret;
}

static int __interruptable_init(void) {
    if (__pipe(aios.fd) < 0) {
        return -1;
    }
    return 0;
}

static int __interruptable_deinit(void) {
    close(aios.fd[0]);
    close(aios.fd[1]);
    return 0;
}

static int __interruptable_sleep(int64_t sleep_ms) {
    int ret = 0;
    char buf[64] = { 0 };
    fd_set readfds = { 0 };
    struct timeval tv = { 0 };
    if (sleep_ms < 0) {
        sleep_ms = AIOS_MAX_SLEEP_MS;
    }
    tv.tv_sec = sleep_ms / 1000;
    tv.tv_usec = (sleep_ms % 1000) * 1000;
    FD_ZERO(&readfds);
    FD_SET(aios.fd[0], &readfds);
    ret = select(aios.fd[0] + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
        return -1;
    }
    if (FD_ISSET(aios.fd[0], &readfds)) {
        if ((ret = read(aios.fd[0], buf, sizeof(buf))) < 0) {
            return -1;
        }
    }
    return 0;
}

static int __interruptable_break(void) {
    char c[1] = { 0x5A};
    return (write(aios.fd[1], c, sizeof(c)) ? 0 : -1);
}

static int64_t aios_get_time(void)
{
    struct timeval time_crt;
    gettimeofday(&time_crt, (void *)0);
    int64_t time_crt_ms = time_crt.tv_sec * 1000 + time_crt.tv_usec / 1000;

    return time_crt_ms;
}

void aios_init(int event_size)
{
    aios.enabled = true;
    aios.running = false;
    aios.session_exist = 0;
    aios.session_enabled = 0;
    aios.sub_table = NULL;

    heap_init(&aios.timer_heap);
    if (__interruptable_init() < 0) {
        printf("aios_init: __interruptable_init fail\n");
        return;
    }

    aios.sub_general = 0;
    volc_list_init(&aios.event_list);
    aios.event_mutex = volc_osal_mutex_create();
    if (NULL == aios.event_mutex) {
        printf("aios_init: event_mutex create fail\n");
        return;
    }
    // printf("event: %p\n", aios.event_mutex);

    aios.init_end = 1;
    aios.time = 0;
    aios.timer_mutex = volc_osal_mutex_create();
    if (NULL == aios.timer_mutex) {
        printf("aios_init: timer_mutex create fail\n");
        return;
    }
    // printf("timer: %p\n", aios.timer_mutex);

    aios.sub_table = (uint32_t *)volc_osal_malloc(sizeof(uint32_t) * event_size);
    if (NULL == aios.sub_table) {
        printf("aios_init: sub_table create fail\n");
        return;
    }
}

void aios_deinit(void)
{
    volc_osal_mutex_destroy(&aios.event_mutex);
    volc_osal_mutex_destroy(&aios.timer_mutex);
    __interruptable_deinit();
}

void aios_sub_init(uint32_t *flag_sub, aios_event_id_t topic_max)
{
    aios.sub_table = flag_sub;
    for (int i = 0; i < topic_max; i ++) {
        aios.sub_table[i] = 0;
    }
}

int64_t aios_evttimer(void)
{
    int64_t system_time = aios_get_time();
    volc_osal_mutex_lock(aios.timer_mutex);
    if (aios.timer_heap.nelts == 0) {
        volc_osal_mutex_unlock(aios.timer_mutex);
        return -1;
    }
    aios_timer_t* item = VOLC_CONTAINER_OF(heap_min(&aios.timer_heap), aios_timer_t, node);
    if (item->timeout_ms > system_time) {
        volc_osal_mutex_unlock(aios.timer_mutex);
        return item->timeout_ms - system_time;
    }
    while (item->timeout_ms <= system_time) {
        heap_remove(&aios.timer_heap, &item->node, __heap_node_less);
        aios_event_pub(item->id, NULL, NULL);
        if (item->period_ms == 0) {
            continue;
        }
        item->timeout_ms += item->period_ms;
        heap_insert(&aios.timer_heap, &item->node, __heap_node_less);
        item = VOLC_CONTAINER_OF(heap_min(&aios.timer_heap), aios_timer_t, node);
    }
    volc_osal_mutex_unlock(aios.timer_mutex);
    return 0;
}

void *aios_event_get_block(uint8_t priority);
void aios_event_gc(void *data);

int aios_once(void)
{
    int64_t timeout_ms = -1;
    if (aios.init_end == 0) {
        return (int)EosRunErr_NotInitEnd;
    }

    if (aios.sub_table == NULL) {
        return (int)EosRunErr_SubTableNull;
    }

    if (aios.enabled == false) {
        return (int)EosRun_NotEnabled;
    }

    // 检查是否有状态机的注册
    if (aios.session_exist == 0 || aios.session_enabled == 0) {
        return (int)EosRun_NoActor;
    }

    timeout_ms = aios_evttimer();

    if (volc_list_empty(&aios.event_list)) {
        __interruptable_sleep(timeout_ms);
        return (int)EosRun_NoEvent;
    }

    // 寻找到优先级最高，且有事件需要处理的Actor
    aios_session_t *session = NULL;
    uint8_t priority = AIOS_MAX_ACTORS;
    for (int i = (int)(AIOS_MAX_ACTORS - 1); i >= 0; i --) {
        if ((aios.session_exist & (1 << i)) == 0)
            continue;
        if ((aios.sub_general & (1 << i)) == 0)
            continue;
        session = aios.sessions[i];
        priority = i;
        break;
    }
    // 如果没有找到，返回
    if (priority == AIOS_MAX_ACTORS) {
        __interruptable_sleep(AIOS_MAX_SLEEP_MS);
        return (int)EosRun_NoActorSub;
    }

    // 寻找当前Actor的最老的事件
    volc_osal_mutex_lock(aios.event_mutex);
    aios_event_t * e = aios_event_get_block(priority);
    AIOS_ASSERT(e != NULL);
    
    volc_osal_mutex_unlock(aios.event_mutex);

    // 对事件进行执行
    if ((aios.sub_table[e->id] & (1 << session->priority)) != 0)
    {
            // 执行状态的转换
            aios_session_dispath(session, e);
    }
    else {
        __interruptable_sleep(AIOS_MAX_SLEEP_MS);
        return (int)EosRunErr_ActorNotSub;
    }
    // 销毁过期事件与其携带的参数
    volc_osal_mutex_lock(aios.event_mutex);
    aios_event_gc(e);
    volc_osal_mutex_unlock(aios.event_mutex);

    volc_osal_mutex_lock(aios.timer_mutex);
    if (aios.timer_heap.nelts == 0) {
        volc_osal_mutex_unlock(aios.timer_mutex);
        // __interruptable_sleep(AIOS_MAX_SLEEP_MS);
    } else {
        aios_timer_t* item = VOLC_CONTAINER_OF(heap_min(&aios.timer_heap), aios_timer_t, node);
        volc_osal_mutex_unlock(aios.timer_mutex);
        int sleep_ms = item->timeout_ms - aios_get_time();
        __interruptable_sleep(sleep_ms);
    }
    return (int)EosRun_OK;
}

void aios_run(void)
{
    aios_hook_start();

    AIOS_ASSERT(aios.enabled == true);
    AIOS_ASSERT(aios.sub_table != 0);

    aios.running = true;

    while (aios.enabled) {
        int ret = aios_once();
        AIOS_ASSERT(ret >= 0);

        if (ret == EosRun_NotEnabled) {
            break;
        }
    }
}
void aios_hook_start(void)
{
}

void aios_hook_stop(void)
{
}

void aios_port_assert(uint32_t error_id)
{
    printf("------------------------------------\n");
    printf("ASSERT >>> Module: EventOS Nano, ErrorId: %d.\n", (int)error_id);
    printf("------------------------------------\n");

    while (1) {
        usleep(100000);
    }
}


void aios_stop(void)
{
    aios.enabled = false;
    // TODO: release resource
    aios_hook_stop();
}

// state machine ---------------------------------------------------------------
void aios_session_init(   aios_session_t * const me,
                    uint8_t priority,
                    void const * const parameter)
{
    // 框架需要先启动起来
    AIOS_ASSERT(aios.enabled == true);
    AIOS_ASSERT(aios.running == false);
    AIOS_ASSERT(aios.sub_table != NULL);
    // 参数检查
    AIOS_ASSERT(me != (aios_session_t *)0);
    AIOS_ASSERT(priority < AIOS_MAX_ACTORS);

    // 防止二次启动
    if (me->enabled == true)
        return;

    // 检查优先级的重复注册
    AIOS_ASSERT((aios.session_exist & (1 << priority)) == 0);

    // 注册到框架里
    aios.session_exist |= (1 << priority);
    aios.sessions[priority] = me;
    // 状态机   
    me->priority = priority;
    me->state = NULL;
}

void aios_session_start(aios_session_t * const me, aios_state_handler state_init)
{
    aios_state_handler t;

    me->state = state_init;
    me->enabled = true;
    aios.session_enabled |= (1 << me->priority);

    // 进入初始状态，执行TRAN动作。这也意味着，进入初始状态，必须无条件执行Tran动作。
    t = me->state;
    aios_ret_t ret = t(me, &aios_event_table[Event_Null]);
    AIOS_ASSERT(ret == AIOS_Ret_Tran);
    ret = me->state(me, &aios_event_table[Event_Enter]);
    AIOS_ASSERT(ret != AIOS_Ret_Tran);
}

// event -----------------------------------------------------------------------
int aios_event_pub(aios_event_id_t id, void *data, aios_event_free_handler free_handler)
{
    if (aios.init_end == 0) {
        return EosRunErr_NotInitEnd;
    }

    if (aios.sub_table == NULL) {
        return EosRunErr_SubTableNull;
    }

    // 保证框架已经运行
    if (aios.enabled == 0) {
        return EosRun_NotEnabled;
    }

    if (aios.session_exist == 0) {
        return EosRun_NoActor;
    }

    // 没有状态机使能，返回
    if (aios.session_enabled == 0) {
        return EosRun_NotEnabled;
    }
    // 没有状态机订阅，返回
    if (aios.sub_table[id] == 0) {
        return EosRun_NoActorSub;
    }

    // 申请事件空间
    aios_event_t *e = (aios_event_t *)volc_osal_malloc(sizeof(aios_event_t));
    if (e == NULL) {
        return EosRunErr_MallocFail;
    }
    e->id = id;
    e->sub = aios.sub_table[e->id];
    aios.sub_general |= e->sub;
    e->data = data;
    e->free_handler = free_handler;
    volc_osal_mutex_lock(aios.event_mutex);
    volc_list_add_tail(&e->node, &aios.event_list);
    volc_osal_mutex_unlock(aios.event_mutex);
    __interruptable_break();

    return EosRun_OK;
}

void aios_event_sub(aios_session_t * const me, aios_event_id_t id)
{
    aios.sub_table[id] |= (1 << me->priority);
}

void aios_event_unsub(aios_session_t * const me, aios_event_id_t id)
{
    aios.sub_table[id] &= ~(1 << me->priority);
}

void* aios_event_time_pub(aios_event_id_t id, uint32_t time_ms, uint32_t period_ms)
{
    aios_timer_t* item = (aios_timer_t *)volc_osal_malloc(sizeof(aios_timer_t));
    AIOS_ASSERT(item != NULL);
    memset(item, 0, sizeof(aios_timer_t));
    item->id = id;
    item->timeout_ms =  aios_get_time() + time_ms;
    item->period_ms = period_ms;
    volc_osal_mutex_lock(aios.timer_mutex);
    heap_insert(&aios.timer_heap, &item->node, __heap_node_less);
    volc_osal_mutex_unlock(aios.timer_mutex);
    return (void *)item;
}

void aios_event_time_cancel(void** timer)
{
    if (timer == NULL || NULL == *timer) {
        return;
    }
    aios_timer_t* item = (aios_timer_t *)*timer;
    volc_osal_mutex_lock(aios.timer_mutex);
    heap_remove(&aios.timer_heap, &item->node, __heap_node_less);
    volc_osal_mutex_unlock(aios.timer_mutex);
    volc_osal_free(item);
    *timer = NULL;
}

// state tran ------------------------------------------------------------------
aios_ret_t aios_tran(aios_session_t * const me, aios_state_handler state)
{
    me->state = state;

    return AIOS_Ret_Tran;
}

// static function -------------------------------------------------------------
static void aios_session_dispath(aios_session_t * const me, aios_event_t const * const e)
{
    aios_ret_t r;

    AIOS_ASSERT(e != (aios_event_t *)0);

    aios_state_handler s = me->state;
    aios_state_handler t;
    
    r = s(me, e);
    if (r == AIOS_Ret_Tran) {
        t = me->state;
        r = s(me, &aios_event_table[Event_Exit]);
        AIOS_ASSERT(r == AIOS_Ret_Handled || r == AIOS_Ret_NotHandled);
        r = t(me, &aios_event_table[Event_Enter]);
        AIOS_ASSERT(r == AIOS_Ret_Handled || r == AIOS_Ret_NotHandled);
        me->state = t;
    }
    else {
        me->state = s;
    }
}

/* heap library ------------------------------------------------------------- */
void aios_event_gc(void *data)
{
    aios_event_t * e = (aios_event_t *)data;
    if (e->sub == 0) {
        volc_list_del(&e->node);
        if (e->free_handler != NULL) {
            e->free_handler(e->data);
        }
        volc_osal_free(e);
    }
    aios.sub_general = 0;
    volc_list_for_each_entry(e, &aios.event_list, aios_event_t, node) {
        aios.sub_general |= e->sub;
    }
}

void *aios_event_get_block(uint8_t priority)
{
    aios_event_t * e = NULL;
    volc_list_for_each_entry(e, &aios.event_list, aios_event_t, node) {
        if ((e->sub & (1 << priority)) == 0) {
            continue;
        }
        e->sub &=~ (1 << priority);
        break;
    }

    return (void *)e;
}

#ifdef __cplusplus
}
#endif
