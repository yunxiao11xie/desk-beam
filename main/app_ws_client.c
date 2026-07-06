/**
 * @file app_ws_client.c
 * @brief WebSocket 客户端实现
 *
 * === 架构 ===
 * 整个模块运行在一个 FreeRTOS 任务中（app_ws_task）。
 *
 * 任务主循环：
 *   │
 *   ├─ ① 连接阶段（阻塞等待连接）
 *   │    失败 → 指数退避等待后重试
 *   │
 *   ├─ ② 运行阶段（事件驱动）
 *   │    ├── 收到数据 → JSON 解析 → 分发到 app_music_screen
 *   │    ├── 发送请求 → 从队列中取出发送
 *   │    └── 断线 → 回退到①
 *   │
 *   └─ ③ 停止（清理资源）
 *
 * === 消息协议 ===
 * 见 app_ws_client.h 文档。
 */
#include "app_ws_client.h"
#include "app_music_screen.h"
#include "app_deepseek_screen.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_ws";

/* ---- WebSocket 客户端句柄 ---- */
static esp_websocket_client_handle_t s_client = NULL;
static TaskHandle_t s_task = NULL;

/* ---- 服务器配置 ---- */
static char s_host[64]  = {0};
static uint16_t s_port  = 8765;
static char s_path[32]  = "/";

/* ---- 连接状态 ---- */
static bool s_connected = false;
static bool s_stop_requested = false;

/* ---- 发送命令队列 ---- */
static QueueHandle_t s_cmd_queue = NULL;
#define CMD_QUEUE_LEN  8

/* ---- 连接状态回调 ---- */
static void (*s_conn_cb)(bool connected) = NULL;

/* ---- 连接成功信号量（同步 start→CONNECTED 事件） ---- */
static SemaphoreHandle_t s_conn_sem = NULL;

/* ---- 重连参数 ---- */
#define RECONNECT_BASE_MS   1000
#define RECONNECT_MAX_MS    30000

/* ---- 大消息分片接收缓冲区（歌词 JSON 可能超过默认 8192 字节缓冲区） ---- */
static char  *s_frag_buf   = NULL;
static int    s_frag_len   = 0;
static int    s_frag_total = 0;     /* 总消息长度，从首帧 payload_len 获取；0=未知 */


/* ================================================================
 *  JSON 消息分发
 * ================================================================ */

static void handle_song_info(cJSON *root)
{
    cJSON *title  = cJSON_GetObjectItem(root, "title");
    cJSON *artist = cJSON_GetObjectItem(root, "artist");
    cJSON *state  = cJSON_GetObjectItem(root, "state");
    cJSON *shuffle = cJSON_GetObjectItem(root, "shuffle");
    cJSON *repeat  = cJSON_GetObjectItem(root, "repeat_mode");

    if (cJSON_IsString(title) && cJSON_IsString(artist)) {
        app_music_screen_set_song(title->valuestring, artist->valuestring);
        app_music_screen_show();
        ESP_LOGI(TAG, "♪ %s - %s", title->valuestring, artist->valuestring);
    }

    if (cJSON_IsString(state)) {
        bool playing = (strcmp(state->valuestring, "playing") == 0);
        app_music_screen_set_play_state(playing);
    }

    /* 播放模式（可选） */
    if (cJSON_IsBool(shuffle) || cJSON_IsString(repeat)) {
        app_music_screen_set_play_mode(
            cJSON_IsTrue(shuffle),
            cJSON_IsString(repeat) ? repeat->valuestring : "off"
        );
    }

    /* 位置信息（可选） */
    cJSON *pos = cJSON_GetObjectItem(root, "position_ms");
    cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
    if (cJSON_IsNumber(pos) && cJSON_IsNumber(dur)) {
        app_music_screen_set_position(
            (uint32_t)pos->valuedouble,
            (uint32_t)dur->valuedouble
        );
    }
}

