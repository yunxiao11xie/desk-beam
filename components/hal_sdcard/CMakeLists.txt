# ============================================================================
# @file CMakeLists.txt
# @brief HAL SD 卡组件 — SDMMC 4-bit → FATFS 挂载
#
# 对外接口：hal_sdcard/hal_sdcard.h
# 依赖关系：
#   REQUIRES      bsp       — 板级引脚定义（SDMMC_CLK/CMD/D0~D3_GPIO）
#   PRIV_REQUIRES sdmmc     — SDMMC 主机驱动
#                 fatfs     — FAT 文件系统
# ============================================================================
idf_component_register(
    SRCS "hal_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES sdmmc fatfs
)
