/**
 * @file app_network.h
 * @brief WiFi 站模式管理 — 连接/重连/IP 获取
 *
 * === 分层说明 ===
 * 本文件属于应用层，直接调用 ESP-IDF 的 esp_wifi / esp_netif API。
 * WiFi 在 ESP32 上已由 ESP-IDF 框架封装，此处不额外添加 HAL 层。
 *
 * === 使用流程 ===
 * @code{.c}
 * ESP_ERROR_CHECK(app_network_init());
 * ESP_ERROR_CHECK(app_network_connect("MyWiFi", "password"));
 * ESP_ERROR_CHECK(app_network_wait_connected(pdMS_TO_TICKS(10000)));
 * ESP_LOGI(TAG, "IP: %s", app_network_get_ip());
 * @endcode
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi 站模式
 *
 * 内部流程：NVS 初始化 → 创建 netif → 创建默认事件循环 →
 * 注册事件处理器 → 初始化 WiFi 驱动 → 设置为 STA 模式。
 *
 * 幂等安全：多次调用仅首次生效。
 *
 * @return ESP_OK 成功，ESP_FAIL 重复初始化
 */
esp_err_t app_network_init(void);

/**
 * @brief 发起 STA 连接
 *
 * 配置 SSID/Password 并调用 esp_wifi_connect()。
 * 此函数立即返回，不等待连接完成。
 * 后续通过 app_network_wait_connected() 等待。
 *
 * @param ssid     WiFi 名称（最长 32 字节）
 * @param password WiFi 密码（可为 NULL 表示开放网络）
 * @return ESP_OK 连接已发起
 */
esp_err_t app_network_connect(const char *ssid, const char *password);

/**
 * @brief 等待 WiFi 连接并获取 IP
 *
 * 阻塞直到 WiFi 连接成功且 DHCP 分配 IP，或超时。
 *
 * @param timeout_ticks 等待的 FreeRTOS tick 数，portMAX_DELAY 表示无限等待
 * @return ESP_OK 已连接且有 IP；ESP_FAIL 超时
 */
esp_err_t app_network_wait_connected(TickType_t timeout_ticks);

/**
 * @brief 断开 WiFi 并停止自动重连
 */
void app_network_disconnect(void);

/**
 * @brief 获取当前 IP 地址字符串
 *
 * @return 静态缓冲的 IP 字符串，未连接时返回 "0.0.0.0"
 */
const char *app_network_get_ip(void);

/**
 * @brief WiFi 是否已连接且有 IP
 *
 * @return true 已连接
 */
bool app_network_is_connected(void);

/**
 * @brief 强制重新连接（断开后重连）
 */
void app_network_reconnect(void);

#ifdef __cplusplus
}
#endif
