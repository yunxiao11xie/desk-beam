/**
 * @file app_music_screen.c
 * @brief 全屏音乐界面实现 — 顶部信息栏 + 6 行歌词 + 底部工具栏
 *
 * === 字体系统 ===
 * 使用 font_noto_sc_20.c (20px) + font_noto_sc_28.c (28px)
 * 预渲染 Bitmap 字库（RLE 压缩，编译进固件），覆盖 ASCII + CJK 全字集。
 * 图标 / 符号使用 Montserrat 内置字体。
 *
 * === 布局 (800×480, 从上到下) ===
 *   0 ~  48  顶部状态栏 — 歌名 / 歌手·专辑 / 播放模式 / 时间 / ▶❚❚
 *  48 ~ 424  歌词主区域 — 6 行歌词, 当前行高亮居中 (376px)
 * 424 ~ 480  底部工具栏 — ◄◄ ▶❚❚ ►► / 🔀 🔁 / 🎤 🔊 / 进度条
 *
 * === 歌词滚动 ===
 * 收到 position 消息 → 根据 position_ms 在歌词时间数组查找当前行
 * → 更新 6 个歌词标签的文字 + 颜色 + 字号
 *
 * === 线程安全 ===
 * 所有 public 函数在操作 LVGL 前调用 lvgl_port_lock/unlock
 */
#include "app_music_screen.h"
#include "app_deepseek_screen.h"
#include "app_ws_client.h"
#include "app_led_effects.h"
#include "app_network.h"
#include "bsp/bsp_board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"     /* ESP32-S31 硬件 JPEG 编解码器 */
#include "driver/ppa.h"             /* PPA 硬件缩放引擎 */
#include "esp_cache.h"              /* PPA→PSRAM cache 同步 */

/* 预渲染 Bitmap 字库 (编译进固件) */
LV_FONT_DECLARE(font_noto_sc_20);
LV_FONT_DECLARE(font_noto_sc_28);

static const char *TAG = "app_music";

/* ================================================================
 *  布局常量 (800 × 480)
 * ================================================================ */

#define SCREEN_W            LCD_H_RES       /* 800 */
#define SCREEN_H            LCD_V_RES       /* 480 */

#define TOP_BAR_H           48
#define BOTTOM_BAR_H        56
#define LYRICS_Y            TOP_BAR_H
#define LYRICS_H            (SCREEN_H - TOP_BAR_H - BOTTOM_BAR_H)   /* 376 */
#define LYRICS_CENTER_Y     (LYRICS_Y + LYRICS_H / 2)               /* 236 */

/* 进度条 (位于底部工具栏最下方) */
#define PROGRESS_BAR_Y      (SCREEN_H - 6)  /* 474 */
#define PROGRESS_BAR_H      4
#define PROGRESS_BAR_W      (SCREEN_W - 40)
#define PROGRESS_BAR_X      20

/* ---- 歌词行参数: 6 行, 当前行居中 ---- */
#define LYR_LINE_COUNT      6
/* 每行的垂直偏移 (相对 LYRICS_CENTER_Y 的像素偏移) */
static const int16_t LYR_OFFSET[LYR_LINE_COUNT] = { -99, -57, -17, 31, 77, 123 };
/* 0=20px (外围行), 1=28px (当前行第3行) */
static const uint8_t LYR_FONT_IDX[LYR_LINE_COUNT] = { 0, 0, 1, 0, 0, 0 };
/* 每行的亮度等级: 0~255 */
static const uint8_t LYR_BRIGHTNESS[LYR_LINE_COUNT] = { 100, 160, 255, 160, 110, 80 };


/* ---- 底部工具栏布局常量 ---- */
#define MODE_ICONS_X        {24, 90}
#define CTRL_ICONS_X        {320, 386, 452}
#define UTIL_ICONS_X        {690, 738}
#define SEP_L_X             210     /* 左分隔线 */
#define SEP_R_X             590     /* 右分隔线 */


/* ================================================================
 *  内部状态
 * ================================================================ */

static bool s_visible = false;
static bool s_is_playing = false;
static bool s_conn_state = false;
static uint32_t s_position_ms = 0;
static uint32_t s_duration_ms = 0;

/* ---- 歌词数据 ---- */
typedef struct {
    char     *text;
    uint32_t  time_ms;
} lyrics_line_t;

static lyrics_line_t *s_lyrics = NULL;
static int            s_lyrics_count = 0;
static int            s_lyrics_current = -1;

/* ---- 播放模式 ---- */
static bool     s_shuffle = false;
static char     s_repeat_mode[8] = "off";

/* ---- WiFi 状态 ---- */
static bool     s_wifi_connected = false;
static char     s_wifi_ssid[33]  = {0};
static lv_obj_t *s_wifi_label    = NULL;

/* ---- 歌词滚动动画 ---- */
static bool     s_anim_active      = false;
static int32_t  s_anim_offset      = 0;
static int      s_target_line_idx  = -1;
static int      s_pending_line_idx = -1;
static int      s_last_displayed   = -1;
static lv_anim_t s_scroll_anim;

#define LYRIC_SCROLL_DURATION_MS  250
#define LYRIC_LINE_HEIGHT_AVG     44

/* ---- 歌词行横向滚动参数 ---- */
#define LYRIC_SCROLL_SPEED      40      /* px/s (默认 dpi/3 ≈ 55) */
#define LYRIC_SCROLL_DELAY_MS   2000    /* 滚动循环间停顿 ms */

/* 动画模板：用于 SCROLL_CIRCULAR 的 repeat_delay */
static const lv_anim_t s_lyric_scroll_anim = {
    .repeat_delay = LYRIC_SCROLL_DELAY_MS,
};

/* ---- 显示模式 ---- */
static music_display_mode_t s_mode = MUSIC_MODE_LYRICS;

/* 各模式的顶层容器（歌词模式复用 s_container） */
static lv_obj_t *s_np_container  = NULL;   /* Now Playing */
static lv_obj_t *s_viz_container = NULL;   /* Visualizer */
static lv_obj_t *s_info_container = NULL;  /* Info */

/* Now Playing 模式子元素 */
static lv_obj_t *s_np_cover_placeholder = NULL;
static lv_obj_t *s_np_title = NULL;
static lv_obj_t *s_np_artist = NULL;
static lv_obj_t *s_np_album = NULL;
static lv_obj_t *s_np_progress = NULL;

/* Info 模式子元素 */
static lv_obj_t *s_info_ip_label = NULL;
static lv_obj_t *s_info_ssid_label = NULL;
static lv_obj_t *s_info_conn_label = NULL;
static lv_obj_t *s_info_fw_label = NULL;
static lv_obj_t *s_info_uptime_label = NULL;

/* (未使用保留位) */


/* ================================================================
 *  LVGL 对象
 * ================================================================ */

/* 容器 */
static lv_obj_t *s_container      = NULL;   /* 全屏透明容器 */

/* 顶部状态栏 */
static lv_obj_t *s_topbar_bg      = NULL;
static lv_obj_t *s_title_label    = NULL;   /* 歌名 */
static lv_obj_t *s_artist_label   = NULL;   /* 歌手·专辑 */
static lv_obj_t *s_mode_icon      = NULL;   /* 🔀 🔁 ▶ */
static lv_obj_t *s_time_label     = NULL;   /* 4:32 */
static lv_obj_t *s_play_icon      = NULL;   /* ▶ ❚❚ */
static lv_obj_t *s_conn_dot       = NULL;   /* 连接状态圆点 */

/* 歌词行标签 (6 个) */
static lv_obj_t *s_lyrics_line[LYR_LINE_COUNT] = { NULL };

/* 底部工具栏 */
static lv_obj_t *s_bottombar_bg   = NULL;
static lv_obj_t *s_btn_prev       = NULL;   /* ⏮ */
static lv_obj_t *s_btn_play       = NULL;   /* ▶/⏸ */
static lv_obj_t *s_btn_next       = NULL;   /* ⏭ */
static lv_obj_t *s_mode_shuffle   = NULL;   /* 🔀 */
static lv_obj_t *s_mode_repeat    = NULL;   /* 🔁 循环 */
static lv_obj_t *s_vol_icon       = NULL;   /* 🔊 */
static lv_obj_t *s_btn_next_page  = NULL;   /* ▶▶ 下一页 */
static lv_obj_t *s_sep_left       = NULL;   /* 左分隔线 */
static lv_obj_t *s_sep_right      = NULL;   /* 右分隔线 */

/* ---- 进度条 ---- */
static lv_obj_t *s_progress_bg    = NULL;
static lv_obj_t *s_progress_fg    = NULL;

/* ---- 专辑封面背景 ---- */
static lv_obj_t    *s_cover_img    = NULL;   /* 全屏封面图 */
static lv_obj_t    *s_cover_darken = NULL;   /* 半透明黑色遮罩 */
static lv_img_dsc_t *s_cover_dsc   = NULL;   /* 解码后的封面描述符（内部 DRAM） */
static uint8_t     *s_cover_fb     = NULL;   /* 解码后的 RGB565 帧缓冲（PSRAM，PPA 可读） */

/* 封面主色调色板（供氛围灯取色，歌词行切换时循环使用） */
#define COVER_PAL_MAX   5
#define LED_SOFT_R      200
#define LED_SOFT_G      225
#define LED_SOFT_B      250
static uint8_t  s_cover_palette[COVER_PAL_MAX][3];
static int      s_cover_palette_n = 0;
static int      s_cover_palette_idx = 0;