static void handle_lyrics(cJSON *root)
{
    cJSON *lines = cJSON_GetObjectItem(root, "lines");
    if (!cJSON_IsArray(lines)) return;

    int count = cJSON_GetArraySize(lines);
    if (count <= 0) return;

    /* 分配临时数组存储歌词文本和时长 */
    const char **texts = (const char **)malloc(count * sizeof(char *));
    uint32_t *times = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (texts == NULL || times == NULL) {
        free(texts);
        free(times);
        ESP_LOGE(TAG, "歌词分配失败 (%d 行)", count);
        return;
    }

    int actual = 0;
    cJSON *line;
    cJSON_ArrayForEach(line, lines) {
        if (actual >= count) break;
        cJSON *t = cJSON_GetObjectItem(line, "text");
        cJSON *tm = cJSON_GetObjectItem(line, "time_ms");
        if (cJSON_IsString(t) && cJSON_IsNumber(tm)) {
            texts[actual] = t->valuestring;
            times[actual] = (uint32_t)tm->valuedouble;
            actual++;
        }
    }

    if (actual > 0) {
        app_music_screen_set_lyrics(texts, times, actual);
        ESP_LOGI(TAG, "歌词已加载: %d 行", actual);
    }

    free(texts);
    free(times);
}

static void handle_position(cJSON *root)
{
    cJSON *pos   = cJSON_GetObjectItem(root, "position_ms");
    cJSON *dur   = cJSON_GetObjectItem(root, "duration_ms");
    cJSON *state = cJSON_GetObjectItem(root, "state");

    if (cJSON_IsNumber(pos)) {
        app_music_screen_set_position(
            (uint32_t)pos->valuedouble,
            cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 0
        );
    }
    if (cJSON_IsString(state)) {
        app_music_screen_set_play_state(
            strcmp(state->valuestring, "playing") == 0
        );
    }
}

static void handle_server_status(cJSON *root)
{
    cJSON *paused = cJSON_GetObjectItem(root, "paused");
    if (cJSON_IsBool(paused)) {
        app_music_screen_set_play_state(!cJSON_IsTrue(paused));
    }
}

