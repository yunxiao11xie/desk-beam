/**
 * @file hal_sdcard.c
 * @brief HAL SD 卡实现 — SDMMC 4-bit → FATFS
 *
 * 使用 ESP-IDF esp_vfs_fat_sdmmc_mount() 一站式完成：
 *   SDMMC 主机初始化 → 卡检测 → FATFS 挂载 → VFS 注册
 *
 * 卸载时按逆序释放所有资源。
 */
#include "hal_sdcard/hal_sdcard.h"
#include "bsp/bsp_board.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "hal_sdcard";

/** FATFS 挂载点路径 */
#define SD_MOUNT_POINT      "/sdcard"

/** SDMMC 总线位宽（使用硬件 4-bit 模式） */
#define SDMMC_BUS_WIDTH     4

/** 单扇区大小（标准 SD 卡，字节） */
#define SD_SECTOR_SIZE      512

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;


/* ================================================================
 *  内部：构造 SDMMC 槽位配置
 *
 *  将 BSP 定义的 GPIO 引脚号写入 slot_config，
 *  启用内部上拉（板级无外部上拉电阻）。
 * ================================================================ */

static sdmmc_slot_config_t slot_config_create(void)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    /* 自定义引脚映射（覆盖默认引脚） */
    slot.clk = SDMMC_CLK_GPIO;
    slot.cmd = SDMMC_CMD_GPIO;
    slot.d0  = SDMMC_D0_GPIO;
    slot.d1  = SDMMC_D1_GPIO;
    slot.d2  = SDMMC_D2_GPIO;
    slot.d3  = SDMMC_D3_GPIO;

    /* 启用内部上拉（许多 Korvo 板未安装外部上拉） */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    return slot;
}


/* ================================================================
 *  公共 API
 * ================================================================ */

esp_err_t hal_sdcard_mount(void)
{
    /* 已挂载 → 先卸载，允许重复调用 */
    if (s_mounted) {
        hal_sdcard_unmount();
    }

    esp_err_t ret;

    /* ---- 1. 获取 SDMMC 主机实例（slot 1，4-bit 模式） ---- */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags      = SDMMC_HOST_FLAG_4BIT;     /* 强制 4-bit 总线 */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;     /* 400 kHz 初始化 */

    /* ---- 2. 构造槽位配置 ---- */
    sdmmc_slot_config_t slot = slot_config_create();

    /* ---- 3. 挂载 FATFS（初始化 + 卡检测 + 挂载，一站式） ---- */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,        /* 不自动格式化 */
        .max_files              = 2,            /* 仅需少量文件句柄 */
        .allocation_unit_size   = 16 * 1024,    /* 16 KB 簇对齐 */
        .disk_status_check_enable = false,
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡挂载失败: %s", esp_err_to_name(ret));
        s_mounted = false;
        s_card = NULL;
        return ret;
    }

    /* ---- 4. 切换至高速模式 ---- */
    /* 卡初始化时以 400 kHz 运行，初始化完成后可提高频率 */
    sdmmc_card_t *card = s_card;
    if (card != NULL) {
        /* 尝试切换至 20 MHz 高速模式 */
        uint32_t target_freq = 20000;   /* 保守使用 20 MHz（默认高速） */
        esp_err_t speed_ret = sdmmc_host_set_card_clk(host.slot, target_freq);
        if (speed_ret != ESP_OK) {
            ESP_LOGW(TAG, "高速模式切换失败，保持低速: %s",
                     esp_err_to_name(speed_ret));
            target_freq = host.max_freq_khz;
        }

        /* 打印卡信息 */
        ESP_LOGI(TAG, "SD 卡已挂载: %s OEM:%d",
                 card->cid.name, card->cid.oem_id);
        ESP_LOGI(TAG, "  容量: %llu MB  (频率: %lu KHz)",
                 ((uint64_t)card->csd.capacity * card->csd.sector_size
                  + (512 * 1024 - 1)) / (512 * 1024),
                 (unsigned long)target_freq);
    }

    s_mounted = true;
    return ESP_OK;
}


esp_err_t hal_sdcard_unmount(void)
{
    if (!s_mounted || s_card == NULL) {
        s_mounted = false;
        s_card = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    /* 卸载 FATFS + 释放 SDMMC 驱动 */
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD 卡卸载异常: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD 卡已安全卸载");
    }

    return ret;
}


bool hal_sdcard_is_mounted(void)
{
    return s_mounted;
}


size_t hal_sdcard_get_total(void)
{
    if (!s_mounted || s_card == NULL) {
        return 0;
    }

    /* 注意：sdmmc_card_t 没有直接的总容量字段，需从 CSD 计算 */
    /* csd.capacity = 块数，csd.sector_size = 每块字节数（通常 512） */
    uint64_t total = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    return (size_t)total;
}


size_t hal_sdcard_get_free(void)
{
    if (!s_mounted) {
        return 0;
    }

    /* 使用 FATFS 标准 API 查询剩余空间 */
    FATFS *fs;
    DWORD free_clusters;

    FRESULT fr = f_getfree(SD_MOUNT_POINT "/", &free_clusters, &fs);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "获取剩余空间失败: %d", fr);
        return 0;
    }

    uint64_t free_bytes = (uint64_t)free_clusters * fs->csize * SD_SECTOR_SIZE;
    return (size_t)free_bytes;
}