/* ═══════════════════════════════════════════════════════════════
 *  预渲染 Bitmap 字库
 *  使用 font_noto_sc_20.c + font_noto_sc_28.c 编译进固件
 *  20px: 歌词非当前行、歌手  /  28px: 歌名标题、歌词当前行高亮
 * ═══════════════════════════════════════════════════════════════ */

#define FONT_CN_20   (&font_noto_sc_20)
#define FONT_CN_28   (&font_noto_sc_28)

/** Montserrat 回退字体（图标 + 拉丁字母 + 数字） */
#define FONT_ICON_14 (&lv_font_montserrat_14)
#define FONT_ICON_20 (&lv_font_montserrat_20)
#define FONT_ICON_28 (&lv_font_montserrat_28)



/**
 * @brief 获取歌词字体：0=20px (外围), 1=28px (当前行高亮)
 */
static inline const lv_font_t *get_lyric_font(int font_idx)
{
    return (font_idx == 1) ? FONT_CN_28 : FONT_CN_20;
}


/* ================================================================
 *  内部帮助函数
 * ================================================================ */

static int find_lyrics_line(uint32_t pos_ms)
{
    /* 二分查找：歌词按 time_ms 升序，找最后一个 time_ms <= pos_ms 的行 */
    if (s_lyrics == NULL || s_lyrics_count <= 0) return -1;
    int lo = 0, hi = s_lyrics_count - 1;
    int result = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_lyrics[mid].time_ms <= pos_ms) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return result;
}

static void format_ms(char *buf, size_t size, uint32_t ms)
{
    uint32_t total_sec = ms / 1000;
    uint32_t min = total_sec / 60;
    uint32_t sec = total_sec % 60;
    snprintf(buf, size, "%u:%02u", (unsigned)min, (unsigned)sec);
}

/* ---- 进度条更新 ---- */
static void update_progress(void)
{
    if (s_progress_fg == NULL || s_time_label == NULL) return;

    uint32_t w = 0;
    if (s_duration_ms > 0 && s_position_ms <= s_duration_ms) {
        w = (uint32_t)((uint64_t)s_position_ms * PROGRESS_BAR_W / s_duration_ms);
    }
    lv_obj_set_width(s_progress_fg, w);

    char pos_buf[8], dur_buf[8], time_buf[24];
    format_ms(pos_buf, sizeof(pos_buf), s_position_ms);
    if (s_duration_ms > 0) {
        format_ms(dur_buf, sizeof(dur_buf), s_duration_ms);
        snprintf(time_buf, sizeof(time_buf), "%s / %s", pos_buf, dur_buf);
    } else {
        snprintf(time_buf, sizeof(time_buf), "%s", pos_buf);
    }
    lv_label_set_text(s_time_label, time_buf);
}

