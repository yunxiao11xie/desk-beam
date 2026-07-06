# ============================================================================
# @file CMakeLists.txt
# @brief HAL 显示子系统组件 — RGB LCD + GT1151 触摸
#
# LCD 和触摸共享同一条 I2C 总线（触摸挂在 LCD 接口上），
# 因此合并为一个 HAL 组件。
#
# 对外接口：hal_display/hal_display.h
# 依赖关系：
#   REQUIRES    bsp         — 板级引脚定义
#   PRIV_REQUIRES esp_driver_i2c — I2C 驱动
#                esp_lcd         — RGB LCD 面板驱动
#                esp_lcd_touch_gt1151 — GT1151 触摸驱动
# ============================================================================
idf_component_register(
    SRCS "hal_display.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES esp_driver_i2c esp_lcd esp_lcd_touch_gt1151
)