static void handle_deepseek_usage(cJSON *root)
{
    /* WorkBuddy 数据源格式：
     *   balance, currency, monthly_cost, total_requests, total_tokens
     *   daily_usage[] = float
     *   models[] = { name, requests, tokens }
     *   last_sync
     */
    cJSON *balance       = cJSON_GetObjectItem(root, "balance");
    cJSON *currency      = cJSON_GetObjectItem(root, "currency");
    cJSON *monthly_cost  = cJSON_GetObjectItem(root, "monthly_cost");
    cJSON *total_req     = cJSON_GetObjectItem(root, "total_requests");
    cJSON *total_tok     = cJSON_GetObjectItem(root, "total_tokens");
    cJSON *models_arr    = cJSON_GetObjectItem(root, "models");
    cJSON *last_sync     = cJSON_GetObjectItem(root, "last_sync");
    cJSON *daily_arr     = cJSON_GetObjectItem(root, "daily_usage");

    /* 解析模型数组 */
    deepseek_model_t models[DEEPSEEK_MODEL_MAX];
    int model_count = 0;
    uint32_t computed_total_requests = 0;
    uint32_t computed_total_tokens   = 0;

    if (cJSON_IsArray(models_arr)) {
        int n = cJSON_GetArraySize(models_arr);
        if (n > DEEPSEEK_MODEL_MAX) n = DEEPSEEK_MODEL_MAX;
        cJSON *item;
        cJSON_ArrayForEach(item, models_arr) {
            if (model_count >= DEEPSEEK_MODEL_MAX) break;
            cJSON *nm = cJSON_GetObjectItem(item, "name");
            cJSON *rq = cJSON_GetObjectItem(item, "requests");
            cJSON *tk = cJSON_GetObjectItem(item, "tokens");
            if (cJSON_IsString(nm) && cJSON_IsNumber(tk)) {
                snprintf(models[model_count].name, sizeof(models[0].name),
                         "%s", nm->valuestring);
                models[model_count].requests = cJSON_IsNumber(rq)
                    ? (uint32_t)rq->valuedouble : 0;
                models[model_count].tokens   = (uint32_t)tk->valuedouble;
                computed_total_requests += models[model_count].requests;
                computed_total_tokens   += models[model_count].tokens;
                model_count++;
            }
        }
    }

    /* 优先用服务器直接提供的汇总值，否则自己累加 */
    uint32_t total_requests = cJSON_IsNumber(total_req)
        ? (uint32_t)total_req->valuedouble : computed_total_requests;
    uint32_t total_tokens   = cJSON_IsNumber(total_tok)
        ? (uint32_t)total_tok->valuedouble : computed_total_tokens;

    /* 解析 daily_usage 数组 */
    float daily_usage_vals[DAILY_USAGE_MAX_DAYS] = {0};
    int daily_usage_count = 0;
    if (cJSON_IsArray(daily_arr)) {
        int n = cJSON_GetArraySize(daily_arr);
        if (n > DAILY_USAGE_MAX_DAYS) n = DAILY_USAGE_MAX_DAYS;
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(daily_arr, i);
            if (cJSON_IsNumber(item)) {
                daily_usage_vals[i] = (float)item->valuedouble;
                daily_usage_count = i + 1;
            }
        }
    }

    /* 推送数据到 UI */
    app_deepseek_screen_update_data(
        cJSON_IsString(balance) ? balance->valuestring : NULL,
        cJSON_IsString(currency) ? currency->valuestring : NULL,
        cJSON_IsString(monthly_cost) ? monthly_cost->valuestring : NULL,
        total_requests,
        total_tokens,
        daily_usage_count > 0 ? daily_usage_vals : NULL,
        daily_usage_count,
        models, model_count,
        cJSON_IsString(last_sync) ? last_sync->valuestring : NULL
    );

    ESP_LOGI(TAG, "DeepSeek 用量已更新: %d 模型, 请求=%lu, Tokens=%lu",
             model_count,
             (unsigned long)total_requests,
             (unsigned long)total_tokens);
}

static void dispatch_message(const char *data, int len)
{
    /* 复制到以 null 结尾的缓冲区 */
    char *json_str = (char *)malloc(len + 1);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "JSON 分配失败 (%d bytes)", len);
        return;
    }
    memcpy(json_str, data, len);
    json_str[len] = '\0';

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON 解析失败: %s", json_str);
        free(json_str);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        free(json_str);
        return;
    }

    if (strcmp(type->valuestring, "song_info") == 0) {
        handle_song_info(root);
    } else if (strcmp(type->valuestring, "lyrics") == 0) {
        handle_lyrics(root);
    } else if (strcmp(type->valuestring, "position") == 0) {
        handle_position(root);
    } else if (strcmp(type->valuestring, "server_status") == 0) {
        handle_server_status(root);
    } else if (strcmp(type->valuestring, "deepseek_usage") == 0) {
        handle_deepseek_usage(root);
    }

    cJSON_Delete(root);
    free(json_str);
}


