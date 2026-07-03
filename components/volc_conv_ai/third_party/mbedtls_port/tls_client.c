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
 *  Modified: 使用 ESP-IDF esp_tls 组件重写 TLS 客户端
 *  原因: 原生 mbedtls_net_connect/mbedtls_net_send/mbedtls_net_recv 在 ESP-IDF v5.5
 *        中被重定向到 esp_mbedtls_* 系列函数，与 ESP-Hosted SDIO 架构下的动态缓冲区
 *        机制冲突，导致:
 *        1. MBEDTLS_ERR_SSL_INVALID_RECORD (-29184) 错误
 *        2. mbedtls_net_send → esp_mbedtls_add_rx_buffer 处 Core 1 崩溃
 *
 *  解决方案: 使用 esp_tls 组件，它正确管理:
 *        - 动态 TLS 缓冲区分配
 *        - bio 回调（包括带超时的 recv_timeout）
 *        - 硬件加速（AES、SHA 等）
 *        - 与 ESP-Hosted 的兼容性（经过 ESP-IDF 官方测试）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "tls_certificate.h"
#include "tls_client.h"

#include "volc_osal.h"
#include "util/volc_list.h"
#include "util/volc_log.h"

#define TAG "tls_client"

/* TLS 连接超时（毫秒），ESP-Hosted SDIO 架构需要更长超时 */
#define TLS_CONNECT_TIMEOUT_MS  (15000)
/* TLS 读取超时（毫秒） */
#define TLS_READ_TIMEOUT_MS     (5000)

int mbedtls_client_init(MbedTLSSession *session, void *entropy, size_t entropyLen)
{
  if (session == NULL) {
    LOGE("session is NULL");
    return -1;
  }

  /* esp_tls 在 connect 阶段自动处理熵源和 DRBG 初始化，无需手动配置 */
  session->tls = NULL;
  session->sockfd = -1;
  memset(&session->tls_cfg, 0, sizeof(esp_tls_cfg_t));

  LOGD("mbedtls client (esp_tls mode) init success...");
  return 0;
}

int mbedtls_client_close(MbedTLSSession *session)
{
  if (session == NULL) {
    return -1;
  }

  /* 使用 esp_tls_conn_destroy 正确释放所有内部资源
   * 包括 SSL 上下文、socket、缓冲区等 */
  if (session->tls != NULL) {
    esp_tls_conn_destroy(session->tls);
    session->tls = NULL;
  }
  session->sockfd = -1;

  HAL_SAFE_FREE(session->buffer);
  HAL_SAFE_FREE(session->host);
  HAL_SAFE_FREE(session->port);
  HAL_SAFE_FREE(session);

  LOGD("mbedtls client (esp_tls mode) closed");
  return 0;
}

int mbedtls_client_context(MbedTLSSession *session)
{
  if (session == NULL) {
    LOGE("session is NULL");
    return -1;
  }

  /* 配置 esp_tls
   * 使用内嵌的 DigiCert Global Root G2 证书进行服务器证书验证 */
  memset(&session->tls_cfg, 0, sizeof(esp_tls_cfg_t));

  session->tls_cfg.cacert_buf = (const unsigned char *)GLOBAL_ROOT_CERT;
  session->tls_cfg.cacert_bytes = GLOBAL_ROOT_CERT_LEN;
  session->tls_cfg.timeout_ms = TLS_CONNECT_TIMEOUT_MS;
  session->tls_cfg.non_block = false;

  /* 如果后续需要跳过证书验证（仅调试用），可以：
   * session->tls_cfg.skip_common_name = true;
   * 但生产环境强烈不推荐 */

  LOGD("mbedtls client context (esp_tls mode) init success...");
  return 0;
}

