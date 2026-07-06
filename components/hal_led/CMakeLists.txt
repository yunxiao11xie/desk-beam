# ============================================================================
# @file CMakeLists.txt
# @brief HAL LED 组件 — WS2812 RGB LED (RMT 驱动)
#
# led_strip_encoder.c/h 是本组件的内部实现（源自乐鑫 RMT LED Strip 例程），
# 不对外暴露。外部只需通过 hal_led/hal_led.h 接口控制 LED。
#
# 对外接口：hal_led/hal_led.h
# 依赖关系：
#   REQUIRES    bsp      — 板级引脚定义（LED_WS2812_GPIO）
#   PRIV_REQUIRES driver — RMT 驱动
# ============================================================================
idf_component_register(
    SRCS "hal_led.c" "led_strip_encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES esp_driver_rmt
)
