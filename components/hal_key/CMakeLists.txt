# ============================================================================
# @file CMakeLists.txt
# @brief HAL 按键组件 — ADC 按键阵列
#
# ADC 扫描 → 消抖 → 分类 → 队列发送。不含任何业务逻辑。
# 业务调度由 app_logic 从队列读取事件后完成。
#
# 对外接口：hal_key/hal_key.h
# 依赖关系：
#   REQUIRES    bsp       — 板级引脚定义（KEY_ARRAY_ADC_GPIO）
#   PRIV_REQUIRES esp_adc   — ADC 驱动 + 校准方案
#                esp_timer — 按键长按计时
# ============================================================================
idf_component_register(
    SRCS "hal_key.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp
    PRIV_REQUIRES esp_adc esp_timer
)
