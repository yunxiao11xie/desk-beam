/**
 * @file app_led_effects.c
 * @brief WS2812 氛围灯效果引擎实现
 *
 * === 效果任务 ===
 * 专用 FreeRTOS 任务 (app_led_effects, 2048 字节栈, 优先级 3)，
 * 每 ~30ms 渲染一帧，直接调用 hal_led_set_rgb() 输出。
 *
 * === 脉冲触发 ===
 * app_led_trigger_pulse() 由 app_music_screen 在歌词行切换时调用，
 * 设置目标颜色和触发标记，效果任务在下一帧开始执行脉冲动画。
 *
 * === 状态机 (PULSE 模式) ===
 *   IDLE (playing)  → 环境微光
 *   TRIGGERED       → ATTACK (0→200ms, 40%→100%)
 *                   → DECAY (200→600ms, 100%→60%)
 *                   → IDLE 恢复微光
 *   PAUSED          → 彩虹循环
 */
#include "app_led_effects.h"
#include "hal_led/hal_led.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "app_led_effects";

/* 前向声明 — render_pulse 在暂停时调用 render_rainbow */
static void render_rainbow(void);

/* ================================================================
 *  效果参数
 * ================================================================ */

/* 帧率 */
#define EFFECT_FRAME_MS         30      /* ~33fps */

/* 脉冲 */
#define PULSE_ATTACK_MS         200     /* 起振时长 */
#define PULSE_DECAY_MS          400     /* 衰减时长 */
#define PULSE_ATTACK_START      0.40f   /* 起振起始亮度比例 */
#define PULSE_PEAK              1.00f   /* 峰值亮度比例 */
#define PULSE_DECAY_END         0.60f   /* 衰减结束亮度比例 */

/* 呼吸 */
#define BREATHE_PERIOD_MS       2000    /* 呼吸周期 */

/* 彩虹 */
#define RAINBOW_CYCLE_MS        8000    /* 色相循环周期 */
#define RAINBOW_BRIGHTNESS      0.40f   /* 彩虹亮度比例 */

/* 微光 */
#define AMBIENT_BRIGHTNESS      0.08f   /* 无脉冲时环境微光比例 */


/* ================================================================
 *  内部状态
 * ================================================================ */

static bool             s_initialized   = false;
static TaskHandle_t     s_task          = NULL;

/* 模式 */
static led_effect_mode_t s_mode         = LED_EFFECT_PULSE;

/* 播放状态 */
static bool             s_playing       = true;

/* 全局亮度 (0~255) */
static uint8_t          s_brightness    = 200;

/* 脉冲状态 */
typedef struct {
    uint8_t r, g, b;            /* 脉冲目标颜色 */
    bool    active;             /* 是否有脉冲动画进行中 */
    uint32_t elapsed_ms;        /* 当前脉冲已运行时间 */
} pulse_state_t;
static pulse_state_t s_pulse = { .r = 80, .g = 220, .b = 255, .active = false, .elapsed_ms = 0 };

/* 呼吸步进 */
static uint32_t s_breath_tick = 0;

/* 彩虹色相 (0~360) */
static uint16_t s_rainbow_hue = 0;

/* 最后一帧输出的颜色（用于微光保持） */
static uint8_t s_last_r = 80, s_last_g = 220, s_last_b = 255;


/* ================================================================
 *  HSV → RGB 转换
 * ================================================================ */

static void hsv2rgb(uint16_t hue, uint8_t sat, uint8_t val,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region, remainder, p, q, t;
    if (sat == 0) {
        *r = *g = *b = val;
        return;
    }
    region = (uint8_t)(hue / 60);
    remainder = (uint8_t)((hue % 60) * 255 / 60);
    p = (uint8_t)((uint32_t)val * (255 - sat) / 255);
    q = (uint8_t)((uint32_t)val * (255 - (uint32_t)sat * remainder / 255) / 255);
    t = (uint8_t)((uint32_t)val * (255 - (uint32_t)sat * (255 - remainder) / 255) / 255);
    switch (region) {
    case 0: *r = val; *g = t;   *b = p;   break;
    case 1: *r = q;   *g = val; *b = p;   break;
    case 2: *r = p;   *g = val; *b = t;   break;
    case 3: *r = p;   *g = q;   *b = val; break;
    case 4: *r = t;   *g = p;   *b = val; break;
    default:*r = val; *g = p;   *b = q;   break;
    }
}


/* ================================================================
 *  亮度缩放
 * ================================================================ */

static void apply_brightness(uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t scale = s_brightness;
    *r = (uint8_t)((uint32_t)*r * scale / 255);
    *g = (uint8_t)((uint32_t)*g * scale / 255);
    *b = (uint8_t)((uint32_t)*b * scale / 255);
}


/* ================================================================
 *  效果渲染函数
 * ================================================================ */

