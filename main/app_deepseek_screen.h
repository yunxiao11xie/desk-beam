/**
 * @file app_deepseek_screen.h
 * @brief DeepSeek API 用量可视化页面 — 全屏覆盖层 (WorkBuddy 数据源)
 *
 * 布局 (800×480)：
 *   y=0   导航栏              DeepSeek 用量监控 | 更新时间
 *   y=54  统计卡片 (104px)      余额 / 月消费 / API请求 / 总Tokens
 *   y=166 消费金额标题          消费金额  CNY 6.60
 *   y=196 日消费柱状图 (114px)   31 天可左右拖动
 *   y=318 模型明细             表头 + 2 行 (Montserrat 20)
 *   y=424 底部栏               Refresh | Back to Music
 *
 * 通过音乐屏幕右下角的"►►"按钮进入，底部栏"Back to Music"返回。
 * 数据由 app_ws_client 收到 deepseek_usage 后调用 update_data() 推送。
 * 所有 LVGL 操作内部持有锁，调用者无需额外同步。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 模型用量 */
typedef struct {
    char   name[28];        /**< "deepseek-v4-flash" */
    uint32_t requests;       /**< API 请求次数 */
    uint32_t tokens;         /**< Token 消耗量 */
} deepseek_model_t;

/** @brief 最大模型数 */
#define DEEPSEEK_MODEL_MAX     2

/** @brief 每日消费最大天数 */
#define DAILY_USAGE_MAX_DAYS   31

/**
 * @brief 初始化 DeepSeek 页面
 *
 * 创建所有 LVGL 对象，初始为隐藏状态。
 *
 * @return ESP_OK 成功
 */
esp_err_t app_deepseek_screen_init(void);

/**
 * @brief 显示 / 隐藏 DeepSeek 页面
 *
 * 页面显示时完全覆盖音乐屏幕（800×480 全屏黑色背景）。
 */
void app_deepseek_screen_show(void);
void app_deepseek_screen_hide(void);
bool app_deepseek_screen_is_visible(void);

/**
 * @brief 更新 DeepSeek API 用量数据 (WorkBuddy 格式)
 *
 * 由 app_ws_client dispatch_message() 收到 deepseek_usage 后调用。
 * 内部持有 LVGL 锁，可在任何上下文中调用。
 *
 * @param balance        余额字符串，如 "2.45"
 * @param currency        货币单位，如 "CNY"
 * @param monthly_cost    本月消费字符串，如 "4.92"
 * @param total_requests  总 API 请求次数
 * @param total_tokens    总 Token 消耗量
 * @param daily_usage     每日消费金额数组，长度 daily_usage_count；NULL 表示无数据
 * @param daily_usage_count 有效天数 (0~31)
 * @param models          各模型用量数组
 * @param model_count     有效模型数 (1~DEEPSEEK_MODEL_MAX)
 * @param last_sync       更新时间字符串，如 "2026-07-05 17:46:16"
 */
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
    const char    *last_sync
);

#ifdef __cplusplus
}
#endif
