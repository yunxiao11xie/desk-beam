/**
 * @file app_deepseek_screen.c
 * @brief DeepSeek API 用量可视化页面 — WorkBuddy 数据源
 *
 * 布局 (800 × 480)：
 *   y=0   导航栏 (48px)            DeepSeek 用量监控 | 更新时间
 *   y=54  统计卡片 (104px)          175|175|135|235 不等宽
 *   y=166 消费金额标题 (16px)       消费金额  CNY 6.60
 *   y=196 日消费柱状图 (114px)      31 天可左右拖动
 *   y=318 模型明细标题 (16px)
 *   y=340 模型明细表头 (26px)
 *   y=368 模型明细数据行 ×2 (52px)  Montserrat 20
 *   y=424 底部栏 (56px)            Refresh | Back to Music
 *
 * 数据源由 WorkBuddy 每 1h 采集自 DeepSeek 平台。
 */
#include "app_deepseek_screen.h"
#include "app_music_screen.h"
#include "app_ws_client.h"

#include "bsp/bsp_board.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "app_deepseek";

/* 中文等宽字体声明（由 font_noto_sc_20.c / font_noto_sc_28.c 提供） */
LV_FONT_DECLARE(font_noto_sc_20);
LV_FONT_DECLARE(font_noto_sc_28);
#define FONT_CN_20   (&font_noto_sc_20)
#define FONT_CN_28   (&font_noto_sc_28)


/* ═══════════════════════════════════════════════════════════════════
 *  颜色常量
 * ═══════════════════════════════════════════════════════════════════ */

#define CLR_BG          lv_color_make(0, 0, 0)
#define CLR_CARD_BG     lv_color_make(20, 20, 25)
#define CLR_CARD_BORDER lv_color_make(40, 40, 45)
#define CLR_CYAN        lv_color_make(80, 220, 255)
#define CLR_PRIMARY     lv_color_make(240, 240, 240)
#define CLR_SECONDARY   lv_color_make(180, 180, 180)
#define CLR_MUTED       lv_color_make(140, 140, 140)
#define CLR_GREEN       lv_color_make(0, 200, 0)
#define CLR_RED         lv_color_make(220, 80, 80)

/** 4 张卡片宽度（不等宽） */
#define CARD_GAP        10
#define CARD_START_X    ((LCD_H_RES - (175 + 175 + 135 + 235 + CARD_GAP * 3)) / 2)

/** 日消费柱状图 */
#define DAILY_COL_W     36      /* 每列宽度 */
#define DAILY_BAR_W     28      /* 柱宽 */
#define DAILY_BAR_H     80      /* 柱最大高度 */
#define DAILY_INNER_W   (DAILY_COL_W * DAILY_USAGE_MAX_DAYS)


/* ═══════════════════════════════════════════════════════════════════
 *  静态对象句柄
 * ═══════════════════════════════════════════════════════════════════ */

static lv_obj_t *s_scr         = NULL;  /* 全屏容器 */
static lv_obj_t *s_placeholder = NULL;  /* 无数据占位 */
static lv_obj_t *s_content     = NULL;  /* 数据内容容器 */

/* 导航栏 */
static lv_obj_t *s_label_sync;

/* 统计卡片 */
static lv_obj_t *s_card_balance_val;
static lv_obj_t *s_card_balance_extra;
static lv_obj_t *s_card_cost_val;
static lv_obj_t *s_card_cost_extra;
static lv_obj_t *s_card_req_val;
static lv_obj_t *s_card_req_extra;
static lv_obj_t *s_card_token_val;
static lv_obj_t *s_card_token_extra;

/* 日消费柱状图 */
static lv_obj_t *s_chart_title;                          /* "消费金额  CNY 6.60" */
static lv_obj_t *s_daily_bars[DAILY_USAGE_MAX_DAYS];     /* 31 根柱 */
static lv_obj_t *s_daily_labels[DAILY_USAGE_MAX_DAYS];   /* 日期数字 1~31 */

/* 模型明细表 — 固定 2 行 */
static lv_obj_t *s_table_name[2];
static lv_obj_t *s_table_req[2];
static lv_obj_t *s_table_tokens[2];
static lv_obj_t *s_table_pct[2];

static bool s_visible = false;


