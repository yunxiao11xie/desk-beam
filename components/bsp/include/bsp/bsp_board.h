/**
 * @file bsp_board.h
 * @brief ESP32-S31-Korvo 板级硬件配置（纯宏头文件，无函数）
 *
 * 所有引脚定义、屏幕常量、布局参数、嵌入式数据声明集中于此。
 * 各模块通过 #include "bsp/bsp_board.h" 获取硬件描述。
 *
 * === 分层说明 ===
 * BSP 层是项目中最底层的硬件描述层，位于：
 *   components/bsp/include/bsp/bsp_board.h
 *
 * === 使用示例 ===
 * @code{.c}
 * #include "bsp/bsp_board.h"
 * // 访问引脚号：LCD_HSYNC_GPIO, LED_WS2812_GPIO 等
 * // 访问屏幕尺寸：LCD_H_RES, LCD_V_RES
 * @endcode
 */
#pragma once

#include <stdint.h>
#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  RGB LCD 接口引脚（ST7262E43, RGB565 并行）
 *
 *  16 位 RGB565 并行数据总线 + 4 路控制信号。
 *  像素时钟 ~18MHz，帧率约 35Hz。
 * ================================================================ */
#define LCD_HSYNC_GPIO      GPIO_NUM_44   /* 行同步 */
#define LCD_VSYNC_GPIO      GPIO_NUM_45   /* 场同步 */
#define LCD_DE_GPIO         GPIO_NUM_43   /* 数据使能 */
#define LCD_PCLK_GPIO       GPIO_NUM_40   /* 像素时钟 */
#define LCD_DISP_GPIO       (-1)          /* 背光/显示使能（-1=不使用） */

/* RGB565 数据线（16 位并行，按原理图接线顺序） */
#define LCD_DATA0_GPIO      GPIO_NUM_8    /* B3 */
#define LCD_DATA1_GPIO      GPIO_NUM_9    /* B4 */
#define LCD_DATA2_GPIO      GPIO_NUM_10   /* B5 */
#define LCD_DATA3_GPIO      GPIO_NUM_11   /* B6 */
#define LCD_DATA4_GPIO      GPIO_NUM_12   /* B7 */
#define LCD_DATA5_GPIO      GPIO_NUM_13   /* G2 */
#define LCD_DATA6_GPIO      GPIO_NUM_14   /* G3 */
#define LCD_DATA7_GPIO      GPIO_NUM_15   /* G4 */
#define LCD_DATA8_GPIO      GPIO_NUM_16   /* G5 */
#define LCD_DATA9_GPIO      GPIO_NUM_17   /* G6 */
#define LCD_DATA10_GPIO     GPIO_NUM_18   /* G7 */
#define LCD_DATA11_GPIO     GPIO_NUM_19   /* R3 */
#define LCD_DATA12_GPIO     GPIO_NUM_33   /* R4 */
#define LCD_DATA13_GPIO     GPIO_NUM_34   /* R5 */
#define LCD_DATA14_GPIO     GPIO_NUM_35   /* R6 */
#define LCD_DATA15_GPIO     GPIO_NUM_36   /* R7 */

/* ================================================================
 *  GT1151 触摸 I2C 引脚
 *
 *  触摸控制器通过 I2C 总线与 ESP32-S31 通信，
 *  与 LCD 共用同一条 I2C 外设总线。
 * ================================================================ */
#define TOUCH_I2C_SDA       GPIO_NUM_0    /* I2C 数据线 */
#define TOUCH_I2C_SCL       GPIO_NUM_1    /* I2C 时钟线 */
#define TOUCH_RST_GPIO      (-1)          /* 触摸复位（-1=不使用） */
#define TOUCH_INT_GPIO      (-1)          /* 触摸中断（-1=不使用） */

/* ================================================================
 *  WS2812 RGB LED
 * ================================================================ */
#define LED_WS2812_GPIO     GPIO_NUM_37

/* ================================================================
 *  按键阵列
 * ================================================================ */
#define KEY_ARRAY_ADC_GPIO  GPIO_NUM_42

/* ================================================================
 *  microSD 卡槽 — SDMMC 4-bit 模式
 *
 *  引脚号根据官方原理图：GPIO20~25 分别对应 SDIO_DATA0~CMD。
 *  与 LCD 数据线（GPIO8~19）无冲突。
 * ================================================================ */
#define SDMMC_CLK_GPIO      GPIO_NUM_24
#define SDMMC_CMD_GPIO      GPIO_NUM_25
#define SDMMC_D0_GPIO       GPIO_NUM_20
#define SDMMC_D1_GPIO       GPIO_NUM_21
#define SDMMC_D2_GPIO       GPIO_NUM_22
#define SDMMC_D3_GPIO       GPIO_NUM_23

/* ================================================================
 *  屏幕分辨率
 * ================================================================ */
#define LCD_H_RES           800
#define LCD_V_RES           480

/* ================================================================
 *  LED 亮度
 * ================================================================ */
#define LED_BRIGHTNESS      204           /* ~80% */

/* ================================================================
 *  ADC 按键扫描参数
 * ================================================================ */
#define KEY_SCAN_PERIOD_MS      50
#define KEY_DEBOUNCE_SAMPLES    3
#define KEY_LONG_PRESS_MS       1000

#ifdef __cplusplus
}
#endif
