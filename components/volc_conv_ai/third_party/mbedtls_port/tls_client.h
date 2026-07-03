/*
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Modified: 使用 ESP-IDF esp_tls 组件替代原生 mbedtls 调用
 *  原因: 原生 mbedtls API 与 ESP-IDF v5.5 的动态缓冲区机制冲突，
 *        导致 MBEDTLS_ERR_SSL_INVALID_RECORD 和 Load access fault 崩溃
 */

#ifndef MBEDTLS_CLIENT_H
#define MBEDTLS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* 使用 ESP-IDF 的 esp_tls 组件替代原生 mbedtls
 * esp_tls 正确管理动态缓冲区、bio 回调和硬件加速，
 * 与 ESP-Hosted SDIO 架构兼容 */
#include "esp_tls.h"

typedef struct MbedTLSSession {
  char *host;
  char *port;

  unsigned char *buffer;
  size_t buffer_len;

  /* 使用 esp_tls 句柄替代原生 mbedtls 上下文 */
  esp_tls_t *tls;

  /* 底层 socket fd，供 websocket.c 的 select()/poll 使用 */
  int sockfd;

  /* esp_tls 配置，在 context 阶段设置 */
  esp_tls_cfg_t tls_cfg;
} MbedTLSSession;

extern int mbedtls_client_init(MbedTLSSession *session, void *entropy, size_t entropyLen);
extern int mbedtls_client_close(MbedTLSSession *session);
extern int mbedtls_client_context(MbedTLSSession *session);
extern int mbedtls_client_connect(MbedTLSSession *session);
extern int mbedtls_client_read(MbedTLSSession *session, unsigned char *buf, size_t len);
extern int mbedtls_client_write(MbedTLSSession *session, const unsigned char *buf, size_t len);

#endif
