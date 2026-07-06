/**
 * @file bsp.c
 * @brief BSP 板级支持包 — 编译时硬件配置验证
 *
 * BSP 的核心功能由 bsp_board.h 中的宏定义提供。
 * 本文件仅通过 static_assert 在编译时验证关键常量的有效性，
 * 确保引脚定义和尺寸参数在合理范围内。
 *
 * 如果此文件编译失败，请检查 bsp_board.h 中的宏定义。
 */
#include "bsp/bsp_board.h"

/* ---- 编译时检查：分辨率 ---- */
_Static_assert(LCD_H_RES >= 320 && LCD_H_RES <= 1920,
               "BSP: LCD_H_RES 超出合理范围");
_Static_assert(LCD_V_RES >= 240 && LCD_V_RES <= 1080,
               "BSP: LCD_V_RES 超出合理范围");

/* ---- 编译时检查：LED 引脚 ---- */
_Static_assert(LED_WS2812_GPIO >= 0 && LED_WS2812_GPIO <= 48,
               "BSP: LED_WS2812_GPIO 无效");

/* ---- 编译时检查：ADC 按键引脚 ---- */
_Static_assert(KEY_ARRAY_ADC_GPIO >= 0 && KEY_ARRAY_ADC_GPIO <= 48,
               "BSP: KEY_ARRAY_ADC_GPIO 无效");