int mbedtls_client_connect(MbedTLSSession *session)
{
  if (session == NULL || session->host == NULL || session->port == NULL) {
    LOGE("invalid session/host/port");
    return -1;
  }

  int port = atoi(session->port);
  if (port <= 0) {
    LOGE("invalid port: %s", session->port);
    return -1;
  }

  LOGI("Connecting to %s:%d via esp_tls...", session->host, port);

  /* esp_tls_conn_new_sync 完成以下所有步骤:
   * 1. DNS 解析
   * 2. TCP 连接（带超时）
   * 3. TLS 握手（使用提供的 CA 证书）
   * 4. 证书验证（包括 CN/SAN 检查）
   * 5. 正确配置 bio 回调（包括带超时的 recv_timeout）
   * 6. 正确配置动态缓冲区
   *
   * 这些步骤由 ESP-IDF 内部管理，与 ESP-Hosted SDIO 传输完全兼容 */
  /* 初始化 esp_tls 句柄 */
  esp_tls_t *tls = esp_tls_init();
  if (tls == NULL) {
    LOGE("esp_tls_init failed");
    return -1;
  }

  /* esp_tls_conn_new_sync 在 ESP-IDF v5.x 中有 5 个参数:
   * 返回值: 1 成功, -1 失败, 0 正在进行
   * 最后一个参数是已初始化的 esp_tls_t 句柄 */
  int ret = esp_tls_conn_new_sync(
      session->host,
      (int)strlen(session->host),
      port,
      &session->tls_cfg,
      tls);

  if (ret != 1) {
    LOGE("esp_tls_conn_new_sync failed: ret=%d host=%s port=%d", ret, session->host, port);
    esp_tls_conn_destroy(tls);
    return -1;
  }
  session->tls = tls;

  /* 获取底层 socket fd，供 websocket.c 的 select()/poll 使用 */
  int sockfd = -1;
  esp_err_t err = esp_tls_get_conn_sockfd(session->tls, &sockfd);
  if (err != ESP_OK || sockfd < 0) {
    LOGE("Failed to get socket fd from esp_tls: %d", err);
    esp_tls_conn_destroy(session->tls);
    session->tls = NULL;
    return -1;
  }
  session->sockfd = sockfd;

  LOGI("Connected to %s:%d via esp_tls, sockfd=%d", session->host, port, sockfd);
  return 0;
}

int mbedtls_client_read(MbedTLSSession *session, unsigned char *buf, size_t len)
{
  int ret = 0;

  if (!session || !session->tls || !buf || (len == 0)) {
    LOGW("invalid arguments: session(%p), buf(%p), len(%d)",
        session, buf, (int)len);
    return -1;
  }

  /* esp_tls_conn_read 内部处理:
   * - WANT_READ / WANT_WRITE 重试
   * - 超时管理
   * - 动态缓冲区管理
   * 但我们仍然添加重试循环以处理非阻塞情况 */
  do {
    ret = esp_tls_conn_read(session->tls, (char *)buf, len);
  } while (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE);

  if (ret < 0) {
    /* 将 esp_tls 错误码转换为日志输出 */
    if (ret == ESP_TLS_ERR_SSL_TIMEOUT) {
      LOGD("esp_tls read timeout");
    } else {
      LOGE("esp_tls_conn_read failed: %d (0x%x)", ret, -ret);
    }
  } else if (ret == 0) {
    LOGE("esp_tls connection closed by peer");
    ret = -1;
  }

  return ret;
}

int mbedtls_client_write(MbedTLSSession *session, const unsigned char *buf, size_t len)
{
  int ret = 0;
  size_t written = 0;

  if (session == NULL || session->tls == NULL || buf == NULL) {
    return -1;
  }

  /* esp_tls_conn_write 可能部分写入，需要循环确保全部发送 */
  while (written < len) {
    ret = esp_tls_conn_write(session->tls, (const char *)buf + written, len - written);
    if (ret > 0) {
      written += ret;
    } else if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
      /* 非阻塞模式下需要重试 */
      continue;
    } else {
      LOGE("esp_tls_conn_write failed: %d (0x%x)", ret, -ret);
      return ret;
    }
  }

  return (int)written;
}
