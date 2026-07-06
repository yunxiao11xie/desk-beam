/**
 * @file app_ui.h
 * @brief LVGL 端口初始化（仅此而已，不创建任何 UI 元素）
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 端口（显示 + 触摸）
 *
 * 仅注册 LVGL 显示设备和触摸输入，不创建任何 UI 对象。
 * 所有 UI 由 app_music_screen 模块独立创建。
 *
 * @param lcd   LCD 面板句柄
 * @param touch 触摸句柄（可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t app_ui_init(esp_lcd_panel_handle_t lcd, esp_lcd_touch_handle_t touch);

#ifdef __cplusplus
}
#endif