/* ================================================================
 *  WebSocket 事件回调（在 esp_websocket_client 任务上下文中调用）
 * ================================================================ */

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *data)
{
    esp_websocket_event_data_t *evt = (esp_websocket_event_data_t *)data;

    switch (event_id) {

    case WEBSOCKET_EVENT_CONNECTED: {
        s_connected = true;
        app_music_screen_set_conn_state(true);
        ESP_LOGI(TAG, "WebSocket 已连接 → ws://%s:%d%s", s_host, s_port, s_path);
        if (s_conn_sem) xSemaphoreGive(s_conn_sem);   /* 唤醒等待连接的 task */
        if (s_conn_cb) s_conn_cb(true);
        break;
    }

    case WEBSOCKET_EVENT_DISCONNECTED: {
        if (s_connected) {
            ESP_LOGW(TAG, "WebSocket 断线");
        }
        s_connected = false;
        app_music_screen_set_conn_state(false);
        /* 清理可能残留的碎片缓冲区 */
        free(s_frag_buf);
        s_frag_buf  = NULL;
        s_frag_len  = 0;
        s_frag_total = 0;
        if (s_conn_cb) s_conn_cb(false);
        break;
    }

    case WEBSOCKET_EVENT_DATA: {
        if (evt->data_len <= 0) break;

        /* 分片消息累积：歌词等大 JSON 可能被分成多个 WEBSOCKET_EVENT_DATA
         *
         * ★ 分片完成判断（重要）：
         *   首选 s_frag_len >= total（精确），
         *   只有在 total=0（首帧 payload_len 未知）时才依赖 evt->fin。
         *   因为 ESP-IDF 某些版本中 evt->fin 在第一块分片就已置 true，
         *   导致提前 dispatch 截断的 JSON。
         */
        if (evt->payload_offset == 0) {
            /* 新消息开始 → 重置缓冲区；记录总大小 */
            if (s_frag_buf != NULL) {
                ESP_LOGW(TAG, "WS 未消费缓冲区被重置 (len=%d, total=%d)", s_frag_len, s_frag_total);
                free(s_frag_buf);
            }
            s_frag_buf   = NULL;
            s_frag_len   = 0;
            s_frag_total = evt->payload_len;
        }

        /* 统一用 realloc 追加数据（抛弃原来的 2 分支逻辑） */
        {
            int new_len = s_frag_len + evt->data_len;
            char *p = (char *)realloc(s_frag_buf, new_len + 1);
            if (p == NULL) {
                ESP_LOGE(TAG, "WS 分片 realloc 失败 (%d bytes)", new_len);
                free(s_frag_buf);
                s_frag_buf   = NULL;
                s_frag_len   = 0;
                s_frag_total = 0;
                dispatch_message(evt->data_ptr, evt->data_len);
                break;
            }
            s_frag_buf = p;
            memcpy(s_frag_buf + s_frag_len, evt->data_ptr, evt->data_len);
            s_frag_len = new_len;
        }

        /* 判断是否所有分片已到齐 */
        bool all_received = false;
        if (s_frag_total > 0) {
            /* 已知总大小：检查已累积的字节数是否到达总大小 */
            all_received = (s_frag_len >= s_frag_total);
        } else {
            /* 总大小未知（首帧 payload_len==0）：依赖 evt->fin */
            all_received = evt->fin;
        }

        if (all_received) {
            s_frag_buf[s_frag_len] = '\0';
            ESP_LOGD(TAG, "WS 消息完整: %d bytes (total=%d, chunks=%d)",
                     s_frag_len, s_frag_total,
                     evt->payload_offset > 0 ? 2 : 1);
            dispatch_message(s_frag_buf, s_frag_len);
            free(s_frag_buf);
            s_frag_buf   = NULL;
            s_frag_len   = 0;
            s_frag_total = 0;
        } else {
            ESP_LOGD(TAG, "WS 分片接收中: %d/%d bytes (off=%llu, fin=%d)",
                     s_frag_len, s_frag_total,
                     (unsigned long long)evt->payload_offset, evt->fin);
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR: {
        ESP_LOGW(TAG, "WebSocket 错误");
        break;
    }

    default:
        break;
    }
}


/* ================================================================
 *  WebSocket 连接日志（调试辅助）
 * ================================================================ */

static void print_conn_status(void)
{
    if (s_client == NULL) {
        ESP_LOGI(TAG, "试图连接 ws://%s:%d%s ...", s_host, s_port, s_path);
    }
}


/* ================================================================
 *  后台任务
 * ================================================================ */

static void app_ws_task(void *arg)
{
    (void)arg;
    uint32_t reconnect_delay = RECONNECT_BASE_MS;
    char uri[128];

    /* ---- 创建命令队列 ---- */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(char *));
    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "命令队列创建失败");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* ---- 创建连接信号量 ---- */
    s_conn_sem = xSemaphoreCreateBinary();
    if (s_conn_sem == NULL) {
        ESP_LOGE(TAG, "连接信号量创建失败");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (!s_stop_requested) {
        /* ---- 1. 构造 URI 并创建客户端 ---- */
        snprintf(uri, sizeof(uri), "ws://%s:%u%s", s_host, s_port, s_path);

        esp_websocket_client_config_t ws_cfg = {
            .uri = uri,
            .task_stack = 16384,
            .task_prio = 4,
            .buffer_size = 16384,           /* 16KB 接收缓冲区，减少歌词 JSON 分片 */
            .network_timeout_ms = 5000,
            .keep_alive_idle   = 10000,
            .keep_alive_interval = 5000,
            .keep_alive_count = 3,
            .disable_auto_reconnect = true,   /* 我们自己控制重连 */
        };

        s_client = esp_websocket_client_init(&ws_cfg);
        if (s_client == NULL) {
            ESP_LOGE(TAG, "WebSocket 客户端创建失败，5s 后重试...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 注册事件回调 */
        ESP_ERROR_CHECK(esp_websocket_register_events(
            s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL
        ));

        /* 清空信号量（防止上一次残留的 give） */
        xSemaphoreTake(s_conn_sem, 0);

        /* ---- 2. 启动并等待 CONNECTED 事件（最多 10s） ---- */
        print_conn_status();
        esp_err_t ret = esp_websocket_client_start(s_client);

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "启动失败 (%s)，%lums 后重试...",
                     esp_err_to_name(ret), (unsigned long)reconnect_delay);
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay));
            reconnect_delay = (reconnect_delay * 2 > RECONNECT_MAX_MS)
                              ? RECONNECT_MAX_MS : reconnect_delay * 2;
            continue;
        }

        /* start() 是异步的，需等待 CONNECTED 事件确认真正连上 */
        if (xSemaphoreTake(s_conn_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
            ESP_LOGW(TAG, "连接超时（10s），%lums 后重试...",
                     (unsigned long)reconnect_delay);
            esp_websocket_client_stop(s_client);
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
            s_connected = false;
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay));
            reconnect_delay = (reconnect_delay * 2 > RECONNECT_MAX_MS)
                              ? RECONNECT_MAX_MS : reconnect_delay * 2;
            continue;
        }

        /* 连接成功 → 重置退避 */
        reconnect_delay = RECONNECT_BASE_MS;

        /* ---- 3. 运行循环：发送命令 + 等待断线 ---- */
        while (!s_stop_requested && esp_websocket_client_is_connected(s_client)) {
            char *cmd = NULL;
            if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (cmd != NULL) {
                    esp_websocket_client_send_text(s_client, cmd, strlen(cmd), pdMS_TO_TICKS(500));
                    free(cmd);
                }
            }
        }

        /* ---- 4. 断开清理 ---- */
        if (s_client != NULL) {
            esp_websocket_client_stop(s_client);
            esp_websocket_client_destroy(s_client);
            s_client = NULL;
        }
        s_connected = false;

        if (!s_stop_requested) {
            ESP_LOGW(TAG, "连接断开，%lums 后重连...", (unsigned long)reconnect_delay);
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay));
            reconnect_delay = (reconnect_delay * 2 > RECONNECT_MAX_MS)
                              ? RECONNECT_MAX_MS : reconnect_delay * 2;
        }
    }

    /* ---- 清理 ---- */
    ESP_LOGI(TAG, "WebSocket 任务退出");

    /* 清空发送队列 */
    char *cmd;
    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        free(cmd);
    }
    vQueueDelete(s_cmd_queue);
    s_cmd_queue = NULL;

    if (s_conn_sem != NULL) {
        vSemaphoreDelete(s_conn_sem);
        s_conn_sem = NULL;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}


