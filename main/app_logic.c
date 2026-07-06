/**
 * @file app_logic.c
 * @brief 业务逻辑调度层 — 按键 → 音乐控制 + 氛围灯
 *
 * 从 hal_key 队列读取按键事件，调度 app_led_effects、app_ws_client。
 * 这是唯一包含"按了什么键该做什么事"这个知识的地方。
 *
 * === 按键映射 ===
 *  SET 短按 → WebSocket play_pause（播放/暂停）
 *  SET 长按 → 循环切换氛围灯模式（脉冲→呼吸→彩虹→关→脉冲）
 *  MODE 短按 → WebSocket next（下一曲）
 *  MODE 长按 → 循环切换显示模式（Lyrics→NowPlaying→Visualizer→Info）
 *  VOL- 短按 → 氛围灯亮度减 20
 *  VOL- 长按 → WebSocket prev（上一曲）
 *  VOL+ 短按 → 氛围灯亮度加 20
 *  VOL+ 长按 → WebSocket next（下一曲）
 */
#include "bsp/bsp_board.h"
#include "app_logic.h"
#include "app_led_effects.h"
#include "hal_key/hal_key.h"
#include "app_ws_client.h"
#include "app_music_screen.h"

#include "esp_log.h"

static const char *TAG = "app_logic";


void app_logic_key_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    key_event_msg_t msg;

    ESP_LOGI(TAG, "按键业务任务已启动");

    while (1) {
        if (xQueueReceive(q, &msg, portMAX_DELAY)) {
            ESP_LOGI(TAG, "KEY: %s", msg.long_press ? "LONG" : "SHORT");

            switch (msg.key) {

            case KEY_EVENT_SET:
                if (msg.long_press) {
                    app_led_effects_cycle_mode();
                } else {
                    app_ws_send_command("play_pause");
                }
                break;

            case KEY_EVENT_MODE:
                if (msg.long_press) {
                    /* 长按 MODE → 循环切换显示模式 */
                    app_music_screen_cycle_mode();
                } else {
                    app_ws_send_command("next");
                }
                break;

            case KEY_EVENT_VOL_MINUS:
                if (msg.long_press) {
                    /* Phase 5: 长按 VOL- → prev（上一曲） */
                    app_ws_send_command("prev");
                } else {
                    app_led_effects_adjust_brightness(-20);
                }
                break;

            case KEY_EVENT_VOL_PLUS:
                if (msg.long_press) {
                    /* Phase 5: 长按 VOL+ → next（下一曲） */
                    app_ws_send_command("next");
                } else {
                    app_led_effects_adjust_brightness(20);
                }
                break;

            default:
                break;
            }
        }
    }
}
