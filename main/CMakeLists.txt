# ============================================================================
# @file CMakeLists.txt
# @brief 应用层主组件 — 入口 + LVGL 端口 + 音乐屏幕 + 网络
#
# 本组件仅包含应用层代码，不直接操作硬件。
# 硬件抽象通过 REQUIRES 引入的 HAL 组件完成。
#
# 源文件列表：
#   main.c                — 入口编排
#   app_ui.c/h            — LVGL 端口初始化（仅注册显示+触摸）
#   app_music_screen.c/h  — 全屏音乐歌词界面
#   app_deepseek_screen.c/h — DeepSeek API 用量可视化页面
#   app_network.c/h       — WiFi 站模式连接管理
#   app_ws_client.c/h     — WebSocket 客户端 + JSON 协议
#   app_logic.c/h         — 按键 → 音乐控制调度
#   font_noto_sc_20.c     — 预渲染 Bitmap 字库（Noto Sans SC 20px, RLE）
#   font_noto_sc_28.c     — 预渲染 Bitmap 字库（Noto Sans SC 28px, RLE）
#
# 依赖关系：
#   REQUIRES      hal_display hal_led hal_key hal_sdcard  — 硬件抽象层
#   PRIV_REQUIRES lvgl esp_lvgl_port                       — LVGL 图形库
#                 esp_websocket_client                     — WebSocket 客户端
#                 cjson                                    — JSON 解析
#                 esp_wifi nvs_flash                       — WiFi 连接
#
# 字体：
#   font_noto_sc_20.c (20px) + font_noto_sc_28.c (28px)
#   预渲染 Bitmap 字库，编译进固件（RLE 压缩）。
# ============================================================================
idf_component_register(
    SRCS "font_noto_sc_28.c" "main.c" "app_logic.c" "app_ui.c"
         "app_network.c" "app_ws_client.c" "app_music_screen.c"
         "app_deepseek_screen.c" "app_led_effects.c" "font_noto_sc_20.c"
    INCLUDE_DIRS "."
    REQUIRES hal_display hal_led hal_key hal_sdcard
    PRIV_REQUIRES lvgl esp_lvgl_port esp_wifi nvs_flash
                 esp_websocket_client cjson
)