/* ================================================================
 *  公共接口
 * ================================================================ */

esp_err_t app_ws_start(const char *host, uint16_t port, const char *path)
{
    if (host == NULL || strlen(host) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_task != NULL) {
        ESP_LOGW(TAG, "WebSocket 客户端已在运行，先停止");
        app_ws_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    s_stop_requested = false;

    /* 保存配置 */
    strncpy(s_host, host, sizeof(s_host) - 1);
    s_port = port;
    if (path != NULL) {
        strncpy(s_path, path, sizeof(s_path) - 1);
    } else {
        s_path[0] = '/';
        s_path[1] = '\0';
    }

    /* 创建后台任务 */
    BaseType_t res = xTaskCreatePinnedToCore(
        app_ws_task, "app_ws", 8192, NULL, 4, &s_task, 0
    );
    if (res != pdPASS) {
        ESP_LOGE(TAG, "WebSocket 任务创建失败");
        s_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebSocket 后台任务已创建");
    return ESP_OK;
}

void app_ws_stop(void)
{
    s_stop_requested = true;

    /* 断开客户端（触发任务退出） */
    if (s_client != NULL) {
        esp_websocket_client_stop(s_client);
    }

    /* 等待任务退出（100ms × 50 = 5s 超时） */
    if (s_task != NULL) {
        for (int i = 0; i < 50 && s_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_task != NULL) {
            ESP_LOGW(TAG, "WebSocket 任务未及时退出，强制删除");
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }

    s_connected = false;
    ESP_LOGI(TAG, "WebSocket 客户端已停止");
}

esp_err_t app_ws_send_command(const char *action)
{
    if (!s_connected || s_cmd_queue == NULL) {
        ESP_LOGW(TAG, "WS 未连接，无法发送命令: %s", action ? action : "NULL");
        return ESP_FAIL;
    }
    if (action == NULL) return ESP_ERR_INVALID_ARG;

    /* 构造 JSON 命令 */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "command");
    cJSON_AddStringToObject(root, "action", action);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json == NULL) return ESP_FAIL;

    /* 入队（复制一份，消息由接收方 free） */
    char *cmd = strdup(json);
    free(json);

    if (cmd == NULL) return ESP_FAIL;

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "命令队列满，丢弃: %s", action);
        free(cmd);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, ">> 命令: %s", action);
    return ESP_OK;
}

