/**
 * @file hal_sdcard.h
 * @brief HAL SD 卡子系统 — SDMMC 4-bit 模式 + FATFS 挂载
 *
 * 封装 ESP-IDF 的 SDMMC 驱动 + FATFS 文件系统，提供简洁的挂载/卸载接口。
 * 初始化失败仅返回错误码，不阻塞系统启动。
 *
 * === 硬件连接（ESP32-S31-Korvo V1.1）===
 *   SDIO_DATA0  → GPIO20
 *   SDIO_DATA1  → GPIO21
 *   SDIO_DATA2  → GPIO22
 *   SDIO_DATA3  → GPIO23
 *   SDIO_CLK    → GPIO24
 *   SDIO_CMD    → GPIO25
 *
 * === 使用示例 ===
 * @code{.c}
 * esp_err_t err = hal_sdcard_mount();
 * if (err == ESP_OK) {
 *     ESP_LOGI("app", "SD 容量: %llu MB", hal_sdcard_get_total() / (1024*1024));
 * }
 * @endcode
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 SDMMC 并挂载 FATFS
 *
 * 挂载点: /sdcard
 * 失败时仅返回错误码，不 panic。可重复调用（先自动卸载）。
 *
 * @return ESP_OK 成功，否则失败
 */
esp_err_t hal_sdcard_mount(void);

/**
 * @brief 卸载 FATFS 并释放 SDMMC 驱动
 */
esp_err_t hal_sdcard_unmount(void);

/**
 * @brief 查询 SD 卡是否已挂载
 */
bool hal_sdcard_is_mounted(void);

/**
 * @brief 获取 SD 卡总容量（字节）
 * @return 总字节数，未挂载时返回 0
 */
size_t hal_sdcard_get_total(void);

/**
 * @brief 获取 SD 卡剩余容量（字节）
 * @return 剩余字节数，未挂载时返回 0
 */
size_t hal_sdcard_get_free(void);

#ifdef __cplusplus
}
#endif
