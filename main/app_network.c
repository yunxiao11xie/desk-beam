/**
 * @file app_network.c
 * @brief WiFi 站模式实现
 *
 * === 设计要点 ===
 * - 使用 FreeRTOS 事件组同步连接状态（免轮询）
 * - 事件处理器通过系统事件循环接收 WiFi/IP 事件
 * - 自动重连：ESP-IDF 的 wifi_station 模块默认开启重连
 * - 线程安全：IP 字符串通过静态缓冲 + 事件排他访问
 *
 * === 事件组比特位 ===
 * - BIT0: WiFi 已连接（GOT_IP）
 * - BIT1: 连接失败（断连或 DHCP 超时）
 */
#include "app_network.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "app_network";

/* ---- 事件组：同步连接/断开 ---- */
static EventGroupHandle_t s_net_events = NULL;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* ---- 当前 IP（静态缓冲，事件处理器中更新） ---- */
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected   = false;
static bool s_initialized = false;

/* ---- 当前 SSID/PWD（重连时使用） ---- */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};


/* ================================================================
 *  WiFi / IP 事件处理器
 * ================================================================ */

static void network_event_handler(void *arg, esp_event_base_t base,
                                   int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA 已启动，开始连接...");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        strcpy(s_ip_str, "0.0.0.0");

        xEventGroupClearBits(s_net_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_net_events, WIFI_FAIL_BIT);

        /* 显式重连（wifi_config_t.auto_reconnect 默认 false，需手动调用） */
        if (s_ssid[0] != '\0') {
            ESP_LOGW(TAG, "WiFi 断开，正在重连...");
            esp_wifi_connect();
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip_str, sizeof(s_ip_str));
        s_connected = true;

        ESP_LOGI(TAG, "=== WiFi 已连接，IP: %s ===", s_ip_str);
        xEventGroupClearBits(s_net_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_net_events, WIFI_CONNECTED_BIT);
    }
}


/* ================================================================
 *  公共接口
 * ================================================================ */

esp_err_t app_network_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "WiFi 已初始化，跳过");
        return ESP_OK;
    }

    /* ---- 1. 创建事件组 ---- */
    s_net_events = xEventGroupCreate();
    if (s_net_events == NULL) {
        ESP_LOGE(TAG, "事件组创建失败");
        return ESP_FAIL;
    }

    /* ---- 2. NVS 初始化（WiFi 需要存储配置） ---- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除，正在执行...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS 初始化失败");

    /* ---- 3. 创建 netif + 事件循环 ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* ---- 4. 注册事件处理器 ---- */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,    ESP_EVENT_ANY_ID,  &network_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,      IP_EVENT_STA_GOT_IP, &network_event_handler, NULL));

    /* ---- 5. 初始化 WiFi ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi 站模式已初始化");
    return ESP_OK;
}

esp_err_t app_network_connect(const char *ssid, const char *password)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "WiFi 未初始化，请先调用 app_network_init()");
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID 不能为空");
        return ESP_ERR_INVALID_ARG;
    }

    /* 保存凭据 */
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (password != NULL) {
        strncpy(s_pass, password, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
    } else {
        s_pass[0] = '\0';
    }

    /* 配置并连接 */
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, s_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (s_pass[0] != '\0') {
        strncpy((char *)wifi_cfg.sta.password, s_pass, sizeof(wifi_cfg.sta.password) - 1);
    }

    ESP_LOGI(TAG, "正在连接 WiFi: %s...", s_ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t app_network_wait_connected(TickType_t timeout_ticks)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_net_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,     /* 退出时清除位，让下次调用能重新等待 */
        pdFALSE,    /* 任一比特满足即可 */
        timeout_ticks
    );

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "WiFi 连接超时（%lu ticks）", (unsigned long)timeout_ticks);
    return ESP_FAIL;
}

void app_network_disconnect(void)
{
    if (!s_initialized) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_connected = false;
    strcpy(s_ip_str, "0.0.0.0");
    ESP_LOGI(TAG, "WiFi 已断开");
}

const char *app_network_get_ip(void)
{
    return s_ip_str;
}

bool app_network_is_connected(void)
{
    return s_connected;
}

void app_network_reconnect(void)
{
    if (!s_initialized) return;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    ESP_LOGI(TAG, "正在重新连接 WiFi...");
}
