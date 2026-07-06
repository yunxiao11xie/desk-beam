/**
 * @file hal_led.h
 * @brief WS2812 RGB LED 硬件抽象层
 *
 * 封装 LED 状态（on/off、模式、亮度）为内部 static 变量，
 * 对外仅暴露行为接口。调用方不需要知道 LED 的内部状态存储方式。
 *
 * === 分层说明 ===
 * 本文件属于 HAL 层，位于：
 *   components/hal_led/include/hal_led/hal_led.h
 *
 * === 功能 ===
 * - 单颗 WS2812 RGB LED，通过 RMT 驱动
 * - 支持三种模式：红(0)、绿(1)、蓝(2)
 * - 亮度 0~255 可调
 * - 多任务安全（RMT 操作受互斥锁保护）
 *
 * === 调用者 ===
 * - main.c（初始化）
 * - app_logic.c（业务调度）
 * - app_ui.c（GIF→LED 颜色同步）
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WS2812 RGB LED（RMT 驱动）
 *
 * 创建 RMT TX 通道、安装 LED 编码器、启用通道。
 * 初始化后 LED 为关闭状态。
 *
 * @return ESP_OK 成功
 */
esp_err_t hal_led_init(void);

/**
 * @brief 直接设置 RGB 颜色并立即显示
 *
 * 用于 GIF 颜色追踪等功能，跳过模式管理。
 *
 * @param r 红色分量 (0~255)
 * @param g 绿色分量 (0~255)
 * @param b 蓝色分量 (0~255)
 */
void hal_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 设置 LED 模式（0=红, 1=绿, 2=蓝），自动打开 LED
 *
 * @param mode 模式索引 (0~2)，超出范围会自动取模
 */
void hal_led_set_mode(uint8_t mode);

/**
 * @brief 设置亮度 0~255
 *
 * @param brightness 亮度值，0 为灭，255 为最亮
 */
void hal_led_set_brightness(uint8_t brightness);

/**
 * @brief 相对调整亮度（+/-），自动处理边界
 *
 * @param delta 亮度变化量（可为负值）
 */
void hal_led_adjust_brightness(int delta);

/** @brief 打开 LED（恢复上次模式） */
void hal_led_on(void);

/** @brief 关闭 LED */
void hal_led_off(void);

/** @brief 切换开关 */
void hal_led_toggle(void);

/** @brief 当前是否打开 */
bool hal_led_is_on(void);

/** @brief 当前模式 */
uint8_t hal_led_get_mode(void);

/** @brief 当前亮度 */
uint8_t hal_led_get_brightness(void);

#ifdef __cplusplus
}
#endif
