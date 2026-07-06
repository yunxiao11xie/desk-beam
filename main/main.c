/**
 * @file main.c
 * @brief desk-beam — ESP32-S31 桌面副屏伴侣 — 主程序入口
 *
 * 仅负责模块初始化编排，不含业务逻辑。
 *
 * 初始化顺序：
 *   1. LCD 面板（RGB 并行接口）
 *   2. GT1151 触摸（I2C）
 *   3. LVGL 端口（仅注册显示+触摸，不创建 UI）
 *   4. 音乐屏幕（全屏歌词界面，启动即显示）
 *   5. WS2812 LED + 氛围灯
 *   6. SD 卡（SDMMC 4-bit → FATFS，失败仅警告）
 *   7. ADC 按键（启动扫描任务）
 *   8. 按键业务调度
 *   9. 网络任务（WiFi + WebSocket，后台异步）
 *
 * ══════════════════════════════════════════════════════════════════════
 *   ⚠  必 须 配 置 项
 *   ─────────────────
 *   在下面修改 WIFI_SSID / WIFI_PASSWORD 为你家的 WiFi 名称和密码。
 *   修改 PC_HOST 为运行 windows_media_server.py 的电脑 IP 地址。
 * ══════════════════════════════════════════════════════════════════════
 */
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "bsp/bsp_board.h"
#include "hal_display/hal_display.h"
#include "hal_led/hal_led.h"
#include "hal_key/hal_key.h"
#include "app_ui.h"
#include "app_logic.h"
#include "app_network.h"
#include "app_ws_client.h"
#include "app_music_screen.h"
#include "app_deepseek_screen.h"
#include "app_led_effects.h"
#include "hal_sdcard/hal_sdcard.h"

static const char *TAG = "main";

/* ═══════════════════════════════════════════════════════════════════
 *  配 置 区 — 按你的环境修改
 * ═══════════════════════════════════════════════════════════════════ */

/** WiFi 名称（2.4 GHz） */
#define WIFI_SSID       "xxxxxxxx"

/** WiFi 密码 */
#define WIFI_PASSWORD   "xxxxxxxxx"

/** 运行 windows_media_server.py 的 PC IP 地址 */
#define PC_HOST         "192.168.xxx.xxx"

/** PC WebSocket 服务器端口（默认 8765） */
#define PC_WS_PORT      8765

/** WiFi 连接超时（毫秒） */
#define WIFI_TIMEOUT_MS 20000


/* ═══════════════════════════════════════════════════════════════════
 *  网络连接任务（WiFi → WebSocket）
 * ═══════════════════════════════════════════════════════════════════ */

static void network_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "网络任务启动");

    /* ---- 1. 初始化 WiFi ---- */
    esp_err_t ret = app_network_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败，网络功能不可用");
        vTaskDelete(NULL);
        return;
    }

    /* ---- 2. 连接 WiFi ---- */
    ret = app_network_connect(WIFI_SSID, WIFI_PASSWORD);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 连接发起失败");
        vTaskDelete(NULL);
        return;
    }

    /* ---- 3. 等待连接成功 ---- */
    ret = app_network_wait_connected(pdMS_TO_TICKS(WIFI_TIMEOUT_MS));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "⚠ WiFi 连接超时！检查 SSID/密码");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  WiFi 已连接: %s", app_network_get_ip());
    ESP_LOGI(TAG, "============================================");

    /* 通知音乐界面：WiFi 已连 */
    app_music_screen_set_wifi_state(true, WIFI_SSID);

    /* ---- 4. 启动 WebSocket 客户端 ---- */
    ret = app_ws_start(PC_HOST, PC_WS_PORT, "/");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket 启动失败，仅离线模式");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WebSocket → ws://%s:%d/ 连接中...", PC_HOST, PC_WS_PORT);

    /* 任务保持存活：WS 客户端有独立的重连线程，
     * 本任务只需等待退出信号（暂不实现，保持常驻） */
    vTaskDelete(NULL);
}


void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  desk-beam — ESP32-S31 桌面副屏伴侣");
    ESP_LOGI(TAG, "  屏幕: %dx%d RGB565", LCD_H_RES, LCD_V_RES);
    ESP_LOGI(TAG, "  WiFi: %s", WIFI_SSID);
    ESP_LOGI(TAG, "  PC:   %s:%d", PC_HOST, PC_WS_PORT);
    ESP_LOGI(TAG, "============================================");

    /* ---- 1. LCD ---- */
    esp_lcd_panel_handle_t lcd = NULL;
    ESP_ERROR_CHECK(hal_display_lcd_init(&lcd));

    /* ---- 2. 触摸 ---- */
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t touch_err = hal_display_touch_init(&touch);
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "触摸初始化失败（无触摸交互）");
    }

    /* ---- 3. LVGL 端口（仅注册显示 + 触摸，不创建 UI） ---- */
    ESP_ERROR_CHECK(app_ui_init(lcd, touch));

    /* ---- 4. 音乐屏幕（全屏歌词界面，启动即显示） ---- */
    esp_err_t music_err = app_music_screen_init();
    if (music_err != ESP_OK) {
        ESP_LOGW(TAG, "音乐屏幕初始化失败（继续运行）");
    }
    app_music_screen_show();
    app_music_screen_set_wifi_state(false, NULL);   /* 初始离线 */

    /* ---- 4.5. DeepSeek API 用量页面（隐藏叠加层，等待数据） ---- */
    esp_err_t ds_err = app_deepseek_screen_init();
    if (ds_err != ESP_OK) {
        ESP_LOGW(TAG, "DeepSeek 页面初始化失败（继续运行）");
    }

    /* ---- 5. RGB LED  + 氛围灯效果 ---- */
    esp_err_t led_err = hal_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "LED 初始化跳过: %s", esp_err_to_name(led_err));
    }
    esp_err_t fx_err = app_led_effects_init();
    if (fx_err != ESP_OK) {
        ESP_LOGW(TAG, "氛围灯初始化跳过: %s", esp_err_to_name(fx_err));
    }

    /* ---- 5.5. SD 卡（SDMMC 4-bit → FATFS，失败仅警告） ---- */
    esp_err_t sd_err = hal_sdcard_mount();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载跳过: %s（继续运行）", esp_err_to_name(sd_err));
    } else {
        ESP_LOGI(TAG, "SD 卡已就绪: %llu MB",
                 (unsigned long long)hal_sdcard_get_total() / (1024 * 1024));
    }

    /* ---- 6. ADC 按键 ---- */
    QueueHandle_t key_queue = xQueueCreate(8, sizeof(key_event_msg_t));
    if (key_queue == NULL) {
        ESP_LOGE(TAG, "按键事件队列创建失败");
        return;
    }
    esp_err_t key_err = hal_key_init(key_queue);
    if (key_err != ESP_OK) {
        ESP_LOGW(TAG, "ADC 按键未启动: %s", esp_err_to_name(key_err));
    }

    /* ---- 7. 按键业务调度 ---- */
    xTaskCreate(app_logic_key_task, "app_logic", 4096, key_queue, 5, NULL);

    /* ---- 8. 网络任务（WiFi → WebSocket，后台） ---- */
    xTaskCreatePinnedToCore(network_task, "network", 6144, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "全部初始化完成！");
    ESP_LOGI(TAG, "提示：请在 pc_tools/windows_media_server.py 中按 [P] 暂停 [Q] 退出");
}
