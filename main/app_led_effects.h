/**
 * @file app_led_effects.h
 * @brief WS2812 氛围灯效果引擎 — 脉冲/呼吸/彩虹/关闭
 *
 * === 效果模式 ===
 *   PULSE   — 歌词行切换脉冲（默认），暂停时自动降为彩虹
 *   BREATHE — 正弦波呼吸律动
 *   RAINBOW — HSV 色相循环彩虹
 *   OFF     — 关闭
 *
 * === 分层 ===
 * 本模块属于应用层，直接调用 hal_led_set_rgb() 输出颜色。
 * 背景效果由专用 FreeRTOS 任务驱动（~33fps），
 * 脉冲由外部（app_music_screen）触发。
 *
 * === 线程安全 ===
 * 所有公共函数可从中断/任务上下文中调用。
 * 内部状态用 volatile 或单字节赋值保证可见性（FreeRTOS 单核模型）。
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 氛围灯效果模式 */
typedef enum {
    LED_EFFECT_PULSE = 0,      /**< 歌词切换脉冲（默认，暂停自动彩虹） */
    LED_EFFECT_BREATHE,        /**< 呼吸律动 */
    LED_EFFECT_RAINBOW,        /**< HSV 色相循环彩虹 */
    LED_EFFECT_OFF,            /**< 关闭 */
    LED_EFFECT_COUNT
} led_effect_mode_t;

/**
 * @brief 初始化氛围灯效果引擎
 *
 * 启动后台效果任务（栈 2048，优先级 3）。
 * 需在 hal_led_init() 之后调用。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_led_effects_init(void);

/**
 * @brief 设置效果模式
 *
 * @param mode 目标模式
 */
void app_led_effects_set_mode(led_effect_mode_t mode);

/**
 * @brief 获取当前效果模式
 *
 * @return 当前模式
 */
led_effect_mode_t app_led_effects_get_mode(void);

/**
 * @brief 循环切换到下一个效果模式
 */
void app_led_effects_cycle_mode(void);

/**
 * @brief 触发歌词脉冲
 *
 * 由 app_music_screen 在歌词行切换时调用。
 * 脉冲颜色建议传当前高亮色或歌词主色。
 *
 * @param r 红色分量 (0~255)
 * @param g 绿色分量 (0~255)
 * @param b 蓝色分量 (0~255)
 */
void app_led_trigger_pulse(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 通知播放状态
 *
 * PULSE 模式下，暂停 → 自动切换为彩虹；
 * 恢复播放 → 回到脉冲等待状态。
 *
 * @param playing true=播放中, false=暂停
 */
void app_led_effects_set_playing(bool playing);

/**
 * @brief 设置氛围灯微光基色（无脉冲时显示的颜色）
 *
 * 由 app_music_screen 在设置专辑封面时调用，传入封面主色；
 * 无封面时传入柔和默认色。PULSE 模式下，歌词脉冲结束后回到该基色。
 *
 * @param r 红色分量 (0~255)
 * @param g 绿色分量 (0~255)
 * @param b 蓝色分量 (0~255)
 */
void app_led_effects_set_base_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 设置全局亮度（0~255）
 *
 * 影响所有效果模式的最终输出亮度。
 *
 * @param brightness 亮度值
 */
void app_led_effects_set_brightness(uint8_t brightness);

/**
 * @brief 相对调整亮度
 *
 * @param delta 亮度变化量（可负）
 */
void app_led_effects_adjust_brightness(int delta);

/**
 * @brief 获取当前亮度
 *
 * @return 亮度值 0~255
 */
uint8_t app_led_effects_get_brightness(void);

#ifdef __cplusplus
}
#endif