esp_err_t app_ws_send_command_ex(const char *action, const char *extra_json)
{
    if (!s_connected || s_cmd_queue == NULL) {
        ESP_LOGW(TAG, "WS 未连接，无法发送命令: %s", action ? action : "NULL");
        return ESP_FAIL;
    }
    if (action == NULL) return ESP_ERR_INVALID_ARG;

    /* 构造 JSON: {"type":"command","action":"<action>"<extra_json>} */
    size_t base_len = strlen(action) + 64;
    size_t extra_len = (extra_json && extra_json[0]) ? strlen(extra_json) : 0;
    size_t total_len = base_len + extra_len;

    char *json = (char *)malloc(total_len);
    if (json == NULL) return ESP_FAIL;

    if (extra_json && extra_json[0]) {
        snprintf(json, total_len,
            "{\"type\":\"command\",\"action\":\"%s\"%s}", action, extra_json);
    } else {
        snprintf(json, total_len,
            "{\"type\":\"command\",\"action\":\"%s\"}", action);
    }

    char *cmd = strdup(json);
    free(json);
    if (cmd == NULL) return ESP_FAIL;

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "命令队列满，丢弃: %s", action);
        free(cmd);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, ">> 命令: %s%s", action, extra_json ? extra_json : "");
    return ESP_OK;
}

esp_err_t app_ws_send_seek(uint32_t position_ms)
{
    char extra[32];
    snprintf(extra, sizeof(extra), ",\"position_ms\":%lu", (unsigned long)position_ms);
    return app_ws_send_command_ex("seek", extra);
}

bool app_ws_is_connected(void)
{
    return s_connected && s_client != NULL
           && esp_websocket_client_is_connected(s_client);
}

void app_ws_set_conn_callback(void (*cb)(bool connected))
{
    s_conn_cb = cb;
}
