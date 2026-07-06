/**
 * @file app_ui.c
 * @brief LVGL 端口初始化 — 仅注册显示和触摸设备，不创建任何 UI 元素
 *
 * 所有 UI 由 app_music_screen 模块独立创建和管理。
 */
#include "bsp/bsp_board.h"
#include "app_ui.h"

#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "app_ui";



/* ================================================================
 *  LVGL 端口初始化
 * ================================================================ */

esp_err_t app_ui_init(esp_lcd_panel_handle_t lcd, esp_lcd_touch_handle_t touch)
{
    /* ---- 1. LVGL 端口 ---- */
    lvgl_port_cfg_t lvgl_cfg = {
        .task_priority   = 4,
        .task_stack      = 16384,
        .task_affinity   = 0,
        .task_max_sleep_ms = 10,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL 端口初始化失败");

    /* ---- 2. 注册显示设备 ---- */
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = NULL,
        .panel_handle  = lcd,
        .buffer_size   = LCD_H_RES * 100,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .flags = {
            .buff_dma   = true,
            .buff_spiram = true,
        },
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode       = 0,
            .avoid_tearing = 0,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "显示设备注册失败");
        return ESP_FAIL;
    }

    /* ---- 3. 注册触摸（可选） ---- */
    if (touch != NULL) {
        lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "触摸注册失败（继续运行）");
        }
    }

    /* ---- 4. 开启显示 ---- */
    esp_lcd_panel_disp_on_off(lcd, true);

    ESP_LOGI(TAG, "LVGL 端口初始化完成");
    return ESP_OK;
}
