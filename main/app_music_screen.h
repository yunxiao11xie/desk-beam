/**
 * @file app_music_screen.h
 * @brief 全屏音乐界面 — 顶部信息栏 + 歌词 + 底部控制栏
 *
 * === 布局 (800×480) ===
 *   y=0    顶部状态栏 (48px) — 歌名/歌手/模式/时间/状态
 *   y=48  歌词主区域 (376px) — 6 行歌词，当前行高亮居中
 *   y=424 底部工具栏 (56px) — 播放控制/模式/进度条
 *
 * === 分层 ===
 * 本模块属于应用层，直接调用 LVGL API。
 * 数据由 app_ws_client 的消息分发函数喂入。
 *
 * === 显示模式 ===
 *   0 — 歌词 (Lyrics)
 *   1 — 正在播放 (Now Playing)
 *   2 — 频谱 (Visualizer)
 *   3 — 信息 (Info)
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 显示模式 */
typedef enum {
    MUSIC_MODE_LYRICS = 0,      /**< 滚动歌词 */
    MUSIC_MODE_NOW_PLAYING,     /**< 正在播放：大封面 + 歌名 */
    MUSIC_MODE_VISUALIZER,      /**< 频谱可视化（预留） */
    MUSIC_MODE_INFO,            /**< 系统信息 */
    MUSIC_MODE_COUNT
} music_display_mode_t;

/**
 * @brief 全屏音乐界面初始化
 *
 * 创建顶部状态栏、歌词区域、底部工具栏的所有 LVGL 对象。
 * 初始状态为隐藏，调用 app_music_screen_show() 显示。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_music_screen_init(void);

/**
 * @brief 显示/隐藏全屏音乐界面
 *
 * 显示时覆盖整个屏幕（800×480），隐藏时恢复 GIF 播放器界面。
 */
void app_music_screen_show(void);
void app_music_screen_hide(void);
bool app_music_screen_is_visible(void);

/**
 * @brief 更新歌曲信息（歌名 + 歌手 + 专辑）
 *
 * @param title  歌曲标题（会被内部复制）
 * @param artist 艺术家
 */
void app_music_screen_set_song(const char *title, const char *artist);

/**
 * @brief 更新播放进度
 *
 * 自动更新进度条宽度和时间戳显示。
 *
 * @param position_ms 当前播放位置（毫秒）
 * @param duration_ms 总时长（毫秒），0 表示未知
 */
void app_music_screen_set_position(uint32_t position_ms, uint32_t duration_ms);

/**
 * @brief 设置播放/暂停状态
 *
 * 更新顶部状态栏的 ▶/❚❚ 图标。
 *
 * @param playing true=播放中, false=暂停
 */
void app_music_screen_set_play_state(bool playing);

/**
 * @brief 设置播放模式（随机/重复/顺序）
 *
 * 更新底部工具栏的模式图标高亮。
 *
 * @param shuffle    true=随机播放
 * @param repeat_mode "off"=顺序, "one"=单曲循环, "all"=列表循环
 */
void app_music_screen_set_play_mode(bool shuffle, const char *repeat_mode);

/**
 * @brief 设置带时间戳的歌词列表
 *
 * 模块内部会按 position_ms 自动匹配当前行并更新 6 行显示。
 *
 * @param texts    歌词文本数组
 * @param times_ms 每行对应的毫秒时间戳
 * @param count    行数
 */
void app_music_screen_set_lyrics(const char **texts, const uint32_t *times_ms, int count);

/**
 * @brief 设置连接状态指示
 *
 * @param connected true=WebSocket 已连, false=断开
 */
void app_music_screen_set_conn_state(bool connected);

/**
 * @brief 更新顶部 WiFi 状态显示
 *
 * @param connected true=已连接
 * @param ssid     WiFi 名称（NULL 表示未知）
 */
void app_music_screen_set_wifi_state(bool connected, const char *ssid);

/**
 * @brief 切换显示模式（Lyrics / Now Playing / Visualizer / Info）
 *
 * @param mode 目标模式
 */
void app_music_screen_set_mode(music_display_mode_t mode);

/**
 * @brief 获取当前显示模式
 *
 * @return 当前模式枚举值
 */
music_display_mode_t app_music_screen_get_mode(void);

/**
 * @brief 循环切换到下一个显示模式
 */
void app_music_screen_cycle_mode(void);

/**
 * @brief 设置专辑封面背景（SD 卡 JPEG 路径）
 *
 * 内部用 ESP32-S31 硬件 JPEG 解码器 + PPA 缩放为 RGB565，全屏 cover-fit 填充（溢出部分居中裁切），
 * 叠加半透明黑色遮罩压暗以保证歌词可读。path 为 NULL 或空字符串时清除封面、
 * 恢复纯黑背景。
 *
 * @param path SD 卡上 JPEG 文件路径（如 /sdcard/covers/xxxx.jpg）
 */
void app_music_screen_set_cover(const char *path);

#ifdef __cplusplus
}
#endif