/* ═══════════════════════════════════════════════════════════════════
 *  辅助函数
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief 格式化带逗号的整数，如 "64,653,619"
 */
static void format_with_commas(char *buf, size_t bufsz, uint32_t val)
{
    char tmp[20];
    int n = lv_snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)val);
    if (n <= 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            if (out < (int)bufsz - 1) buf[out++] = ',';
        }
        if (out < (int)bufsz - 1) buf[out++] = tmp[i];
    }
    buf[out] = '\0';
}


/* ═══════════════════════════════════════════════════════════════════
 *  回调函数
 * ═══════════════════════════════════════════════════════════════════ */

static void on_back_click(lv_event_t *e)
{
    (void)e;
    app_deepseek_screen_hide();
    app_music_screen_show();
}

static void on_refresh_click(lv_event_t *e)
{
    (void)e;
    app_ws_send_command("deepseek_refresh");
}


/* ═══════════════════════════════════════════════════════════════════
 *  创建 UI 元素
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 导航栏 ──────────────────────────────────────────────────── */

static void create_nav_bar(lv_obj_t *parent)
{
    /* 背景条 */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_H_RES, 48);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 底部分隔线 */
    lv_obj_t *line = lv_obj_create(bar);
    lv_obj_set_pos(line, 0, 47);
    lv_obj_set_size(line, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(line, lv_color_make(60, 60, 60), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "DeepSeek 用量监控");
    lv_obj_set_style_text_color(title, CLR_PRIMARY, 0);
    lv_obj_set_style_text_font(title, FONT_CN_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    /* 更新时间 */
    s_label_sync = lv_label_create(bar);
    lv_label_set_text(s_label_sync, "Waiting...");
    lv_obj_set_style_text_color(s_label_sync, CLR_MUTED, 0);
    lv_obj_set_style_text_font(s_label_sync, &lv_font_montserrat_14, 0);
    lv_obj_align(s_label_sync, LV_ALIGN_RIGHT_MID, -12, 0);
}


/* ── 统计卡片 ────────────────────────────────────────────────── */

static lv_obj_t *create_one_card(lv_obj_t *parent, lv_coord_t x, lv_coord_t w)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, 0);
    lv_obj_set_size(card, w, 100);
    lv_obj_set_style_bg_color(card, CLR_CARD_BG, 0);
    lv_obj_set_style_border_color(card, CLR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void create_stat_cards(lv_obj_t *parent)
{
    const lv_coord_t widths[4] = {175, 175, 135, 235};
    lv_coord_t x = CARD_START_X;

    /* ── Card 1: 充值余额 ── */
    lv_obj_t *c1 = create_one_card(parent, x, widths[0]);
    s_card_balance_val = lv_label_create(c1);
    lv_label_set_text(s_card_balance_val, "CNY --.--");
    lv_obj_set_style_text_color(s_card_balance_val, CLR_CYAN, 0);
    lv_obj_set_style_text_font(s_card_balance_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_card_balance_val, LV_ALIGN_TOP_MID, 0, 10);

    s_card_balance_extra = lv_label_create(c1);
    lv_label_set_text(s_card_balance_extra, "充值余额");
    lv_obj_set_style_text_color(s_card_balance_extra, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(s_card_balance_extra, FONT_CN_20, 0);
    lv_obj_align(s_card_balance_extra, LV_ALIGN_TOP_MID, 0, 52);
    x += widths[0] + CARD_GAP;

    /* ── Card 2: 月消费 ── */
    lv_obj_t *c2 = create_one_card(parent, x, widths[1]);
    s_card_cost_val = lv_label_create(c2);
    lv_label_set_text(s_card_cost_val, "CNY --.--");
    lv_obj_set_style_text_color(s_card_cost_val, CLR_RED, 0);
    lv_obj_set_style_text_font(s_card_cost_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_card_cost_val, LV_ALIGN_TOP_MID, 0, 10);

    s_card_cost_extra = lv_label_create(c2);
    lv_label_set_text(s_card_cost_extra, "7月消费");
    lv_obj_set_style_text_color(s_card_cost_extra, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(s_card_cost_extra, FONT_CN_20, 0);
    lv_obj_align(s_card_cost_extra, LV_ALIGN_TOP_MID, 0, 52);
    x += widths[1] + CARD_GAP;

    /* ── Card 3: API请求 ── */
    lv_obj_t *c3 = create_one_card(parent, x, widths[2]);
    s_card_req_val = lv_label_create(c3);
    lv_label_set_text(s_card_req_val, "-");
    lv_obj_set_style_text_color(s_card_req_val, CLR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_card_req_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_card_req_val, LV_ALIGN_TOP_MID, 0, 10);

    s_card_req_extra = lv_label_create(c3);
    lv_label_set_text(s_card_req_extra, "总API请求");
    lv_obj_set_style_text_color(s_card_req_extra, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(s_card_req_extra, FONT_CN_20, 0);
    lv_obj_align(s_card_req_extra, LV_ALIGN_TOP_MID, 0, 52);
    x += widths[2] + CARD_GAP;

    /* ── Card 4: 总Tokens ── */
    lv_obj_t *c4 = create_one_card(parent, x, widths[3]);
    s_card_token_val = lv_label_create(c4);
    lv_label_set_text(s_card_token_val, "-");
    lv_obj_set_style_text_color(s_card_token_val, CLR_PRIMARY, 0);
    lv_obj_set_style_text_font(s_card_token_val, &lv_font_montserrat_28, 0);
    lv_obj_align(s_card_token_val, LV_ALIGN_TOP_MID, 0, 10);

    s_card_token_extra = lv_label_create(c4);
    lv_label_set_text(s_card_token_extra, "总Tokens");
    lv_obj_set_style_text_color(s_card_token_extra, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(s_card_token_extra, FONT_CN_20, 0);
    lv_obj_align(s_card_token_extra, LV_ALIGN_TOP_MID, 0, 52);
}


/* ── 日消费柱状图 ────────────────────────────────────────────── */

static void create_daily_chart(lv_obj_t *parent, lv_coord_t y)
{
    /* 可滚动视口 */
    lv_obj_t *chart_cont = lv_obj_create(parent);
    lv_obj_set_pos(chart_cont, 0, y);
    lv_obj_set_size(chart_cont, LCD_H_RES, DAILY_BAR_H + 34);   /* 80+34=114 */
    lv_obj_set_style_bg_color(chart_cont, CLR_BG, 0);
    lv_obj_set_style_border_width(chart_cont, 0, 0);
    lv_obj_set_style_pad_all(chart_cont, 0, 0);
    lv_obj_set_style_radius(chart_cont, 0, 0);
    lv_obj_set_scroll_dir(chart_cont, LV_DIR_HOR);
    lv_obj_add_flag(chart_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(chart_cont, LV_OBJ_FLAG_SCROLL_ONE);

    /* 内层宽内容（31 列） */
    lv_obj_t *inner = lv_obj_create(chart_cont);
    lv_obj_set_pos(inner, 0, 0);
    lv_obj_set_size(inner, DAILY_INNER_W, DAILY_BAR_H + 34);
    lv_obj_set_style_bg_color(inner, CLR_BG, 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_set_style_radius(inner, 0, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);

    /* 每条柱的底部 y 基准（柱从下往上长，底部留空间给日期标签） */
    const lv_coord_t bar_bottom = DAILY_BAR_H + 4;   /* 84 */

    for (int i = 0; i < DAILY_USAGE_MAX_DAYS; i++) {
        lv_coord_t col_x = i * DAILY_COL_W;
        lv_coord_t bar_x = col_x + (DAILY_COL_W - DAILY_BAR_W) / 2;

        /* 柱状条 — 用纯 lv_obj 矩形手动控制高度，确保从底向上生长 */
        s_daily_bars[i] = lv_obj_create(inner);
        lv_obj_set_pos(s_daily_bars[i], bar_x, bar_bottom);   /* 初始高度 0，不可见 */
        lv_obj_set_size(s_daily_bars[i], DAILY_BAR_W, 0);
        lv_obj_set_style_bg_color(s_daily_bars[i], CLR_CYAN, 0);
        lv_obj_set_style_bg_opa(s_daily_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_daily_bars[i], 0, 0);
        lv_obj_set_style_radius(s_daily_bars[i], 3, 0);
        lv_obj_set_style_pad_all(s_daily_bars[i], 0, 0);
        lv_obj_clear_flag(s_daily_bars[i], LV_OBJ_FLAG_SCROLLABLE);

        /* 日期标签 */
        s_daily_labels[i] = lv_label_create(inner);
        lv_label_set_text_fmt(s_daily_labels[i], "%d", i + 1);
        lv_obj_set_style_text_color(s_daily_labels[i], CLR_MUTED, 0);
        lv_obj_set_style_text_font(s_daily_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s_daily_labels[i], col_x, bar_bottom + 2);
        lv_obj_set_size(s_daily_labels[i], DAILY_COL_W, 14);
        lv_obj_set_style_text_align(s_daily_labels[i], LV_TEXT_ALIGN_CENTER, 0);
    }
}


/* ── 模型明细表 ──────────────────────────────────────────────── */

static void create_model_table(lv_obj_t *parent, lv_coord_t start_y)
{
    const lv_coord_t row_h = 26;
    const lv_coord_t hdr_x[] = {40, 180, 310, 600};
    const lv_coord_t hdr_w[] = {140, 130, 270, 80};
    static const char *hdr_text[] = {"模型", "API请求次数", "Tokens", "占比"};

    /* 表头 */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *hdr = lv_label_create(parent);
        lv_label_set_text(hdr, hdr_text[i]);
        lv_obj_set_style_text_color(hdr, CLR_MUTED, 0);
        lv_obj_set_style_text_font(hdr, FONT_CN_20, 0);
        lv_obj_set_pos(hdr, hdr_x[i], start_y);
        lv_obj_set_size(hdr, hdr_w[i], LV_SIZE_CONTENT);
    }

    /* 2 行数据（Montserrat 20） */
    for (int i = 0; i < 2; i++) {
        lv_coord_t ry = start_y + row_h + 4 + i * (row_h + 2);

        s_table_name[i] = lv_label_create(parent);
        lv_label_set_text(s_table_name[i], "-");
        lv_obj_set_style_text_color(s_table_name[i], CLR_SECONDARY, 0);
        lv_obj_set_style_text_font(s_table_name[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_table_name[i], hdr_x[0], ry);
        lv_obj_set_size(s_table_name[i], hdr_w[0], LV_SIZE_CONTENT);

        s_table_req[i] = lv_label_create(parent);
        lv_label_set_text(s_table_req[i], "");
        lv_obj_set_style_text_color(s_table_req[i], CLR_PRIMARY, 0);
        lv_obj_set_style_text_font(s_table_req[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_table_req[i], hdr_x[1], ry);
        lv_obj_set_size(s_table_req[i], hdr_w[1], LV_SIZE_CONTENT);

        s_table_tokens[i] = lv_label_create(parent);
        lv_label_set_text(s_table_tokens[i], "");
        lv_obj_set_style_text_color(s_table_tokens[i], CLR_PRIMARY, 0);
        lv_obj_set_style_text_font(s_table_tokens[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_table_tokens[i], hdr_x[2], ry);
        lv_obj_set_size(s_table_tokens[i], hdr_w[2], LV_SIZE_CONTENT);

        s_table_pct[i] = lv_label_create(parent);
        lv_label_set_text(s_table_pct[i], "");
        lv_obj_set_style_text_color(s_table_pct[i], CLR_PRIMARY, 0);
        lv_obj_set_style_text_font(s_table_pct[i], &lv_font_montserrat_20, 0);
        lv_obj_set_pos(s_table_pct[i], hdr_x[3], ry);
        lv_obj_set_size(s_table_pct[i], hdr_w[3], LV_SIZE_CONTENT);

        lv_obj_add_flag(s_table_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_req[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_tokens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_pct[i], LV_OBJ_FLAG_HIDDEN);
    }
}


/* ── 底部栏 ──────────────────────────────────────────────────── */

static void create_bottom_bar(lv_obj_t *parent)
{
    /* 背景条 */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_H_RES, 56);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部分隔线 */
    lv_obj_t *line = lv_obj_create(bar);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_size(line, LCD_H_RES, 1);
    lv_obj_set_style_bg_color(line, CLR_CARD_BORDER, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_30, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);

    /* Refresh 按钮 */
    lv_obj_t *btn_refresh = lv_btn_create(bar);
    lv_obj_set_pos(btn_refresh, 20, 8);
    lv_obj_set_size(btn_refresh, 110, 40);
    lv_obj_set_style_bg_color(btn_refresh, lv_color_make(40, 40, 45), 0);
    lv_obj_set_style_border_width(btn_refresh, 0, 0);
    lv_obj_add_event_cb(btn_refresh, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rflbl = lv_label_create(btn_refresh);
    lv_label_set_text(rflbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_color(rflbl, CLR_PRIMARY, 0);
    lv_obj_set_style_text_font(rflbl, &lv_font_montserrat_14, 0);
    lv_obj_center(rflbl);

    /* ♫ 返回音乐按钮 */
    lv_obj_t *btn_back = lv_btn_create(bar);
    lv_obj_set_pos(btn_back, 640, 8);
    lv_obj_set_size(btn_back, 140, 40);
    lv_obj_set_style_bg_color(btn_back, lv_color_make(40, 40, 45), 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, on_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *blbl = lv_label_create(btn_back);
    lv_label_set_text(blbl, LV_SYMBOL_PLAY " Back to Music");
    lv_obj_set_style_text_color(blbl, CLR_CYAN, 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
    lv_obj_center(blbl);
}


/* ── 占位符 ──────────────────────────────────────────────────── */

static void create_placeholder(lv_obj_t *parent)
{
    s_placeholder = lv_obj_create(parent);
    lv_obj_set_size(s_placeholder, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_placeholder, 0, 48);
    lv_obj_set_size(s_placeholder, LCD_H_RES, LCD_V_RES - 48 - 56);
    lv_obj_set_style_bg_color(s_placeholder, CLR_BG, 0);
    lv_obj_set_style_border_width(s_placeholder, 0, 0);
    lv_obj_clear_flag(s_placeholder, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(s_placeholder);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(icon, CLR_MUTED, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *msg = lv_label_create(s_placeholder);
    lv_label_set_text(msg, "等待数据同步...");
    lv_obj_set_style_text_color(msg, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(msg, FONT_CN_20, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *hint = lv_label_create(s_placeholder);
    lv_label_set_text(hint, "请确保 PC 端 WorkBuddy 正在运行");
    lv_obj_set_style_text_color(hint, CLR_MUTED, 0);
    lv_obj_set_style_text_font(hint, FONT_CN_20, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 36);

    /* ◀ 返回按钮 — 无数据时也能回到音乐界面 */
    lv_obj_t *btn_back = lv_btn_create(s_placeholder);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_size(btn_back, 200, 40);
    lv_obj_set_style_bg_color(btn_back, lv_color_make(40, 40, 45), 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, on_back_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *blbl = lv_label_create(btn_back);
    lv_label_set_text(blbl, LV_SYMBOL_LEFT " Back to Music");
    lv_obj_set_style_text_color(blbl, CLR_CYAN, 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_20, 0);
    lv_obj_center(blbl);
}


/* ═══════════════════════════════════════════════════════════════════
 *  公共 API
 * ═══════════════════════════════════════════════════════════════════ */

esp_err_t app_deepseek_screen_init(void)
{
    lvgl_port_lock(0);

    /* ---- 全屏根容器 ---- */
    s_scr = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_scr, LCD_H_RES, LCD_V_RES);
    lv_obj_set_pos(s_scr, 0, 0);
    lv_obj_set_style_bg_color(s_scr, CLR_BG, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 导航栏 y=0（始终可见） ---- */
    create_nav_bar(s_scr);

    /* ---- 占位层（无数据时可见） ---- */
    create_placeholder(s_scr);

    /* ---- 数据内容层（有数据后才显示） ---- */
    s_content = lv_obj_create(s_scr);
    lv_obj_set_pos(s_content, 0, 48);
    lv_obj_set_size(s_content, LCD_H_RES, LCD_V_RES - 48 - 56);
    lv_obj_set_style_bg_color(s_content, CLR_BG, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 统计卡片 y=6（abs 54） ---- */
    lv_obj_t *cards_cont = lv_obj_create(s_content);
    lv_obj_set_pos(cards_cont, 0, 6);
    lv_obj_set_size(cards_cont, LCD_H_RES, 104);
    lv_obj_set_style_bg_color(cards_cont, CLR_BG, 0);
    lv_obj_set_style_border_width(cards_cont, 0, 0);
    lv_obj_set_style_pad_all(cards_cont, 0, 0);
    lv_obj_clear_flag(cards_cont, LV_OBJ_FLAG_SCROLLABLE);
    create_stat_cards(cards_cont);

    /* ---- 消费金额标题 y=118（abs 166） ---- */
    s_chart_title = lv_label_create(s_content);
    lv_label_set_text(s_chart_title, "消费金额  --");
    lv_obj_set_style_text_color(s_chart_title, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(s_chart_title, FONT_CN_20, 0);
    lv_obj_set_pos(s_chart_title, 40, 118);

    /* ---- 日消费柱状图 y=148（abs 196） ---- */
    create_daily_chart(s_content, 148);

    /* ---- 模型明细标题 y=270（abs 318） ---- */
    lv_obj_t *tbl_title = lv_label_create(s_content);
    lv_label_set_text(tbl_title, "模型明细");
    lv_obj_set_style_text_color(tbl_title, CLR_SECONDARY, 0);
    lv_obj_set_style_text_font(tbl_title, FONT_CN_20, 0);
    lv_obj_set_pos(tbl_title, 40, 270);

    /* ---- 模型明细表 y=292（abs 340） ---- */
    create_model_table(s_content, 292);

    /* ---- 底部栏 y=424（始终可见） ---- */
    lv_obj_t *bottom_cont = lv_obj_create(s_scr);
    lv_obj_set_pos(bottom_cont, 0, 424);
    lv_obj_set_size(bottom_cont, LCD_H_RES, 56);
    lv_obj_set_style_bg_color(bottom_cont, CLR_BG, 0);
    lv_obj_set_style_border_width(bottom_cont, 0, 0);
    lv_obj_set_style_pad_all(bottom_cont, 0, 0);
    lv_obj_clear_flag(bottom_cont, LV_OBJ_FLAG_SCROLLABLE);
    create_bottom_bar(bottom_cont);

    /* 初始隐藏 */
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "DeepSeek 页面初始化完成");
    return ESP_OK;
}


void app_deepseek_screen_show(void)
{
    if (s_visible) return;
    lvgl_port_lock(0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "DeepSeek 页面显示");
}

void app_deepseek_screen_hide(void)
{
    if (!s_visible) return;
    lvgl_port_lock(0);
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "DeepSeek 页面隐藏");
}

bool app_deepseek_screen_is_visible(void)
{
    return s_visible;
}


void app_deepseek_screen_update_data(
    const char    *balance,
    const char    *currency,
    const char    *monthly_cost,
    uint32_t       total_requests,
    uint32_t       total_tokens,
    const float   *daily_usage,
    int            daily_usage_count,
    const deepseek_model_t *models,
    int            model_count,
    const char    *last_sync)
{
    lvgl_port_lock(0);

    /* ---- 移除占位符，显示内容 ---- */
    if (s_content && lv_obj_has_flag(s_content, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_placeholder, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    /* ---- 更新时间 ---- */
    if (last_sync != NULL) {
        lv_label_set_text(s_label_sync, last_sync);
    }

    /* ---- 统计卡片 ---- */

    /* 卡 1: 余额 */
    if (balance != NULL && currency != NULL) {
        lv_snprintf(buf, sizeof(buf), "CNY %s", balance);
        lv_label_set_text(s_card_balance_val, buf);
        lv_snprintf(buf, sizeof(buf), "充值余额  %s", currency);
        lv_label_set_text(s_card_balance_extra, buf);
        lv_obj_set_style_text_color(s_card_balance_extra, CLR_GREEN, 0);
    } else {
        lv_label_set_text(s_card_balance_val, "CNY N/A");
        lv_label_set_text(s_card_balance_extra, "充值余额");
        lv_obj_set_style_text_color(s_card_balance_extra, CLR_RED, 0);
    }

    /* 卡 2: 月消费 */
    if (monthly_cost != NULL) {
        lv_snprintf(buf, sizeof(buf), "CNY %s", monthly_cost);
        lv_label_set_text(s_card_cost_val, buf);
    } else {
        lv_label_set_text(s_card_cost_val, "CNY --");
    }
    lv_label_set_text(s_card_cost_extra, "7月消费");
    lv_obj_set_style_text_color(s_card_cost_extra, CLR_SECONDARY, 0);

    /* 卡 3: API请求 */
    lv_snprintf(buf, sizeof(buf), "%lu", (unsigned long)total_requests);
    lv_label_set_text(s_card_req_val, buf);
    lv_label_set_text(s_card_req_extra, "总API请求");

    /* 卡 4: 总Tokens */
    format_with_commas(buf, sizeof(buf), total_tokens);
    lv_label_set_text(s_card_token_val, buf);
    lv_label_set_text(s_card_token_extra, "总Tokens");

    /* ---- 消费金额标题 ---- */
    if (monthly_cost != NULL) {
        lv_snprintf(buf, sizeof(buf), "消费金额  CNY %s", monthly_cost);
        lv_label_set_text(s_chart_title, buf);
    }

    /* ---- 日消费柱状图 ---- */
    if (daily_usage != NULL && daily_usage_count > 0) {
        /* 找最大值用于归一化 */
        float max_cost = 0.0f;
        int n = (daily_usage_count > DAILY_USAGE_MAX_DAYS)
                    ? DAILY_USAGE_MAX_DAYS : daily_usage_count;
        for (int i = 0; i < n; i++) {
            if (daily_usage[i] > max_cost) max_cost = daily_usage[i];
        }

        const lv_coord_t bar_bottom = DAILY_BAR_H + 4;

        for (int i = 0; i < DAILY_USAGE_MAX_DAYS; i++) {
            lv_coord_t bar_h = 0;
            if (max_cost > 0.0f && i < n && daily_usage[i] > 0.0f) {
                int pct = (int)(daily_usage[i] / max_cost * 100.0f);
                if (pct > 100) pct = 100;
                if (pct < 1)   pct = 1;
                bar_h = (lv_coord_t)((int64_t)DAILY_BAR_H * pct / 100);
                if (bar_h > DAILY_BAR_H) bar_h = DAILY_BAR_H;
                if (bar_h < 1)           bar_h = 1;
            }

            lv_obj_set_size(s_daily_bars[i], DAILY_BAR_W, bar_h);
            lv_obj_set_pos(s_daily_bars[i],
                           (i * DAILY_COL_W) + (DAILY_COL_W - DAILY_BAR_W) / 2,
                           bar_bottom - bar_h);
            lv_obj_set_style_bg_opa(s_daily_bars[i],
                                    bar_h > 0 ? LV_OPA_COVER : LV_OPA_0, 0);
        }
    } else {
        /* 无数据，全部置零 */
        const lv_coord_t bar_bottom = DAILY_BAR_H + 4;
        for (int i = 0; i < DAILY_USAGE_MAX_DAYS; i++) {
            lv_obj_set_size(s_daily_bars[i], DAILY_BAR_W, 0);
            lv_obj_set_pos(s_daily_bars[i],
                           (i * DAILY_COL_W) + (DAILY_COL_W - DAILY_BAR_W) / 2,
                           bar_bottom);
            lv_obj_set_style_bg_opa(s_daily_bars[i], LV_OPA_0, 0);
        }
    }

    /* ---- 模型明细表 ---- */
    /* 隐藏旧行 */
    for (int i = 0; i < 2; i++) {
        lv_obj_add_flag(s_table_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_req[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_tokens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_table_pct[i], LV_OBJ_FLAG_HIDDEN);
    }

    int n = (model_count > 2) ? 2 : model_count;
    for (int i = 0; i < n; i++) {
        /* 提取短名称 */
        const char *short_name = models[i].name;
        if (strncmp(short_name, "deepseek-", 9) == 0) {
            short_name += 9;
        }
        lv_label_set_text(s_table_name[i], short_name);

        lv_snprintf(buf, sizeof(buf), "%lu", (unsigned long)models[i].requests);
        lv_label_set_text(s_table_req[i], buf);

        format_with_commas(buf, sizeof(buf), models[i].tokens);
        lv_label_set_text(s_table_tokens[i], buf);

        lv_snprintf(buf, sizeof(buf), "%.2f%%",
                    (total_tokens > 0)
                        ? (double)models[i].tokens * 100.0 / total_tokens
                        : 0.0);
        lv_label_set_text(s_table_pct[i], buf);

        lv_obj_clear_flag(s_table_name[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_table_req[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_table_tokens[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_table_pct[i], LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
}