/** 脉冲渲染：三阶段 (IDLE/ATTACK/DECAY) */
static void render_pulse(void)
{
    if (!s_pulse.active) {
        /* 无脉冲 → 环境微光或彩虹 */
        if (!s_playing) {
            /* 暂停 → 彩虹 */
            render_rainbow();
            return;
        }
        /* 播放中 → 保持最后一帧颜色的微光 */
        uint8_t r = (uint8_t)((float)s_last_r * AMBIENT_BRIGHTNESS);
        uint8_t g = (uint8_t)((float)s_last_g * AMBIENT_BRIGHTNESS);
        uint8_t b = (uint8_t)((float)s_last_b * AMBIENT_BRIGHTNESS);
        apply_brightness(&r, &g, &b);
        hal_led_set_rgb(r, g, b);
        return;
    }

    /* 脉冲进行中 */
    float ratio;
    if (s_pulse.elapsed_ms < PULSE_ATTACK_MS) {
        /* ATTACK: 40% → 100% */
        float t = (float)s_pulse.elapsed_ms / PULSE_ATTACK_MS;
        ratio = PULSE_ATTACK_START + (PULSE_PEAK - PULSE_ATTACK_START) * t;
    } else if (s_pulse.elapsed_ms < PULSE_ATTACK_MS + PULSE_DECAY_MS) {
        /* DECAY: 100% → 60% */
        float t = (float)(s_pulse.elapsed_ms - PULSE_ATTACK_MS) / PULSE_DECAY_MS;
        ratio = PULSE_PEAK - (PULSE_PEAK - PULSE_DECAY_END) * t;
    } else {
        /* 脉冲结束 */
        s_pulse.active = false;
        s_pulse.elapsed_ms = 0;
        /* 保存颜色用于微光 */
        s_last_r = s_pulse.r;
        s_last_g = s_pulse.g;
        s_last_b = s_pulse.b;
        render_pulse();  /* 递归 → 进入 IDLE 分支 */
        return;
    }

    uint8_t r = (uint8_t)((float)s_pulse.r * ratio);
    uint8_t g = (uint8_t)((float)s_pulse.g * ratio);
    uint8_t b = (uint8_t)((float)s_pulse.b * ratio);
    apply_brightness(&r, &g, &b);
    hal_led_set_rgb(r, g, b);
}

/** 呼吸渲染：正弦波亮度调制 */
static void render_breathe(void)
{
    float phase = (float)(s_breath_tick % BREATHE_PERIOD_MS) / BREATHE_PERIOD_MS;
    float brightness = 0.30f + 0.50f * (1.0f + sinf(phase * 2.0f * (float)M_PI)) / 2.0f;
    s_breath_tick += EFFECT_FRAME_MS;

    /* 暖色呼吸 */
    uint8_t r = (uint8_t)(255.0f * brightness);
    uint8_t g = (uint8_t)(140.0f * brightness);
    uint8_t b = (uint8_t)(80.0f  * brightness);
    apply_brightness(&r, &g, &b);
    hal_led_set_rgb(r, g, b);
}

/** 彩虹渲染：HSV 色相循环 */
static void render_rainbow(void)
{
    uint8_t r, g, b;
    hsv2rgb(s_rainbow_hue, 200, (uint8_t)(255 * RAINBOW_BRIGHTNESS), &r, &g, &b);

    /* 色相步进 (每帧) */
    uint32_t step = 360 * EFFECT_FRAME_MS / RAINBOW_CYCLE_MS;
    s_rainbow_hue = (s_rainbow_hue + (uint16_t)step) % 360;

    apply_brightness(&r, &g, &b);
    hal_led_set_rgb(r, g, b);
}


/* ================================================================
 *  效果任务
 * ================================================================ */

static void led_effect_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "氛围灯效果任务已启动");

    while (1) {
        switch (s_mode) {
        case LED_EFFECT_PULSE:
            render_pulse();
            break;
        case LED_EFFECT_BREATHE:
            render_breathe();
            break;
        case LED_EFFECT_RAINBOW:
            render_rainbow();
            break;
        case LED_EFFECT_OFF:
            hal_led_set_rgb(0, 0, 0);
            break;
        default:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(EFFECT_FRAME_MS));
    }
}


/* ================================================================
 *  对外接口
 * ================================================================ */

esp_err_t app_led_effects_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(
        led_effect_task, "app_led_effects", 2048, NULL, 3, &s_task
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "效果任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "氛围灯效果引擎已启动 (模式=%d)", s_mode);
    return ESP_OK;
}

void app_led_effects_set_mode(led_effect_mode_t mode)
{
    if (mode >= LED_EFFECT_COUNT) return;
    s_mode = mode;
    /* 切换模式时重置脉冲状态 */
    s_pulse.active = false;
    s_pulse.elapsed_ms = 0;
    ESP_LOGI(TAG, "效果模式 → %d", mode);
}

led_effect_mode_t app_led_effects_get_mode(void)
{
    return s_mode;
}

void app_led_effects_cycle_mode(void)
{
    led_effect_mode_t next = (s_mode + 1) % LED_EFFECT_COUNT;
    app_led_effects_set_mode(next);
}

void app_led_trigger_pulse(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) return;
    if (s_mode != LED_EFFECT_PULSE) return;  /* 非脉冲模式不响应 */

    s_pulse.r = r;
    s_pulse.g = g;
    s_pulse.b = b;
    s_pulse.active = true;
    s_pulse.elapsed_ms = 0;
}

void app_led_effects_set_playing(bool playing)
{
    s_playing = playing;
}

void app_led_effects_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

void app_led_effects_adjust_brightness(int delta)
{
    int v = (int)s_brightness + delta;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    s_brightness = (uint8_t)v;
}

uint8_t app_led_effects_get_brightness(void)
{
    return s_brightness;
}