/* ---- 播放状态图标 ---- */
static void update_play_icon(void)
{
    if (s_play_icon == NULL) return;
    lv_label_set_text(s_play_icon, s_is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

/* ---- 播放模式图标 ---- */
static void update_mode_icon(void)
{
    if (s_mode_icon == NULL) return;
    if (s_shuffle) {
        lv_label_set_text(s_mode_icon, LV_SYMBOL_SHUFFLE);
    } else if (strcmp(s_repeat_mode, "one") == 0) {
        lv_label_set_text(s_mode_icon, LV_SYMBOL_LOOP);
    } else if (strcmp(s_repeat_mode, "all") == 0) {
        lv_label_set_text(s_mode_icon, LV_SYMBOL_LOOP);
    } else {
        lv_label_set_text(s_mode_icon, LV_SYMBOL_PLAY);
    }
}

/* ---- 连接状态圆点 ---- */
static void update_conn_dot(void)
{
    if (s_conn_dot == NULL) return;
    if (s_conn_state) {
        lv_obj_set_style_bg_color(s_conn_dot, lv_color_make(0, 220, 0), 0);
    } else {
        lv_obj_set_style_bg_color(s_conn_dot, lv_color_make(180, 40, 40), 0);
    }
}

/* ---- 歌词标签文字 + 颜色更新（不含位置动画） ---- */
static void update_lyrics_labels(int line_idx)
{
    for (int i = 0; i < LYR_LINE_COUNT; i++) {
        if (s_lyrics_line[i] == NULL) continue;

        int idx = (line_idx >= 0) ? (line_idx + i - 2) : -1;

        if (idx >= 0 && idx < s_lyrics_count && s_lyrics[idx].text != NULL) {
            lv_label_set_text(s_lyrics_line[i], s_lyrics[idx].text);
            lv_obj_set_style_text_font(s_lyrics_line[i], get_lyric_font(LYR_FONT_IDX[i]), 0);
            if (i == 2) {
                /* ★ 当前行（高亮行）：开启横向滚动，方便完整显示长句 */
                lv_label_set_long_mode(s_lyrics_line[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_set_style_text_color(s_lyrics_line[i],
                    lv_color_make(80, 220, 255), 0);
            } else {
                /* 非当前行：超出部分用 ... 截断 */
                lv_label_set_long_mode(s_lyrics_line[i], LV_LABEL_LONG_DOT);
                uint8_t b = LYR_BRIGHTNESS[i];
                lv_obj_set_style_text_color(s_lyrics_line[i],
                    lv_color_make(b, b, b), 0);
            }
        } else {
            lv_label_set_text(s_lyrics_line[i], "");
        }
    }
}

/* ---- 滚动动画执行回调：每帧更新 6 行标签 Y 偏移 ---- */
static void scroll_anim_exec_cb(void *var, int32_t v)
{
    s_anim_offset = v;
    for (int i = 0; i < LYR_LINE_COUNT; i++) {
        if (s_lyrics_line[i] == NULL) continue;
        lv_obj_set_y(s_lyrics_line[i], LYRICS_CENTER_Y + LYR_OFFSET[i] + v);
    }
}

/* 前向声明 — update_lyrics_display 引用 scroll_anim_ready_cb */
static void scroll_anim_ready_cb(lv_anim_t *a);

/* 前向声明 — update_lyrics_display 引用（定义见封面取色段） */
static void led_trigger_cover_pulse(void);

/* ---- 歌词 6 行更新（带动画） ---- */
static void update_lyrics_display(int line_idx)
{
    if (line_idx < 0) line_idx = -1;

    /* 动画进行中 → 只记下目标，动画结束后再切 */
    if (s_anim_active) {
        s_pending_line_idx = line_idx;
        return;
    }

    /* 同一行 → 不动 */
    if (line_idx == s_last_displayed) return;

    /* 歌词行切换 → 触发氛围灯脉冲（封面调色板循环取色） */
    if (line_idx >= 0) {
        led_trigger_cover_pulse();
    }

    /* 大幅度跳转（>2 行）→ 立即刷新，不做动画 */
    int jump = (s_last_displayed >= 0) ? abs(line_idx - s_last_displayed) : 999;
    if (jump > 2) {
        update_lyrics_labels(line_idx);
        s_last_displayed = line_idx;
        s_pending_line_idx = -1;
        return;
    }

    /* ---- 启动平滑上移动画 ---- */
    s_anim_active = true;
    s_target_line_idx = line_idx;
    s_pending_line_idx = -1;

    lv_anim_init(&s_scroll_anim);
    lv_anim_set_var(&s_scroll_anim, &s_anim_offset);
    lv_anim_set_exec_cb(&s_scroll_anim, scroll_anim_exec_cb);
    lv_anim_set_ready_cb(&s_scroll_anim, scroll_anim_ready_cb);
    lv_anim_set_values(&s_scroll_anim, 0, -LYRIC_LINE_HEIGHT_AVG);
    lv_anim_set_time(&s_scroll_anim, LYRIC_SCROLL_DURATION_MS);
    lv_anim_set_path_cb(&s_scroll_anim, lv_anim_path_ease_out);
    lv_anim_start(&s_scroll_anim);
}

/* ---- 滚动动画完成回调：复位 Y → 更新文字 → 处理 pending ---- */
static void scroll_anim_ready_cb(lv_anim_t *a)
{
    s_anim_active = false;
    s_anim_offset = 0;

    /* 复位所有标签到原始 Y 坐标 */
    for (int i = 0; i < LYR_LINE_COUNT; i++) {
        if (s_lyrics_line[i] == NULL) continue;
        lv_obj_set_y(s_lyrics_line[i], LYRICS_CENTER_Y + LYR_OFFSET[i]);
    }

    /* 用目标行更新文字 */
    update_lyrics_labels(s_target_line_idx);
    s_last_displayed = s_target_line_idx;

    /* 处理动画期间累积的 pending 行 */
    if (s_pending_line_idx >= 0 && s_pending_line_idx != s_target_line_idx) {
        int pending = s_pending_line_idx;
        s_pending_line_idx = -1;
        update_lyrics_display(pending);
    } else {
        s_pending_line_idx = -1;
    }
}

/* ---- 底部栏进度状态 ---- */
static void update_bottom_play_icon(void)
{
    if (s_btn_play == NULL) return;
    const char *state_str = s_is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    lv_label_set_text(s_btn_play, state_str);
}


/* ================================================================
 *  创建 UI 元素
 * ================================================================ */

static void create_top_bar(lv_obj_t *parent)
{
    /* 背景色块 */
    s_topbar_bg = lv_obj_create(parent);
    lv_obj_set_size(s_topbar_bg, SCREEN_W, TOP_BAR_H);
    lv_obj_set_pos(s_topbar_bg, 0, 0);
    lv_obj_set_style_radius(s_topbar_bg, 0, 0);
    lv_obj_set_style_border_width(s_topbar_bg, 0, 0);
    lv_obj_set_style_pad_all(s_topbar_bg, 0, 0);
    lv_obj_clear_flag(s_topbar_bg, LV_OBJ_FLAG_SCROLLABLE);
    /* 半透明黑底: 玻璃效果 */
    lv_obj_set_style_bg_color(s_topbar_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_topbar_bg, LV_OPA_50, 0);

    /* ---- 连接状态圆点 (左上角) ---- */
    s_conn_dot = lv_obj_create(s_topbar_bg);
    lv_obj_set_size(s_conn_dot, 8, 8);
    lv_obj_set_pos(s_conn_dot, 14, (TOP_BAR_H - 8) / 2);
    lv_obj_set_style_radius(s_conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_conn_dot, 0, 0);
    lv_obj_set_style_bg_color(s_conn_dot, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_bg_opa(s_conn_dot, LV_OPA_90, 0);
    lv_obj_clear_flag(s_conn_dot, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- WiFi 状态 (SSID + ✓/✗) ---- */
    s_wifi_label = lv_label_create(s_topbar_bg);
    lv_label_set_text(s_wifi_label, "WiFi:--   " LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_make(180, 60, 60), 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_wifi_label, 28, (TOP_BAR_H - 18) / 2);

    /* ---- 歌手·专辑 (左, 可滚动) — 按方案移到 X=200 ---- */
    s_artist_label = lv_label_create(s_topbar_bg);
    lv_label_set_text(s_artist_label, "");
    lv_obj_set_style_text_color(s_artist_label, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(s_artist_label, FONT_CN_20, 0);
    lv_obj_set_pos(s_artist_label, 200, 14);
    lv_label_set_long_mode(s_artist_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_artist_label, 160);

    /* ---- 歌名 (右中, 可滚动) — 28px 高亮 ---- */
    s_title_label = lv_label_create(s_topbar_bg);
    lv_label_set_text(s_title_label, "No Music");
    lv_obj_set_style_text_color(s_title_label, lv_color_make(240, 240, 240), 0);
    lv_obj_set_style_text_font(s_title_label, FONT_CN_28, 0);
    lv_obj_set_pos(s_title_label, 380, 8);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_title_label, 240);

    /* ---- 播放模式图标 (右三) ---- */
    s_mode_icon = lv_label_create(s_topbar_bg);
    lv_label_set_text(s_mode_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_mode_icon, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(s_mode_icon, FONT_ICON_20, 0);
    lv_obj_set_pos(s_mode_icon, 635, 14);

    /* ---- 时间 (右二) ---- */
    s_time_label = lv_label_create(s_topbar_bg);
    lv_label_set_text(s_time_label, "0:00");
    lv_obj_set_style_text_color(s_time_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(s_time_label, FONT_ICON_20, 0);
    lv_obj_set_pos(s_time_label, 665, 14);

    /* ---- 播放状态图标 (右一) - 已移除 ---- */
}

/* 前向声明 — 歌词行点击回调，在 create_lyrics_area 之后定义 */
static void lyrics_line_event_cb(lv_event_t *e);

static void create_lyrics_area(lv_obj_t *parent)
{
    /* ---- 6 行歌词标签 ---- */
    for (int i = 0; i < LYR_LINE_COUNT; i++) {
        s_lyrics_line[i] = lv_label_create(parent);
        lv_label_set_text(s_lyrics_line[i], "");
        lv_obj_set_pos(s_lyrics_line[i], 100, LYRICS_CENTER_Y + LYR_OFFSET[i]);
        lv_obj_set_width(s_lyrics_line[i], 600);
        lv_obj_set_style_text_align(s_lyrics_line[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(s_lyrics_line[i], get_lyric_font(LYR_FONT_IDX[i]), 0);
        uint8_t b = LYR_BRIGHTNESS[i];
        lv_obj_set_style_text_color(s_lyrics_line[i],
            lv_color_make(b, b, b), 0);
        lv_label_set_long_mode(s_lyrics_line[i], LV_LABEL_LONG_DOT);
        /* 预设横向滚动参数（用于当前高亮行）：
         *   anim_speed  → 滚动速度 px/s
         *   anim 模板   → 提供 repeat_delay（滚动间停顿）
         */
        lv_obj_set_style_anim_speed(s_lyrics_line[i], LYRIC_SCROLL_SPEED, 0);
        lv_obj_set_style_anim(s_lyrics_line[i], &s_lyric_scroll_anim, 0);
        lv_obj_set_style_opa(s_lyrics_line[i], LV_OPA_COVER, 0);

        /* ★ Phase 5: 歌词行可点击 → seek 跳转 */
        lv_obj_add_flag(s_lyrics_line[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_lyrics_line[i], lyrics_line_event_cb,
                            LV_EVENT_ALL, (void *)(intptr_t)i);
    }

}

/* ---- 歌词行触摸事件回调（点击跳转到该行时间点） ---- */
static void lyrics_line_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;

    /* 通过 user_data 传入该标签在 6 行数组中的索引（0~5） */
    int slot = (int)(intptr_t)lv_event_get_user_data(e);

    /* 映射到实际歌词行索引 */
    int line_idx = s_lyrics_current + slot - 2;
    if (line_idx < 0 || line_idx >= s_lyrics_count) return;

    uint32_t seek_ms = s_lyrics[line_idx].time_ms;
    esp_err_t ret = app_ws_send_seek(seek_ms);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "歌词点击跳转: 行 %d → %ums", line_idx, seek_ms);
    }
}

/* ---- 下一页按钮（→ DeepSeek 页面） ---- */
static void on_next_page_click(lv_event_t *e)
{
    (void)e;
    app_music_screen_hide();
    app_deepseek_screen_show();
}

/* ---- 底部按钮触摸事件回调 ---- */
static void bottom_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = lv_event_get_target(e);
    const char *action = (const char *)lv_event_get_user_data(e);

    if (action == NULL) return;

    if (code == LV_EVENT_PRESSED) {
        /* 按下变亮 */
        lv_obj_set_style_text_color(obj, lv_color_make(255, 255, 255), 0);
    } else if (code == LV_EVENT_RELEASED) {
        /* 释放恢复 */
        lv_obj_set_style_text_color(obj, lv_color_make(200, 200, 200), 0);
    } else if (code == LV_EVENT_CLICKED) {
        /* 发送命令 */
        app_ws_send_command(action);
        /* 播放/暂停按钮：立即切换本地图标，不等服务器回包 */
        if (strcmp(action, "play_pause") == 0) {
            s_is_playing = !s_is_playing;
            update_bottom_play_icon();
            update_play_icon();
        }
    }
}

static void create_bottom_bar(lv_obj_t *parent)
{
    /* 背景色块 */
    s_bottombar_bg = lv_obj_create(parent);
    lv_obj_set_size(s_bottombar_bg, SCREEN_W, BOTTOM_BAR_H);
    lv_obj_set_pos(s_bottombar_bg, 0, TOP_BAR_H + LYRICS_H);
    lv_obj_set_style_radius(s_bottombar_bg, 0, 0);
    lv_obj_set_style_border_width(s_bottombar_bg, 0, 0);
    lv_obj_set_style_pad_all(s_bottombar_bg, 0, 0);
    lv_obj_clear_flag(s_bottombar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_bottombar_bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bottombar_bg, LV_OPA_50, 0);
    /* 顶部分隔线 */
    lv_obj_set_style_border_width(s_bottombar_bg, 1, 0);
    lv_obj_set_style_border_side(s_bottombar_bg, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(s_bottombar_bg, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_border_opa(s_bottombar_bg, LV_OPA_30, 0);

    /* ---- 左栏：播放模式 2 图标 (28px) ---- */
    int mx[] = MODE_ICONS_X;
    const int icon_y = (BOTTOM_BAR_H - 28) / 2;   /* 垂直居中 = 14 */
    s_mode_shuffle = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_mode_shuffle, LV_SYMBOL_SHUFFLE);
    lv_obj_set_style_text_color(s_mode_shuffle, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(s_mode_shuffle, FONT_ICON_28, 0);
    lv_obj_set_pos(s_mode_shuffle, mx[0], icon_y);
    lv_obj_add_flag(s_mode_shuffle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_shuffle, bottom_btn_event_cb, LV_EVENT_ALL, (void *)"toggle_shuffle");

    s_mode_repeat = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_mode_repeat, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(s_mode_repeat, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(s_mode_repeat, FONT_ICON_28, 0);
    lv_obj_set_pos(s_mode_repeat, mx[1], icon_y);
    lv_obj_add_flag(s_mode_repeat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mode_repeat, bottom_btn_event_cb, LV_EVENT_ALL, (void *)"toggle_repeat");

    /* ---- 左分隔线 ---- */
    s_sep_left = lv_obj_create(s_bottombar_bg);
    lv_obj_set_size(s_sep_left, 1, 28);
    lv_obj_set_pos(s_sep_left, SEP_L_X, (BOTTOM_BAR_H - 28) / 2);
    lv_obj_set_style_bg_color(s_sep_left, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_bg_opa(s_sep_left, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_sep_left, 0, 0);
    lv_obj_set_style_radius(s_sep_left, 0, 0);
    lv_obj_clear_flag(s_sep_left, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 中栏：播放控制 3 图标 (28px) ---- */
    int cx[] = CTRL_ICONS_X;
    s_btn_prev = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_btn_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(s_btn_prev, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(s_btn_prev, FONT_ICON_28, 0);
    lv_obj_set_pos(s_btn_prev, cx[0], icon_y);
    lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_btn_prev, bottom_btn_event_cb, LV_EVENT_ALL, (void *)"prev");

    s_btn_play = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_btn_play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_btn_play, lv_color_make(220, 235, 255), 0);
    lv_obj_set_style_text_font(s_btn_play, FONT_ICON_28, 0);
    lv_obj_set_pos(s_btn_play, cx[1], icon_y);
    lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_btn_play, bottom_btn_event_cb, LV_EVENT_ALL, (void *)"play_pause");

    s_btn_next = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_btn_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(s_btn_next, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(s_btn_next, FONT_ICON_28, 0);
    lv_obj_set_pos(s_btn_next, cx[2], icon_y);
    lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_btn_next, bottom_btn_event_cb, LV_EVENT_ALL, (void *)"next");

    /* ---- 右分隔线 ---- */
    s_sep_right = lv_obj_create(s_bottombar_bg);
    lv_obj_set_size(s_sep_right, 1, 28);
    lv_obj_set_pos(s_sep_right, SEP_R_X, (BOTTOM_BAR_H - 28) / 2);
    lv_obj_set_style_bg_color(s_sep_right, lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_bg_opa(s_sep_right, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_sep_right, 0, 0);
    lv_obj_set_style_radius(s_sep_right, 0, 0);
    lv_obj_clear_flag(s_sep_right, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 右栏：音量 + 下一页 2 图标 (28px) ---- */
    int ux[] = UTIL_ICONS_X;
    s_vol_icon = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(s_vol_icon, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(s_vol_icon, FONT_ICON_28, 0);
    lv_obj_set_pos(s_vol_icon, ux[0], icon_y);

    /* ▶▶ 下一页 — 跳转到 DeepSeek API 用量页面 */
    s_btn_next_page = lv_label_create(s_bottombar_bg);
    lv_label_set_text(s_btn_next_page, LV_SYMBOL_PLAY LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_btn_next_page, lv_color_make(220, 235, 255), 0);
    lv_obj_set_style_text_font(s_btn_next_page, FONT_ICON_28, 0);
    lv_obj_set_pos(s_btn_next_page, ux[1], icon_y);
    lv_obj_add_flag(s_btn_next_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_btn_next_page, on_next_page_click, LV_EVENT_CLICKED, NULL);

    /* ---- 进度条 ---- */
    /* 背景 */
    s_progress_bg = lv_obj_create(parent);
    lv_obj_set_size(s_progress_bg, PROGRESS_BAR_W, PROGRESS_BAR_H);
    lv_obj_set_pos(s_progress_bg, PROGRESS_BAR_X, PROGRESS_BAR_Y);
    lv_obj_set_style_radius(s_progress_bg, 0, 0);
    lv_obj_set_style_border_width(s_progress_bg, 0, 0);
    lv_obj_set_style_bg_color(s_progress_bg, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_bg_opa(s_progress_bg, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_progress_bg, 0, 0);
    lv_obj_clear_flag(s_progress_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* 前景 */
    s_progress_fg = lv_obj_create(parent);
    lv_obj_set_pos(s_progress_fg, PROGRESS_BAR_X, PROGRESS_BAR_Y);
    lv_obj_set_size(s_progress_fg, 0, PROGRESS_BAR_H);
    lv_obj_set_style_radius(s_progress_fg, 0, 0);
    lv_obj_set_style_border_width(s_progress_fg, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fg, lv_color_make(100, 200, 255), 0);
    lv_obj_set_style_bg_opa(s_progress_fg, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_progress_fg, 0, 0);
    lv_obj_clear_flag(s_progress_fg, LV_OBJ_FLAG_SCROLLABLE);
}

/* ================================================================
 *  其他显示模式 UI
 * ================================================================ */

/* ---- Now Playing 模式：居中封面 + 歌名/歌手/专辑 + 大进度条 ---- */
static void create_now_playing(void)
{
    s_np_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_np_container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_np_container, 0, 0);
    lv_obj_set_style_radius(s_np_container, 0, 0);
    lv_obj_set_style_border_width(s_np_container, 0, 0);
    lv_obj_set_style_pad_all(s_np_container, 0, 0);
    lv_obj_set_style_bg_color(s_np_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_np_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_np_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_np_container, LV_OBJ_FLAG_HIDDEN);

    /* 封面占位框 (200×200, 居中偏上) */
    s_np_cover_placeholder = lv_obj_create(s_np_container);
    lv_obj_set_size(s_np_cover_placeholder, 200, 200);
    lv_obj_set_pos(s_np_cover_placeholder, (SCREEN_W - 200) / 2, 80);
    lv_obj_set_style_radius(s_np_cover_placeholder, 12, 0);
    lv_obj_set_style_border_width(s_np_cover_placeholder, 2, 0);
    lv_obj_set_style_border_color(s_np_cover_placeholder, lv_color_make(80, 80, 80), 0);
    lv_obj_set_style_bg_color(s_np_cover_placeholder, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(s_np_cover_placeholder, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_np_cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    /* 封面占位文字 */
    lv_obj_t *cover_hint = lv_label_create(s_np_cover_placeholder);
    lv_label_set_text(cover_hint, LV_SYMBOL_IMAGE "\nCover");
    lv_obj_set_style_text_color(cover_hint, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(cover_hint, &lv_font_montserrat_20, 0);
    lv_obj_center(cover_hint);
    lv_obj_set_style_text_align(cover_hint, LV_TEXT_ALIGN_CENTER, 0);

    /* 歌名 (28px, 封面下方) */
    s_np_title = lv_label_create(s_np_container);
    lv_label_set_text(s_np_title, "");
    lv_obj_set_style_text_color(s_np_title, lv_color_make(240, 240, 240), 0);
    lv_obj_set_style_text_font(s_np_title, FONT_CN_28, 0);
    lv_obj_set_pos(s_np_title, 40, 300);
    lv_obj_set_width(s_np_title, SCREEN_W - 80);
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_DOT);

    /* 歌手 (20px) */
    s_np_artist = lv_label_create(s_np_container);
    lv_label_set_text(s_np_artist, "");
    lv_obj_set_style_text_color(s_np_artist, lv_color_make(160, 160, 160), 0);
    lv_obj_set_style_text_font(s_np_artist, FONT_CN_20, 0);
    lv_obj_set_pos(s_np_artist, 40, 340);
    lv_obj_set_width(s_np_artist, SCREEN_W - 80);
    lv_obj_set_style_text_align(s_np_artist, LV_TEXT_ALIGN_CENTER, 0);

    /* 专辑 (14px) */
    s_np_album = lv_label_create(s_np_container);
    lv_label_set_text(s_np_album, "");
    lv_obj_set_style_text_color(s_np_album, lv_color_make(120, 120, 120), 0);
    lv_obj_set_style_text_font(s_np_album, FONT_ICON_14, 0);
    lv_obj_set_pos(s_np_album, 40, 370);
    lv_obj_set_width(s_np_album, SCREEN_W - 80);
    lv_obj_set_style_text_align(s_np_album, LV_TEXT_ALIGN_CENTER, 0);

    /* 大进度条 */
    s_np_progress = lv_obj_create(s_np_container);
    lv_obj_set_size(s_np_progress, PROGRESS_BAR_W, PROGRESS_BAR_H);
    lv_obj_set_pos(s_np_progress, PROGRESS_BAR_X, PROGRESS_BAR_Y);
    lv_obj_set_style_radius(s_np_progress, 0, 0);
    lv_obj_set_style_border_width(s_np_progress, 0, 0);
    lv_obj_set_style_bg_color(s_np_progress, lv_color_make(100, 200, 255), 0);
    lv_obj_set_style_bg_opa(s_np_progress, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(s_np_progress, 0, 0);
    lv_obj_clear_flag(s_np_progress, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(s_np_progress, 0);
}

/* ---- Visualizer 模式：频谱可视化占位 ---- */
static void create_visualizer(void)
{
    s_viz_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_viz_container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_viz_container, 0, 0);
    lv_obj_set_style_radius(s_viz_container, 0, 0);
    lv_obj_set_style_border_width(s_viz_container, 0, 0);
    lv_obj_set_style_pad_all(s_viz_container, 0, 0);
    lv_obj_set_style_bg_color(s_viz_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_viz_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_viz_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_viz_container, LV_OBJ_FLAG_HIDDEN);

    /* 占位文字 */
    lv_obj_t *hint = lv_label_create(s_viz_container);
    lv_label_set_text(hint, "Visualizer\n\n频谱可视化\n(待 FFT 数据接入)");
    lv_obj_set_style_text_color(hint, lv_color_make(100, 100, 100), 0);
    lv_obj_set_style_text_font(hint, FONT_CN_20, 0);
    lv_obj_center(hint);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- Info 模式：系统信息 ---- */
static void create_info(void)
{
    s_info_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_info_container, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_info_container, 0, 0);
    lv_obj_set_style_radius(s_info_container, 0, 0);
    lv_obj_set_style_border_width(s_info_container, 0, 0);
    lv_obj_set_style_pad_all(s_info_container, 0, 0);
    lv_obj_set_style_bg_color(s_info_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_info_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_info_container, LV_OBJ_FLAG_HIDDEN);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_info_container);
    lv_label_set_text(title, "System Info");
    lv_obj_set_style_text_color(title, lv_color_make(240, 240, 240), 0);
    lv_obj_set_style_text_font(title, FONT_CN_28, 0);
    lv_obj_set_pos(title, 40, 60);

    int y = 120;
    const int line_h = 40;

    /* WiFi IP */
    s_info_ip_label = lv_label_create(s_info_container);
    lv_label_set_text(s_info_ip_label, "IP: --");
    lv_obj_set_style_text_color(s_info_ip_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(s_info_ip_label, FONT_CN_20, 0);
    lv_obj_set_pos(s_info_ip_label, 40, y);  y += line_h;

    /* WiFi SSID */
    s_info_ssid_label = lv_label_create(s_info_container);
    lv_label_set_text(s_info_ssid_label, "WiFi: --");
    lv_obj_set_style_text_color(s_info_ssid_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(s_info_ssid_label, FONT_CN_20, 0);
    lv_obj_set_pos(s_info_ssid_label, 40, y);  y += line_h;

    /* 连接状态 */
    s_info_conn_label = lv_label_create(s_info_container);
    lv_label_set_text(s_info_conn_label, "WS: Disconnected");
    lv_obj_set_style_text_color(s_info_conn_label, lv_color_make(180, 60, 60), 0);
    lv_obj_set_style_text_font(s_info_conn_label, FONT_CN_20, 0);
    lv_obj_set_pos(s_info_conn_label, 40, y);  y += line_h;

    /* 固件版本 */
    s_info_fw_label = lv_label_create(s_info_container);
    lv_label_set_text(s_info_fw_label, "FW: DesktopMusic v0.9");
    lv_obj_set_style_text_color(s_info_fw_label, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(s_info_fw_label, FONT_ICON_14, 0);
    lv_obj_set_pos(s_info_fw_label, 40, y);  y += line_h;

    /* 运行时间 */
    s_info_uptime_label = lv_label_create(s_info_container);
    lv_label_set_text(s_info_uptime_label, "Uptime: 0s");
    lv_obj_set_style_text_color(s_info_uptime_label, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(s_info_uptime_label, FONT_ICON_14, 0);
    lv_obj_set_pos(s_info_uptime_label, 40, y);
}

/* === 专辑封面背景（SD 卡 JPEG → 硬件解码 → PPA 缩放 → RGB565 → lv_img） ===
 *
 * 使用 ESP32-S31 内置硬件 JPEG 解码器 + PPA SRM 引擎：
 *   - 硬件解码 JPEG → RGB565 输出到 PSRAM（2D-DMA 传输，不占 CPU）
 *   - PPA 硬件缩放 ≤160×160 → 内部 DRAM 帧缓冲
 *   - 完全绕过 picolibc FILE 池问题、POSIX read/lseek 等软件栈 */

/* 对齐到 16 像素边界（硬件 JPEG 解码器输出要求） */
#define COVER_ALIGN_16(v)  (((v) + 15) & ~15)

/* 向上圆整到 cache line (64B) 边界，供 esp_cache_msync 使用 */
#define CACHE_ALIGN_UP(s)  (((size_t)(s) + 0x3F) & ~(size_t)0x3F)

/* 硬件解码 SD 卡 JPEG → RGB565 帧缓冲（失败返回 NULL）
 *
 * 流程：读取整个 JPEG → jpeg_decoder_get_info → jpeg_decoder_process
 * → PPA SRM 缩放 → 返回 RGB565 帧缓冲（内部 DRAM，≤51KB）
 *
 * 全尺寸解码输出走 PSRAM（通过 jpeg_alloc_decoder_mem），
 * 最终封面帧缓冲走内部 DRAM（lpgl 图像描述符需要）。 */
static uint8_t *decode_cover_to_rgb565(const char *path, uint16_t *out_w, uint16_t *out_h)
{
    /* ---- 1. 读取整个 JPEG 文件到内存 ---- */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ESP_LOGW(TAG, "封面打开失败: %s", path); return NULL; }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 2) { close(fd); return NULL; }
    uint8_t *jpg_buf = malloc((size_t)sz);
    if (!jpg_buf) { close(fd); return NULL; }
    ssize_t rd = read(fd, jpg_buf, (size_t)sz);
    close(fd);
    if (rd != sz) { free(jpg_buf); return NULL; }

    /* ---- 2. 解析 JPEG 头（纯软件，无需硬件引擎） ---- */
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(jpg_buf, (uint32_t)sz, &info) != ESP_OK) {
        ESP_LOGW(TAG, "封面 JPEG 头解析失败");
        free(jpg_buf); return NULL;
    }
    uint32_t iw = info.width, ih = info.height;
    ESP_LOGD(TAG, "封面: %lu×%lu, 采样=%d, 文件=%lu bytes",
             (unsigned long)iw, (unsigned long)ih, info.sample_method, (unsigned long)sz);

    /* ---- 3. 计算缩放参数（长边 ≤320px，留足全屏背景细节） ---- */
    int scale_log2 = 0;
    while (scale_log2 < 3 && ((iw >> scale_log2) > 320 || (ih >> scale_log2) > 320))
        scale_log2++;
    uint16_t ow = (uint16_t)(iw >> scale_log2);
    uint16_t oh = (uint16_t)(ih >> scale_log2);
    size_t fb_size = (size_t)ow * oh * 2;       /* RGB565 */

    /* ---- 4. 分配全尺寸硬件解码输出缓冲（PSRAM，DMA 对齐） ---- */
    uint32_t padded_w = COVER_ALIGN_16(iw);
    uint32_t padded_h = COVER_ALIGN_16(ih);
    size_t full_res_size = (size_t)padded_w * padded_h * 2;

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t alloc_size = 0;
    uint8_t *full_buf = (uint8_t *)jpeg_alloc_decoder_mem(
        full_res_size, &mem_cfg, &alloc_size);
    if (!full_buf) {
        ESP_LOGE(TAG, "封面解码缓冲分配失败 (%zu bytes)", full_res_size);
        free(jpg_buf); return NULL;
    }

    /* ---- 5. 创建硬件解码引擎 ---- */
    jpeg_decoder_handle_t jpeg_hdl = NULL;
    jpeg_decode_engine_cfg_t eng_cfg = { .timeout_ms = 80 };
    if (jpeg_new_decoder_engine(&eng_cfg, &jpeg_hdl) != ESP_OK) {
        free(jpg_buf); heap_caps_free(full_buf); return NULL;
    }

    /* ---- 6. 硬件解码：JPEG → RGB565 ---- */
    jpeg_decode_cfg_t dec_cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        /* 关键:LVGL 配置为 LV_COLOR_16_SWAP=0(小端 RGB565)。
         * 硬件解码器 JPEG_DEC_RGB_ELEMENT_ORDER_RGB = 大端输出,
         * 与 LVGL 期望相反 → 每个像素 2 字节对调 → 彩色条纹花屏。
         * 改用 BGR = small endian, 与 LV_COLOR_16_SWAP=0 匹配。 */
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded_size = 0;
    esp_err_t err = jpeg_decoder_process(jpeg_hdl, &dec_cfg,
                                         jpg_buf, (uint32_t)sz,
                                         full_buf, (uint32_t)alloc_size,
                                         &decoded_size);
    jpeg_del_decoder_engine(jpeg_hdl);
    free(jpg_buf);          /* 输入数据不再需要 */

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "硬件解码失败: %s", esp_err_to_name(err));
        heap_caps_free(full_buf); return NULL;
    }
    ESP_LOGD(TAG, "硬件解码完成: %lu bytes RGB565", (unsigned long)decoded_size);

    /* 从 decoded_size 反推硬件解码器真实输出布局(dec_w/dec_h)。
     * 解码器按 MCU 块对齐输出：dec_w = iw 上取整到 mcux，dec_h = ih 上取整到 mcuy。
     * 不同采样格式(4:2:0=16x16 / 4:2:2=16x8 / 4:4:4=8x8)的 mcux/mcuy 不同，
     * 导致真实行宽未必等于我们之前假设的 padded_w。从 decoded_size 精确反推，
     * 即可用正确 stride 喂给 PPA，彻底消除"行宽错位 → 斜向彩色条纹"的问题。 */
    uint32_t dec_w = 0, dec_h = 0;
    {
        const uint32_t mcux_cand[4] = {16, 16, 8, 8};
        const uint32_t mcuy_cand[4] = {16, 8, 16, 8};
        for (int i = 0; i < 4; i++) {
            uint32_t ph = (iw + mcux_cand[i] - 1) / mcux_cand[i] * mcux_cand[i];
            uint32_t pv = (ih + mcuy_cand[i] - 1) / mcuy_cand[i] * mcuy_cand[i];
            if (ph * pv * 2 == decoded_size) { dec_w = ph; dec_h = pv; break; }
        }
    }
    if (dec_w == 0) {
        ESP_LOGW(TAG, "无法从 decoded_size=%lu 反推解码布局，回退 padded",
                 (unsigned long)decoded_size);
        dec_w = padded_w; dec_h = padded_h;
    }
    ESP_LOGI(TAG, "封面解码: 原图 %lux%lu, 解码布局(dec_w=%lu,dec_h=%lu, 行stride=%lu B), 输出 %ux%u",
             (unsigned long)iw, (unsigned long)ih,
             (unsigned long)dec_w, (unsigned long)dec_h, (unsigned long)(dec_w * 2),
             (unsigned)ow, (unsigned)oh);

    /* 硬件 DMA 写完 PSRAM 后 CPU cache 可能含旧数据 */
    esp_cache_msync(full_buf, CACHE_ALIGN_UP((size_t)dec_w * dec_h * 2),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    /* ---- 7. 分配最终封面帧缓冲 ----
     * 优先内部 RAM：规避 PSRAM 经 cache 读取时(被 LVGL 绘制读取)潜在的
     * 一致性问题(表现为彩色条纹)。内部 RAM 不足时回退 PSRAM。 */
    uint8_t *fb = (uint8_t *)heap_caps_aligned_calloc(64, 1, fb_size,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (fb) {
        ESP_LOGI(TAG, "封面帧缓冲分配于 内部RAM (%zu bytes)", fb_size);
    } else {
        fb = (uint8_t *)heap_caps_aligned_calloc(64, 1, fb_size,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (fb) ESP_LOGI(TAG, "封面帧缓冲分配于 PSRAM (%zu bytes)", fb_size);
    }
    if (!fb) {
        ESP_LOGE(TAG, "封面帧缓冲分配失败 (%zu bytes)", fb_size);
        heap_caps_free(full_buf);
        return NULL;
    }

    /* ---- 8. 如果需要缩放，用 PPA SRM 硬件缩放；否则直接拷贝 ---- */
    if (iw != ow || ih != oh) {
        ppa_client_handle_t ppa_client = NULL;
        ppa_client_config_t client_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&client_cfg, &ppa_client) != ESP_OK) {
            heap_caps_free(full_buf); heap_caps_free(fb); return NULL;
        }

        /* calloc 写了零（cache dirty）→ 冲刷 dirty cache 行到 PSRAM，
         * 这样 PPA DMA 写 PSRAM 后 cache 里只有 clean 旧数据（好处理） */
        esp_cache_msync(fb, CACHE_ALIGN_UP(fb_size), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        ppa_srm_oper_config_t srm_cfg = {
            .in = {
                .buffer = full_buf,
                .pic_w = dec_w,        /* 硬件解码器真实输出行宽 */
                .pic_h = dec_h,        /* 硬件解码器真实输出行数 */
                .block_w = iw,          /* 有效区域宽度(原始图宽) */
                .block_h = ih,          /* 有效区域高度(原始图高) */
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = fb,
                .buffer_size = fb_size,
                .pic_w = ow,
                .pic_h = oh,
                .block_offset_x = 0,
                .block_offset_y = 0,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)ow / (float)iw,
            .scale_y = (float)oh / (float)ih,
            .byte_swap = false,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };

        err = ppa_do_scale_rotate_mirror(ppa_client, &srm_cfg);
        ppa_unregister_client(ppa_client);

        /* PPA 通过 DMA 写 PSRAM → CPU cache 仍含旧数据（C2M 后的 clean 数据）→ 失效 */
        esp_cache_msync(fb, CACHE_ALIGN_UP(fb_size), ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        heap_caps_free(full_buf);   /* 全尺寸缓冲不再需要 */

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PPA 缩放失败: %s", esp_err_to_name(err));
            heap_caps_free(fb); return NULL;
        }
        ESP_LOGD(TAG, "PPA 缩放: %lu×%lu → %u×%u",
                 (unsigned long)iw, (unsigned long)ih, ow, oh);
    } else {
        /* 原始尺寸已 ≤320px，无需缩放：按解码器真实 stride 逐行紧凑拷贝，
         * 避免 full_buf 行宽(dec_w*2)与 iw*2 不一致导致逐行错位 → 彩色条纹 */
        const uint8_t *src = full_buf;
        uint8_t *dst = fb;
        size_t row_bytes = (size_t)iw * 2;
        for (uint32_t y = 0; y < ih; y++) {
            memcpy(dst, src, row_bytes);
            src += (size_t)dec_w * 2;
            dst += row_bytes;
        }
        heap_caps_free(full_buf);
        /* memcpy 通过 CPU 写入了 fb → 冲刷 cache 行到 PSRAM，确保一致性 */
        esp_cache_msync(fb, CACHE_ALIGN_UP(fb_size), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    /* 诊断：打印前 8 个 RGB565 像素(小端)，确认格式正确、非全零/非乱码 */
    if (ow >= 8) {
        const uint16_t *p = (const uint16_t *)fb;
        ESP_LOGI(TAG, "封面像素样本[0..7]: %04X %04X %04X %04X %04X %04X %04X %04X",
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }

    *out_w = ow;
    *out_h = oh;
    return fb;
}

/* 从解码后的 RGB565 封面帧缓冲提取主色调色板（最多 max_n 个、互异鲜亮色）
 * 返回实际提取数量，0 表示无有效颜色（如全黑/全白封面）。
 * 算法：量化到 512 色桶统计频次 → 远点采样挑选互异颜色，首色为最频主色。 */
static int extract_cover_palette(const uint8_t *fb, uint16_t w, uint16_t h,
                                 uint8_t pal[][3], int max_n)
{
    if (fb == NULL || w == 0 || h == 0 || max_n <= 0) return 0;

    typedef struct { uint32_t sum_r, sum_g, sum_b, cnt; } bucket_t;
    static bucket_t buckets[512];
    memset(buckets, 0, sizeof(buckets));

    const uint16_t *p = (const uint16_t *)fb;
    uint32_t total = (uint32_t)w * h;
    uint32_t step = (total > 20000) ? 4 : 1;   /* 降采样控制开销 */

    for (uint32_t i = 0; i < total; i += step) {
        uint16_t px = p[i];
        /* RGB565（R 在高位 5bit，小端存储）→ 8bit */
        uint8_t r5 = (px >> 11) & 0x1F;
        uint8_t g6 = (px >> 5)  & 0x3F;
        uint8_t b5 = px & 0x1F;
        uint8_t R = (r5 << 3) | (r5 >> 2);
        uint8_t G = (g6 << 2) | (g6 >> 4);
        uint8_t B = (b5 << 3) | (b5 >> 2);

        uint32_t sum = (uint32_t)R + G + B;
        if (sum < 36)  continue;   /* 近黑（暗角/边框） */
        if (sum > 735) continue;   /* 近白（高光/白底） */

        int idx = ((R >> 5) << 6) | ((G >> 5) << 3) | (B >> 5);  /* 每通道 3bit → 512 桶 */
        buckets[idx].sum_r += R;
        buckets[idx].sum_g += G;
        buckets[idx].sum_b += B;
        buckets[idx].cnt++;
    }

    /* 收集有效桶为候选色（桶均值） */
    typedef struct { uint8_t r, g, b; uint32_t cnt; } cand_t;
    cand_t cands[512];
    int nc = 0;
    for (int i = 0; i < 512; i++) {
        if (buckets[i].cnt > 0) {
            cands[nc].r   = (uint8_t)(buckets[i].sum_r / buckets[i].cnt);
            cands[nc].g   = (uint8_t)(buckets[i].sum_g / buckets[i].cnt);
            cands[nc].b   = (uint8_t)(buckets[i].sum_b / buckets[i].cnt);
            cands[nc].cnt = buckets[i].cnt;
            nc++;
        }
    }
    if (nc == 0) return 0;

    /* 远点采样：先选最频者，再依次选与已选集合距离最远者，保证互异 */
    int picked[COVER_PAL_MAX];
    int np = 0;
    int best = 0;
    for (int i = 1; i < nc; i++) if (cands[i].cnt > cands[best].cnt) best = i;
    picked[np++] = best;

    while (np < max_n && np < nc) {
        int far = -1;
        uint32_t far_d = 0;
        for (int i = 0; i < nc; i++) {
            int used = 0;
            for (int k = 0; k < np; k++) if (picked[k] == i) { used = 1; break; }
            if (used) continue;
            uint32_t md = 0xFFFFFFFF;   /* 与已选集合的最小距离 */
            for (int k = 0; k < np; k++) {
                int j = picked[k];
                int dr = (int)cands[i].r - cands[j].r;
                int dg = (int)cands[i].g - cands[j].g;
                int db = (int)cands[i].b - cands[j].b;
                uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
                if (d < md) md = d;
            }
            if (md > far_d) { far_d = md; far = i; }
        }
        if (far < 0) break;
        picked[np++] = far;
    }

    for (int i = 0; i < np; i++) {
        pal[i][0] = cands[picked[i]].r;
        pal[i][1] = cands[picked[i]].g;
        pal[i][2] = cands[picked[i]].b;
    }
    return np;
}

/* 歌词行切换 → 用封面调色板下一色触发脉冲（多色循环，融合几种封面色） */
static void led_trigger_cover_pulse(void)
{
    if (s_cover_palette_n <= 0) {
        app_led_trigger_pulse(LED_SOFT_R, LED_SOFT_G, LED_SOFT_B);
        return;
    }
    uint8_t *c = s_cover_palette[s_cover_palette_idx % s_cover_palette_n];
    s_cover_palette_idx++;
    app_led_trigger_pulse(c[0], c[1], c[2]);
}

/* 无封面时恢复纯黑背景 */
static void app_music_screen_clear_cover(void)
{
    if (!lvgl_port_lock(0)) return;
    if (s_cover_img)    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    if (s_cover_darken) lv_obj_add_flag(s_cover_darken, LV_OBJ_FLAG_HIDDEN);
    if (s_container)    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    if (s_cover_dsc) { heap_caps_free((void *)s_cover_dsc); s_cover_dsc = NULL; }
    if (s_cover_fb)  { heap_caps_free(s_cover_fb); s_cover_fb = NULL; }
    s_cover_palette_n = 0;
    s_cover_palette_idx = 0;
    app_led_effects_set_base_color(LED_SOFT_R, LED_SOFT_G, LED_SOFT_B);
    lvgl_port_unlock();
}

/* 创建封面背景图层（全屏 lv_img + 半透明遮罩），初始隐藏，置于最底层 */
static void create_cover_background(void)
{
    s_cover_img = lv_img_create(lv_scr_act());
    lv_obj_set_pos(s_cover_img, 0, 0);
    lv_obj_set_size(s_cover_img, SCREEN_W, SCREEN_H);
    lv_img_set_size_mode(s_cover_img, LV_IMG_SIZE_MODE_REAL);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_CLICKABLE);

    s_cover_darken = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_cover_darken, 0, 0);
    lv_obj_set_size(s_cover_darken, SCREEN_W, SCREEN_H);
    lv_obj_set_style_radius(s_cover_darken, 0, 0);
    lv_obj_set_style_border_width(s_cover_darken, 0, 0);
    lv_obj_set_style_pad_all(s_cover_darken, 0, 0);
    lv_obj_set_style_bg_color(s_cover_darken, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_cover_darken, LV_OPA_60, 0);
    lv_obj_add_flag(s_cover_darken, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cover_darken, LV_OBJ_FLAG_CLICKABLE);
}

/* 设置专辑封面（path 为 SD 卡上 JPEG；NULL/空 → 清除封面恢复纯黑） */
void app_music_screen_set_cover(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        app_music_screen_clear_cover();
        return;
    }

    uint16_t w = 0, h = 0;
    uint8_t *fb = decode_cover_to_rgb565(path, &w, &h);
    if (fb == NULL) {
        app_music_screen_clear_cover();
        return;
    }

    if (!lvgl_port_lock(0)) {
        heap_caps_free(fb);
        return;
    }

    /* 释放旧封面（描述符 + 帧缓冲） */
    if (s_cover_dsc) { heap_caps_free((void *)s_cover_dsc); s_cover_dsc = NULL; }
    if (s_cover_fb)  { heap_caps_free(s_cover_fb); s_cover_fb = NULL; }

    s_cover_fb = fb;
    s_cover_dsc = (lv_img_dsc_t *)heap_caps_calloc(1, sizeof(lv_img_dsc_t), MALLOC_CAP_INTERNAL);
    if (s_cover_dsc == NULL) {
        heap_caps_free(fb);
        lvgl_port_unlock();
        return;
    }
    s_cover_dsc->header.always_zero = 0;
    s_cover_dsc->header.w = w;
    s_cover_dsc->header.h = h;
    s_cover_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;   /* LVGL8：按当前色彩深度(16bit=RGB565)解释帧缓冲 */
    s_cover_dsc->data = s_cover_fb;
    s_cover_dsc->data_size = (uint32_t)((size_t)w * h * 2);
    lv_img_set_src(s_cover_img, s_cover_dsc);

    /* cover-fit：等比放大至恰好覆盖全屏，溢出部分居中裁切 */
    float zw = (float)SCREEN_W / (float)w;
    float zh = (float)SCREEN_H / (float)h;
    float z = (zw > zh) ? zw : zh;
    int32_t zoom = (int32_t)(z * 256.0f);
    if (zoom < 256) zoom = 256;
    lv_img_set_zoom(s_cover_img, (uint16_t)zoom);

    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cover_darken, LV_OBJ_FLAG_HIDDEN);
    /* 容器透明，露出封面背景 */
    lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);

    lvgl_port_unlock();

    /* 提取封面主色调色板 → 驱动氛围灯（微光基色=主色，歌词脉冲循环取色） */
    s_cover_palette_idx = 0;
    s_cover_palette_n = extract_cover_palette(s_cover_fb, w, h,
                                              s_cover_palette, COVER_PAL_MAX);
    if (s_cover_palette_n > 0) {
        app_led_effects_set_base_color(s_cover_palette[0][0],
                                       s_cover_palette[0][1],
                                       s_cover_palette[0][2]);
    } else {
        app_led_effects_set_base_color(LED_SOFT_R, LED_SOFT_G, LED_SOFT_B);
    }
}

static void create_music_container(void)
{
    /* 先创建封面背景（置于最底层） */
    create_cover_background();

    /* 全屏不透明容器（完全遮挡底层 GIF） */
    s_container = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_size(s_container, SCREEN_W, SCREEN_H);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_set_style_bg_color(s_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);

    /* 子元素直接创建在此容器上 */
    create_top_bar(s_container);
    create_lyrics_area(s_container);
    create_bottom_bar(s_container);

    /* 初始隐藏 */
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_bg) lv_obj_add_flag(s_progress_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_fg) lv_obj_add_flag(s_progress_fg, LV_OBJ_FLAG_HIDDEN);

    /* 创建其他模式容器 */
    create_now_playing();
    create_visualizer();
    create_info();

    ESP_LOGI(TAG, "全屏音乐界面已创建 (800×480, 4 模式)");
}


/* ================================================================
 *  Phase 5: Info 模式动态更新（运行时间 + WiFi 信号强度）
 * ================================================================ */

static lv_timer_t *s_info_timer = NULL;     /**< Info 定时器 */
static uint64_t    s_start_time_us = 0;      /**< 启动时间戳 (us) */

/**
 * @brief LVGL 定时器回调：每 2s 更新 Info 模式的动态信息
 */
static void info_timer_cb(lv_timer_t *timer)
{
    if (!s_visible || s_mode != MUSIC_MODE_INFO || s_info_container == NULL) return;

    /* ---- 运行时间 ---- */
    if (s_start_time_us > 0 && s_info_uptime_label) {
        uint64_t elapsed_us = esp_timer_get_time() - s_start_time_us;
        uint64_t elapsed_s  = elapsed_us / 1000000ULL;

        char uptime_buf[32];
        if (elapsed_s < 60) {
            snprintf(uptime_buf, sizeof(uptime_buf), "Uptime: %llus",
                     (unsigned long long)elapsed_s);
        } else if (elapsed_s < 3600) {
            snprintf(uptime_buf, sizeof(uptime_buf), "Uptime: %llum %llus",
                     (unsigned long long)(elapsed_s / 60),
                     (unsigned long long)(elapsed_s % 60));
        } else {
            snprintf(uptime_buf, sizeof(uptime_buf), "Uptime: %lluh %llum",
                     (unsigned long long)(elapsed_s / 3600),
                     (unsigned long long)((elapsed_s % 3600) / 60));
        }
        lv_label_set_text(s_info_uptime_label, uptime_buf);
    }

    /* ---- IP 地址 + WiFi 信号强度 (RSSI) ---- */
    if (s_wifi_connected && s_info_ip_label) {
        const char *ip = app_network_get_ip();
        int rssi = 0;
        char info_buf[64];
        if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK && ip && ip[0]) {
            snprintf(info_buf, sizeof(info_buf), "IP: %s  |  RSSI: %d dBm", ip, rssi);
        } else if (ip && ip[0]) {
            snprintf(info_buf, sizeof(info_buf), "IP: %s", ip);
        } else {
            snprintf(info_buf, sizeof(info_buf), "IP: --");
        }
        lv_label_set_text(s_info_ip_label, info_buf);
    }
}

/**
 * @brief 初始化 Info 模式动态更新定时器
 */
static void info_timer_init(void)
{
    s_start_time_us = esp_timer_get_time();
    s_info_timer = lv_timer_create(info_timer_cb, 2000, NULL);
    if (s_info_timer != NULL) {
        ESP_LOGI(TAG, "Info 模式动态更新已启动（间隔 2s）");
    }
}


/* ================================================================
 *  公共接口
 * ================================================================ */

esp_err_t app_music_screen_init(void)
{
    if (!lvgl_port_lock(0)) return ESP_FAIL;
    create_music_container();
    lvgl_port_unlock();

    /* Phase 5: 启动 Info 模式动态更新定时器 */
    info_timer_init();

    return ESP_OK;
}

void app_music_screen_show(void)
{
    s_visible = true;
    if (!lvgl_port_lock(0)) return;

    /* 根据当前模式显示对应容器 */
    app_music_screen_set_mode(s_mode);

    /* 强制刷新歌词模式的显示 */
    update_conn_dot();
    update_play_icon();
    update_mode_icon();
    update_progress();
    s_lyrics_current = find_lyrics_line(s_position_ms);
    update_lyrics_display(s_lyrics_current);

    lvgl_port_unlock();
}

void app_music_screen_hide(void)
{
    s_visible = false;
    if (!lvgl_port_lock(0)) return;

    if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_bg) lv_obj_add_flag(s_progress_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_fg) lv_obj_add_flag(s_progress_fg, LV_OBJ_FLAG_HIDDEN);
    if (s_np_container) lv_obj_add_flag(s_np_container, LV_OBJ_FLAG_HIDDEN);
    if (s_viz_container) lv_obj_add_flag(s_viz_container, LV_OBJ_FLAG_HIDDEN);
    if (s_info_container) lv_obj_add_flag(s_info_container, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
}

/* ================================================================
 *  显示模式切换
 * ================================================================ */

void app_music_screen_set_mode(music_display_mode_t mode)
{
    s_mode = mode;

    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;

    /* 先全部隐藏 */
    if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_bg) lv_obj_add_flag(s_progress_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_progress_fg) lv_obj_add_flag(s_progress_fg, LV_OBJ_FLAG_HIDDEN);
    if (s_np_container) lv_obj_add_flag(s_np_container, LV_OBJ_FLAG_HIDDEN);
    if (s_viz_container) lv_obj_add_flag(s_viz_container, LV_OBJ_FLAG_HIDDEN);
    if (s_info_container) lv_obj_add_flag(s_info_container, LV_OBJ_FLAG_HIDDEN);

    /* 只显示目标模式 */
    switch (mode) {
    case MUSIC_MODE_LYRICS:
        if (s_container) {
            lv_obj_move_foreground(s_container);
            lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_progress_bg) lv_obj_clear_flag(s_progress_bg, LV_OBJ_FLAG_HIDDEN);
        if (s_progress_fg) lv_obj_clear_flag(s_progress_fg, LV_OBJ_FLAG_HIDDEN);
        break;
    case MUSIC_MODE_NOW_PLAYING:
        if (s_np_container) {
            lv_obj_move_foreground(s_np_container);
            lv_obj_clear_flag(s_np_container, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case MUSIC_MODE_VISUALIZER:
        if (s_viz_container) {
            lv_obj_move_foreground(s_viz_container);
            lv_obj_clear_flag(s_viz_container, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case MUSIC_MODE_INFO:
        if (s_info_container) {
            lv_obj_move_foreground(s_info_container);
            lv_obj_clear_flag(s_info_container, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    default:
        break;
    }

    lvgl_port_unlock();
    ESP_LOGI(TAG, "显示模式 → %d", (int)mode);
}

music_display_mode_t app_music_screen_get_mode(void)
{
    return s_mode;
}

void app_music_screen_cycle_mode(void)
{
    int next = ((int)s_mode + 1) % MUSIC_MODE_COUNT;
    app_music_screen_set_mode((music_display_mode_t)next);
}

bool app_music_screen_is_visible(void)
{
    return s_visible;
}

void app_music_screen_set_song(const char *title, const char *artist)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "set_song: 锁获取失败!");
        return;
    }

    if (s_title_label) {
        lv_label_set_text(s_title_label,
            (title && strlen(title) > 0) ? title : "No Title");
        /* 强制刷新标签区域 */
        lv_obj_invalidate(s_title_label);
        ESP_LOGI(TAG, "set_song: title='%s' label=%p visible=%d",
                 title ? title : "NULL",
                 (void*)s_title_label,
                 !lv_obj_has_flag(s_title_label, LV_OBJ_FLAG_HIDDEN));
    } else {
        ESP_LOGW(TAG, "set_song: s_title_label 为 NULL!");
    }

    if (s_artist_label) {
        lv_label_set_text(s_artist_label,
            (artist && strlen(artist) > 0) ? artist : "");
        lv_obj_invalidate(s_artist_label);
        ESP_LOGI(TAG, "set_song: artist='%s' label=%p visible=%d",
                 artist ? artist : "NULL",
                 (void*)s_artist_label,
                 !lv_obj_has_flag(s_artist_label, LV_OBJ_FLAG_HIDDEN));
    }

    /* 同步更新 Now Playing 模式 */
    if (s_np_title) {
        lv_label_set_text(s_np_title,
            (title && strlen(title) > 0) ? title : "No Title");
    }
    if (s_np_artist) {
        lv_label_set_text(s_np_artist,
            (artist && strlen(artist) > 0) ? artist : "");
    }

    lvgl_port_unlock();
}

void app_music_screen_set_position(uint32_t position_ms, uint32_t duration_ms)
{
    s_position_ms = position_ms;
    if (duration_ms > 0) s_duration_ms = duration_ms;

    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;

    update_progress();

    /* 歌词行匹配 */
    int new_line = find_lyrics_line(s_position_ms);
    if (new_line != s_lyrics_current) {
        s_lyrics_current = new_line;
        update_lyrics_display(new_line);
    }

    lvgl_port_unlock();
}

void app_music_screen_set_play_state(bool playing)
{
    s_is_playing = playing;
    app_led_effects_set_playing(playing);
    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;
    update_play_icon();
    update_bottom_play_icon();
    lvgl_port_unlock();
}

void app_music_screen_set_play_mode(bool shuffle, const char *repeat_mode)
{
    s_shuffle = shuffle;
    if (repeat_mode) {
        strncpy(s_repeat_mode, repeat_mode, sizeof(s_repeat_mode) - 1);
        s_repeat_mode[sizeof(s_repeat_mode) - 1] = '\0';
    }
    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;
    update_mode_icon();

    /* 底部栏 2 个模式图标高亮 (🔀 🔁) */
    bool rpt = (strcmp(s_repeat_mode, "one") == 0 || strcmp(s_repeat_mode, "all") == 0);

    lv_color_t c_active   = lv_color_make(255, 200, 50);   /* 黄 */
    lv_color_t c_inactive = lv_color_make(140, 140, 140);  /* 灰 */

    if (s_mode_shuffle)
        lv_obj_set_style_text_color(s_mode_shuffle, shuffle ? c_active : c_inactive, 0);
    if (s_mode_repeat)
        lv_obj_set_style_text_color(s_mode_repeat, rpt ? c_active : c_inactive, 0);

    lvgl_port_unlock();
}

void app_music_screen_set_lyrics(const char **texts, const uint32_t *times_ms, int count)
{
    if (!lvgl_port_lock(0)) return;

    /* 释放旧歌词 */
    if (s_lyrics != NULL) {
        for (int i = 0; i < s_lyrics_count; i++) free(s_lyrics[i].text);
        free(s_lyrics);
        s_lyrics = NULL;
    }
    s_lyrics_count = 0;
    s_lyrics_current = -1;

    if (texts == NULL || times_ms == NULL || count <= 0) {
        update_lyrics_display(-1);
        lvgl_port_unlock();
        return;
    }

    /* 分配 */
    s_lyrics = (lyrics_line_t *)malloc(count * sizeof(lyrics_line_t));
    if (s_lyrics == NULL) {
        ESP_LOGE(TAG, "歌词分配失败 (%d 行)", count);
        lvgl_port_unlock();
        return;
    }

    int copied = 0;
    for (int i = 0; i < count; i++) {
        if (texts[i] != NULL) {
            s_lyrics[copied].text = strdup(texts[i]);
            s_lyrics[copied].time_ms = times_ms[i];
            if (s_lyrics[copied].text == NULL) {
                for (int j = 0; j < copied; j++) free(s_lyrics[j].text);
                free(s_lyrics);
                s_lyrics = NULL;
                lvgl_port_unlock();
                return;
            }
            copied++;
        }
    }
    s_lyrics_count = copied;

    s_lyrics_current = find_lyrics_line(s_position_ms);
    update_lyrics_display(s_lyrics_current);

    ESP_LOGI(TAG, "歌词已加载 %d 行", s_lyrics_count);
    lvgl_port_unlock();
}

void app_music_screen_set_conn_state(bool connected)
{
    s_conn_state = connected;
    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;
    update_conn_dot();

    /* 同步更新 Info 模式 */
    if (s_info_conn_label) {
        lv_label_set_text(s_info_conn_label, connected ? "WS: Connected" : "WS: Disconnected");
        lv_obj_set_style_text_color(s_info_conn_label,
            connected ? lv_color_make(0, 200, 0) : lv_color_make(180, 60, 60), 0);
    }

    lvgl_port_unlock();
}

void app_music_screen_set_wifi_state(bool connected, const char *ssid)
{
    s_wifi_connected = connected;
    if (ssid) {
        strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
    }

    if (!s_visible) return;
    if (!lvgl_port_lock(0)) return;

    if (s_wifi_label) {
        char buf[48];
        if (connected && s_wifi_ssid[0] != '\0') {
            snprintf(buf, sizeof(buf), "WiFi:%s   " LV_SYMBOL_OK, s_wifi_ssid);
            lv_obj_set_style_text_color(s_wifi_label,
                lv_color_make(0, 200, 0), 0);
        } else {
            snprintf(buf, sizeof(buf), "WiFi:--   " LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(s_wifi_label,
                lv_color_make(180, 60, 60), 0);
        }
        lv_label_set_text(s_wifi_label, buf);
    }

    /* 同步更新 Info 模式 */
    if (s_info_ssid_label) {
        char info_buf[48];
        if (connected && s_wifi_ssid[0] != '\0') {
            snprintf(info_buf, sizeof(info_buf), "WiFi: %s", s_wifi_ssid);
        } else {
            snprintf(info_buf, sizeof(info_buf), "WiFi: --");
        }
        lv_label_set_text(s_info_ssid_label, info_buf);
    }

    lvgl_port_unlock();
}

