/**
 * @file app_ws_client.h
 * @brief WebSocket 客户端 — 连接 PC 端媒体服务器
 *
 * === 功能 ===
 * - 连接 ws://host:port/path 并保持心跳
 * - 收到 JSON 消息后自动解析并分发到 app_music_screen
 * - 断线自动重连（指数退避，最长 30s）
 * - 提供 app_ws_send_command() 发送播放控制命令
 *
 * === 协议 ===
 * PC → ESP32:  song_info, lyrics, position, server_status
 * ESP32 → PC:  command (play_pause / next / prev)
 *
 * === 使用 ===
 * @code{.c}
 * app_ws_start("192.168.1.100", 8765, "/");
 * // ...
 * app_ws_send_command("play_pause");
 * // ...
 * app_ws_stop();
 * @endcode
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 WebSocket 客户端
 *
 * 创建一个后台任务处理 WebSocket 连接。
 * 自动重连：首次 1s → 每次翻倍 → 最大 30s。
 *
 * @param host 服务器 IP 或主机名
 * @param port 端口号（默认 8765）
 * @param path 路径（默认 "/"）
 * @return ESP_OK 任务已创建
 */
esp_err_t app_ws_start(const char *host, uint16_t port, const char *path);

/**
 * @brief 停止 WebSocket 客户端（断开连接 + 销毁任务）
 */
void app_ws_stop(void);

/**
 * @brief 向 PC 发送播放控制命令
 *
 * JSON 格式：{"type":"command","action":"<action>"}
 *
 * @param action 命令名称: "play_pause", "next", "prev"
 * @return ESP_OK 已加入发送队列；ESP_FAIL 未连接
 */
esp_err_t app_ws_send_command(const char *action);

/**
 * @brief 向 PC 发送带额外字段的命令
 *
 * JSON 格式：{"type":"command","action":"<action>",<extra_json>}
 *
 * @param action     命令名称
 * @param extra_json 额外 JSON 字段（以逗号开头），例如 ","position_ms":12345"
 *                   传 NULL 或 "" 表示无额外字段（等价于 app_ws_send_command）
 * @return ESP_OK 已加入发送队列；ESP_FAIL 未连接
 */
esp_err_t app_ws_send_command_ex(const char *action, const char *extra_json);

/**
 * @brief 发送 seek 跳转命令
 *
 * @param position_ms 跳转目标位置（毫秒）
 * @return ESP_OK 已加入发送队列
 */
esp_err_t app_ws_send_seek(uint32_t position_ms);

/**
 * @brief WebSocket 是否已连接
 *
 * @return true 已连接
 */
bool app_ws_is_connected(void);

/**
 * @brief 设置/清除连接状态回调
 *
 * @param cb 回调函数（arg=连接状态 true=已连）, NULL 清除
 */
void app_ws_set_conn_callback(void (*cb)(bool connected));

#ifdef __cplusplus
}
#endif
