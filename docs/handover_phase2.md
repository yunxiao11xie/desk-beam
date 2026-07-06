# 桌面音乐伴侣 — 项目交接文档

> **版本**: v0.13 (Phase 6b WorkBuddy 数据源 + DeepSeek 页面重设计)
> **日期**: 2026-07-06
> **硬件**: ESP32-S3-Korvo-2-LCD (ESP32-S31 RISC-V, 16MB PSRAM, 16MB Flash)

---

## 目录

1. [项目概况](#1-项目概况)
2. [硬件环境](#2-硬件环境)
3. [项目结构](#3-项目结构)
4. [模块详解](#4-模块详解)
5. [构建与烧录](#5-构建与烧录)
6. [通信协议](#6-通信协议)
7. [开发路线图](#7-开发路线图)
8. [已知问题与风险](#8-已知问题与风险)
9. [快速排错](#9-快速排错)
10. [v0.3 变更记录](#10-v03-变更记录)

---

## 1. 项目概况

### 1.1 这是什么

一个**桌面音乐伴侣**：PC 用 QQ音乐/网易云等播放器放歌，ESP32 板子通过 WiFi + WebSocket 实时读取歌曲信息和歌词，在 800×480 大屏上显示歌词，WS2812 LED 做氛围灯。

### 1.2 核心理念

```
PC 端    → 管"数据源 + 控制执行" (SMTC 监听 + 多源歌词: NetEase → LRCLIB + 播放控制)
ESP32    → 管"显示 + 交互 + 氛围" (全屏歌词 + 按键 + RGB 灯 + 中文显示)
```

### 1.3 当前状态

| 阶段 | 状态 | 内容 |
|------|------|------|
| **Phase 1** | ✅ 完成 | PC端 `windows_media_server.py`：SMTC 读歌、多源歌词 (NetEase + LRCLIB)、WebSocket 广播 |
| **Phase 2** | ✅ 完成 | ESP32 WiFi + WebSocket + 全屏歌词 UI + 按键控制 + 中文字体；GIF demo 已删除 |
| **Phase 3** | ✅ 完成 | 顶部状态栏 ✅；歌词列表 ✅；UI 布局/符号/高亮 ✅；歌词滚动动画 ✅；触摸热区 ✅；多模式切换 ✅ |
| **Phase 1.5 (字体迁移)** | ✅ 回退 | FreeType + TTF 方案已移除，回归嵌入式 Bitmap 字库方案 |
| **Phase 4** | ✅ 完成 | WS2812 氛围灯效果引擎：脉冲（歌词切换触发）/ 呼吸 / 彩虹 / 关，全局亮度调节，播放状态感知 |
| **Phase 5** | ✅ 完成 | 按键映射完善 + 歌词点击跳转 + Info 动态更新 |
| **Phase 6a** | ⚠️ 待验证 | 语音命令 (ESP-SR 在 RISC-V 上的兼容性待验证) |
| **Phase 6b** | ✅ 完成 | SD 卡集成 (SDMMC 4-bit) + DeepSeek API 用量可视化 (WorkBuddy 文件数据源 → 4卡片不等宽 + 日消费柱状图可拖动 + 模型明细表 Montserrat 20) |

---

## 2. 硬件环境

### 2.1 板卡

**ESP32-S3-Korvo-2-LCD**

| 硬件 | 本项目用途 | 驱动状态 |
|------|-----------|---------|
| **LCD** 800×480 RGB (ST7262E43) | 全屏歌词 + 状态栏 + 底部控制栏 | ✅ 已有 (RGB565 并行) |
| **触摸** GT1151 (I2C, GPIO 0/1) | 将来：点击/滑动交互 | ✅ 已有驱动，未用 |
| **WS2812** RGB LED (GPIO 37) | 将来：氛围灯 | ✅ 已有驱动 |
| **ADC 按键** (GPIO 42) | SET/MODE/VOL-/VOL+ 控制 | ✅ 已有驱动 |
| **ES7210 + 双麦克风** | 将来：语音命令 (GPIO 9/10/16/17) | ⚠️ 待验证 |
| **ES8311 音频** | 将来：系统提示音 (GPIO 16/17/18) | ⚠️ 待验证 |
| **SD 卡槽** (SDMMC 4-bit, GPIO20-25) | 数据存储（封面缓存 + 歌词缓存 + 配置持久化） | ✅ 已有驱动 (FATFS, `/sdcard`) |
| **AP5056 充电** | 将来：电池供电 | ⚠️ 未启用 |

> ⚠️ **引脚冲突风险**: GPIO 9 被 LCD_DATA1 和 ES7210 共用，GPIO 16 被 LCD_DATA8 和 ES8311 共用。需确认原理图是否有复用器或跳线。

### 2.2 网络要求

- ESP32 和 PC 必须在**同一个 WiFi 局域网**（同一网段）
- PC 的 IP 建议设为**静态**（否则重启后 IP 可能变，ESP32 找不到）

---

## 3. 项目结构

```
esp32s31_6project/
├── CMakeLists.txt                        # 顶层构建（标准 ESP-IDF）
├── sdkconfig.defaults                    # 默认配置（PSRAM / LVGL / Bitmap 字体）
├── partitions.csv                        # 分区表（8MB factory）
│
├── components/
│   ├── bsp/
│   │   ├── CMakeLists.txt
│   │   ├── bsp.c                        # 编译时断言
│   │   └── include/bsp/bsp_board.h      # ⭐ 所有引脚/常量定义 (100行)
│   ├── hal_display/                      # LCD + 触摸 (已有, 未改动)
│   ├── hal_led/                          # WS2812 RMT 驱动 (已有, 未改动)
│   ├── hal_key/                          # ADC 按键扫描 (已有, 未改动)
│   └── hal_sdcard/                       # 🔄 SDMMC 4-bit → FATFS 挂载 (v0.12 新增)
│
├── main/                                 # ⭐ 主要工作区
│   ├── CMakeLists.txt                    # 源文件 + 依赖（嵌入 font_noto_sc_28.c）
│   ├── idf_component.yml                # 托管组件: lvgl, esp_websocket_client, cjson
│   ├── main.c                           # 入口编排
│   ├── app_ui.c/h                       # LVGL 端口初始化 (仅注册显示+触摸, ~70行)
│   ├── app_music_screen.c/h            # 🔥 全屏音乐歌词界面（Bitmap 字库，28px/20px）
│   ├── app_deepseek_screen.c/h         # 🔥 DeepSeek API 用量可视化 (v0.13 WorkBuddy 数据源, 4卡片不等宽+日消费柱状图可拖动+模型明细表)
│   ├── app_led_effects.c/h             # 🔥 WS2812 氛围灯引擎（脉冲/呼吸/彩虹/关）
│   ├── font_noto_sc_28.c               # 🔥 28px CJK 全字集位图字体 (33MB, RLE 压缩)
│   ├── font_noto_sc_20.c               # 20px CJK 全字集位图字体 (5MB, RLE 压缩)
│   ├── app_network.c/h                  # 🔥 WiFi 站模式 (已修重连 bug)
│   ├── app_ws_client.c/h               # 🔥 WebSocket 客户端 + JSON 解析
│   └── app_logic.c/h                    # 按键 → 音乐控制 + LED 调度 + 模式切换
│
├── tools/                                # 工具脚本
│   └── prepare_font.py                  # Noto Sans SC TTF 下载 + 裁剪 (FreeType 时代遗留)
│
├── pc_tools/
│   └── windows_media_server.py          # PC 端媒体服务器 (Phase 1, 支持 shuffle/repeat/seek)
│
├── docs/
│   ├── desktop_music_companion.md       # 完整设计文档 (v1.0)
│   └── handover_phase2.md              # 👈 本文件
│
└── managed_components/                  # ESP-IDF 组件管理器下载
    ├── lvgl__lvgl/
    ├── espressif__esp_lvgl_port/
    ├── espressif__esp_websocket_client/
    ├── espressif__cjson/
    ├── espressif__esp_lcd_touch/
    └── espressif__esp_lcd_touch_gt1151/
```

---

## 4. 模块详解

### 4.1 初始化顺序 (`main/main.c`)

```
app_main()
├── (1) hal_display_lcd_init()       → LCD 面板 (RGB)
├── (2) hal_display_touch_init()     → GT1151 触摸
├── (3) app_ui_init()                → LVGL 端口 (仅注册显示+触摸)
├── (4) app_music_screen_init()      → 全屏音乐 UI 对象 (含 4 模式容器)
│     app_music_screen_show()        → 🔥 立即显示 (黑色背景 + "No Music")
├── (4.5) app_deepseek_screen_init() → DeepSeek API 用量页面 (隐藏, v0.12)
├── (5) hal_led_init()               → WS2812 LED
├── (5.5) hal_sdcard_mount()         → SDMMC 4-bit → FATFS 挂载 (v0.12, 失败仅 warning)
├── (6) app_led_effects_init()       → 氛围灯效果引擎（后台任务）
├── (7) hal_key_init()               → ADC 按键扫描任务
├── (8) xTaskCreate(app_logic_key_task) → 按键业务处理
└── (9) xTaskCreatePinnedToCore(network_task) → 🔥 WiFi → WebSocket (后台)
```

> **v0.8 变更**: 移除 Step 0 SPIFFS 初始化 — 字体文件直接嵌入固件，无需外部存储。

`network_task` 内部顺序：
```
app_network_init()
  → app_network_connect(ssid, pass)
  → app_network_wait_connected(20s)
    → (成功) app_ws_start(host, port, "/")
    → (失败) 退出任务 (音乐屏幕保持显示，离线状态)
```

> **关键变更**: 音乐屏幕**启动即显示**，不再等 WiFi 连上。WiFi 连上后 WebSocket 数据自动填充内容。

### 4.2 LVGL 端口 (`app_ui.c/h`)

**唯一接口**: `app_ui_init(lcd, touch)` — 初始化 LVGL，注册 RGB 显示设备和触摸输入。

模块极其精简 (~70 行)，**不创建任何 UI 元素**。所有 UI 由 `app_music_screen` 独立管理。

### 4.3 WiFi 模块 (`app_network.c/h`)

**接口**:

| 函数 | 说明 |
|------|------|
| `app_network_init()` | 初始化 NVS → netif → 事件循环 → WiFi 驱动 (幂等) |
| `app_network_connect(ssid, pwd)` | 配置凭据，启动连接 |
| `app_network_wait_connected(timeout)` | 阻塞等 IP，FreeRTOS 事件组同步 |
| `app_network_get_ip()` | 返回 IP 字串 "192.168.x.x" |
| `app_network_is_connected()` | bool 查询 |
| `app_network_disconnect()` | 断开 |
| `app_network_reconnect()` | 强制重连 |

**设计要点**:
- 使用 `EventGroupHandle_t` 同步：`WIFI_CONNECTED_BIT` / `WIFI_FAIL_BIT`
- 事件处理器监听 `WIFI_EVENT` + `IP_EVENT_STA_GOT_IP`
- **断线后显式调用 `esp_wifi_connect()`** (v0.3 已修：ESP-IDF v5+ 默认不自动重连)
- `wait_connected` 退出时**清除事件位** (`pdTRUE`)，避免死锁
- 线程安全：IP 字串只有事件处理器写，其他任务只读

### 4.4 WebSocket 客户端 (`app_ws_client.c/h`)

**接口**:

| 函数 | 说明 |
|------|------|
| `app_ws_start(host, port, path)` | 创建后台任务连接 ws://host:port/path |
| `app_ws_stop()` | 停止并销毁任务 |
| `app_ws_send_command(action)` | 发送 `{"type":"command","action":"..."}` |
| `app_ws_is_connected()` | bool 查询 |

**架构**: 单一 FreeRTOS 任务 (8KB 栈，优先级 4)：

```
app_ws_task 主循环:
  ① 构造 URI → esp_websocket_client_init
  ② esp_websocket_client_start (异步)
  ③ 等待 CONNECTED 信号量 (10s 超时)
     ├─ 成功 → 进入运行循环
     │   └─ 等待队列命令或断线
     └─ 失败 → 指数退避重连 (1s → 2s → 4s → ... → 30s max)
  ④ 断线 → 清理 → 回到 ①
```

**JSON 消息处理** (`dispatch_message`):

| 消息类型 | 处理函数 | 效果 |
|----------|---------|------|
| `song_info` | `handle_song_info()` | 更新歌名/歌手、播放状态、模式、位置 + 显示屏幕 |
| `lyrics` | `handle_lyrics()` | 歌词列表存入 `app_music_screen` |
| `position` | `handle_position()` | 更新进度条 + 匹配歌词行 |
| `server_status` | `handle_server_status()` | 更新播放状态 |
| `deepseek_usage` (v0.13) | `handle_deepseek_usage()` | 更新 DeepSeek API 用量数据 (余额/月消费/请求/Tokens/日消费柱状图/模型明细表) |

**发送命令**: 通过 `xQueueSend` 入队，主循环出队后调用 `esp_websocket_client_send_text`（超时 500ms）。

### 4.5 全屏音乐界面 (`app_music_screen.c/h`) ⭐

**唯一 UI 模块**，启动即全屏显示，背景为不透明黑色。

**布局** (800×480):

```
┌──────────────────────────────────────────────────────────┐ y=0
│ ● WiFi:MyWiFi ✓            白色风车        周杰伦   ▶ 1:23/4:45▶│ ← 顶部状态栏 48px
├──────────────────────────────────────────────────────────┤ y=48
│                              我 知 道                    ← 6行歌词       │
│                              你 都 不 懂                               │
│                          ▶  世 界 上 的 痛 苦  ← 当前行 青蓝高亮       │ ← 376px
│                              如 果 你 也 会 心 痛                       │
│                              为 什 么 你 不 懂                          │
│                              我 的 心 里 只 有 你                       │
├──────────────────────────────────────────────────────────┤ y=424
│ ⏮  ▶/⏸  ⏭  │  🔀  🔁  │  🔊  ▶▶                            │ ← 底部工具栏 56px
│ ████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│ ← 进度条 4px
└──────────────────────────────────────────────────────────┘ y=480
```

> 布局调整 (v0.5): 歌名/歌手/歌词整体右移 40-145px，填充右侧空白。底部按钮全部改用 FontAwesome 符号（LVGL 内置，无需额外字库）。
> 中文字体 (v0.8): 切换回 **嵌入式 Bitmap 字库**，`font_noto_sc_20.c` (20px, 5MB) + `font_noto_sc_28.c` (28px, 33MB)。
> - 2 级字号: 20px（外围行/歌手）、28px（歌名标题/歌词当前行高亮）
> - 需要 `CONFIG_LV_FONT_FMT_TXT_LARGE` 和 `CONFIG_LV_USE_FONT_COMPRESSED`
> - 28px 覆盖 ASCII + CJK 全字集 (0x4E00-0x9FFF, 20,992 字)；20px 覆盖 0x4E00-0x9FA5

**接口**:

| 函数 | 说明 |
|------|------|
| `app_music_screen_init()` | 创建所有 LVGL 对象 |
| `app_music_screen_show()` | 显示全屏音乐界面 |
| `app_music_screen_hide()` | 隐藏 |
| `app_music_screen_is_visible()` | bool 查询 |
| `app_music_screen_set_song(title, artist)` | 更新歌名 + 歌手 (同步更新 All Modes) |
| `app_music_screen_set_position(pos_ms, dur_ms)` | 进度条 + 歌词行匹配 |
| `app_music_screen_set_play_state(playing)` | ▶ / ⏸ 切换 |
| `app_music_screen_set_play_mode(shuffle, repeat)` | 模式图标高亮 (🔀/🔁/▶) |
| `app_music_screen_set_lyrics(texts, times, count)` | 歌词数据载入 |
| `app_music_screen_set_conn_state(connected)` | 连接状态圆点 (绿/红) + Info 模式 |
| `app_music_screen_set_wifi_state(connected, ssid)` | WiFi SSID 显示 + OK/✗ 图标 + Info 模式 |
| `app_music_screen_set_mode(mode)` | 切换显示模式 (Lyrics/NowPlaying/Visualizer/Info) |
| `app_music_screen_get_mode()` | 获取当前模式 |
| `app_music_screen_cycle_mode()` | 循环切到下一个模式 |

**歌词滚动算法 (v0.8 新增动画)**:
```
收到 position(ms) → find_lyrics_line() 二分查找 O(log n)
  → 行变了 → 判断跳转距离
    → 距1-2行: 启动平滑上移动画 (250ms ease-out, 偏移 44px)
    → 距>2行: 立即刷新 (跳转太远无动画)
  → 动画期间新行变化排队 (s_pending_line_idx)
  → 动画结束时更新 6 个标签文字 + 颜色 + 字号
  → 当前行 (i=2): 青蓝色 (80,220,255), 28px
  → 外围行 (i=0,1,3,4,5): 灰度递减 [100→160→…→80], 20px
```

**歌词横向滚动 (v0.9)**: 当前高亮行（第 3 行）若文本超出标签宽度（600px），自动从 `DOT` 截断模式切换到 `SCROLL_CIRCULAR` 循环滚动模式。
- 滚动速度: 40 px/s（由 `lv_obj_set_style_anim_speed` 控制）
- 停顿: 每轮滚动间停 2 秒（`repeat_delay = 2000ms`）
- 非当前行保持 `DOT` 截断
- 文字短于标签宽度时 LVGL 不触发滚动

**线程安全**: 所有 public 函数在操作 LVGL 前加 `lvgl_port_lock/unlock`。

**关键设计**:
- 容器为**不透明黑色背景** (`LV_OPA_COVER + lv_color_black()`)，完全遮住底层
- 进度条与子元素同层挂载在同一个容器上，show/hide 时同步
- 无死代码，无 `text_shadow` API（LVGL 8.4 不支持）

**显示模式 (v0.8)**:

| 模式 | 枚举 | 内容 | 入口 |
|------|------|------|------|
| **Lyrics** (默认) | `MUSIC_MODE_LYRICS` | 顶部状态栏 + 6 行滚动歌词 + 底部工具栏 | 启动默认 |
| **Now Playing** | `MUSIC_MODE_NOW_PLAYING` | 居中封面占位框 + 歌名 28px + 歌手 20px + 专辑 + 进度条 | MODE 长按 → |
| **Visualizer** | `MUSIC_MODE_VISUALIZER` | "频谱可视化" 占位（待 FFT 数据接入） | MODE 长按 → |
| **Info** | `MUSIC_MODE_INFO` | WiFi IP/SSID、WS 连接状态、固件版本、运行时间 | MODE 长按 → |

模式切换: MODE 长按 → 循环 0→1→2→3→0，或调用 `app_music_screen_cycle_mode()` / `app_music_screen_set_mode()`。

**底部触摸按钮 (v0.8)**:

| 按钮 | 位置 | 命令 | 视觉反馈 |
|------|------|------|---------|
| 🔀 | 左栏 | `toggle_shuffle` | 按下高亮 / 释放恢复 |
| 🔁 | 左栏 | `toggle_repeat` | 按下高亮 / 释放恢复 |
| ⏮ | 中栏 | `prev` | 按下高亮 / 释放恢复 |
| ▶/⏸ | 中栏 | `play_pause` | 按下高亮 / 释放恢复 |
| ⏭ | 中栏 | `next` | 按下高亮 / 释放恢复 |
| ▶▶ (v0.13) | 右栏 (x=738) | 跳转到 DeepSeek 用量页面 | 点击 → `app_music_screen_hide()` + `app_deepseek_screen_show()` |

### 4.6 字体系统 (嵌入式 Bitmap 字库) 🔄 v0.8

**架构**:

```
固件内置 (编译进 elf)
├── font_noto_sc_28.c  (33MB, RLE 压缩)
│     范围: 32-127 + 0x3000-0x303F + 0xFF01-0xFF5E + 0x4E00-0x9FFF
│     覆盖: ASCII + CJK 标点 + 全角符号 + CJK 基本区 20,992 字
│     参数: --bpp 4 --size 28 --format lvgl
│
└── font_noto_sc_20.c  (5MB, RLE 压缩)
      范围: 0x4E00-0x9FA5
      覆盖: CJK 基本区 ~20,000 字
      参数: --bpp 4 --size 20 --format lvgl
    │
    ▼
LVGL Bitmap 字体渲染 (lv_font_t)
    │
    ├── FONT_CN_28 = &font_noto_sc_28 → 歌名标题 (28px) + 歌词当前行高亮
    ├── FONT_CN_20 = &font_noto_sc_20 → 歌词非当前行、歌手 (20px)
    └── FONT_ICON_14/20/28 = Montserrat → 图标 + 拉丁字母 + 数字
```

**配置项**:

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_LV_FONT_MONTSERRAT_14/20/28` | y | 三级英文字号 |
| `CONFIG_LV_USE_FONT_COMPRESSED` | y | RLE 压缩字库支持 |
| `CONFIG_LV_FONT_FMT_TXT_LARGE` | y | 大字体索引（CJK 字库必需） |
| `CONFIG_SPIRAM` | y | PSRAM 存放字库 glyph_bitmap 数组 |

> **v0.8 变更**: FreeType + SPIFFS 方案已完全移除，回归嵌入式 Bitmap 字库方案。
> 两个字体 `.c` 文件编译进固件 → factory 分区需 8MB。

**字体的 2 级字号映射**:

| 线条 | 字号 | 像素 | 亮度 |
|------|------|------|------|
| 0 (最旧) | FONT_CN_20 | 20px | 100 |
| 1 | FONT_CN_20 | 20px | 160 |
| 2 (当前行) | **FONT_CN_28** | **28px** | **255**（青蓝 80,220,255） |
| 3 | FONT_CN_20 | 20px | 160 |
| 4 | FONT_CN_20 | 20px | 110 |
| 5 (最新) | FONT_CN_20 | 20px | 80 |

**28px 字体生成参数** (LVGL Font Converter):

```bash
lv_font_conv --bpp 4 --size 28 --stride 1 --align 1 \
  --font NotoSansSC-Regular.ttf \
  --range 32-127,0x3000-0x303F,0xFF01-0xFF5E,0x4E00-0x9FFF \
  --format lvgl -o font_noto_sc_28.c
```

> ⚠️ **生成后必须手动修复 3 处** (LVGL 8.4 兼容):
> 1. 删除 `extern const lv_font_t Noto Sans SC;` (标识符含空格)
> 2. 删除 `.static_bitmap = 0,` (LVGL 9.x 字段)
> 3. `.fallback = &Noto Sans SC,` → `.fallback = NULL,`

### 4.7 按键业务 (`app_logic.c`)

| 按键 | 短按 | 长按 |
|------|------|------|
| **SET** | WebSocket play_pause | 🔄 循环切换氛围灯模式 (脉冲→呼吸→彩虹→关) |
| **MODE** | WebSocket next | 🔄 循环切换显示模式 (Lyrics→NowPlaying→Visualizer→Info) |
| **VOL-** | 氛围灯亮度 -20 | **WebSocket prev** (Phase 5: 上一曲) |
| **VOL+** | 氛围灯亮度 +20 | **WebSocket next** (Phase 5: 下一曲) |

> **v0.9 变更**: SET 长按从 `LED 开关` 改为氛围灯模式循环切换；VOL± 从 `LED 亮度` 改为 `氛围灯亮度`。

所有 WebSocket 命令调用 `app_ws_send_command()`，若未连接则静默失败。

### 4.8 氛围灯效果 (`app_led_effects.c/h`) 🆕 v0.9

**WS2812 RGB LED 效果引擎**，独立 FreeRTOS 任务 (2048 字节栈, 优先级 3)，每 ~30ms 渲染一帧输出到 `hal_led_set_rgb()`。

**4 种效果模式**:

| 模式 | 枚举 | 说明 |
|------|------|------|
| **脉冲 (PULSE)** | `LED_EFFECT_PULSE` | 默认模式。播放中 → 环境微光 (8%)；歌词行切换 → 青蓝脉冲 (40%→100%→60%)；暂停 → 彩虹循环 |
| **呼吸 (BREATHE)** | `LED_EFFECT_BREATHE` | 正弦波暖色呼吸 (30%→80% 亮度)，周期 2s |
| **彩虹 (RAINBOW)** | `LED_EFFECT_RAINBOW` | HSV 色相循环 (8s 周期)，亮度 40% |
| **关 (OFF)** | `LED_EFFECT_OFF` | 全部熄灭 |

**脉冲状态机**:
```
IDLE (播放中) ↔ TRIGGERED → ATTACK (0→200ms, 40%→100%)
                          → DECAY (200→600ms, 100%→60%)
                          → IDLE 恢复微光
PAUSED → 彩虹循环
```

**接口**:

| 函数 | 说明 |
|------|------|
| `app_led_effects_init()` | 创建效果任务 (在 `main.c` hal_led_init 后调用) |
| `app_led_effects_set_mode(mode)` | 切换效果模式 |
| `app_led_effects_get_mode()` | 获取当前模式 |
| `app_led_effects_cycle_mode()` | 循环切到下一个模式 |
| `app_led_trigger_pulse(r, g, b)` | 触发一次脉冲（歌词行切换时调用） |
| `app_led_effects_set_playing(bool)` | 同步播放状态（暂停时 PULSE 模式自动切彩虹） |
| `app_led_effects_set_brightness(v)` | 设置全局亮度 0~255 |
| `app_led_effects_adjust_brightness(delta)` | 增减亮度 |
| `app_led_effects_get_brightness()` | 获取当前亮度 |

**触发链路**:
```
app_music_screen: 歌词行切换
  → app_led_trigger_pulse(80, 220, 255)  // 青蓝色，与高亮色一致

app_music_screen_set_play_state(playing)
  → app_led_effects_set_playing(playing)  // 暂停/恢复时调整 PULSE 模式行为

app_logic: SET 长按
  → app_led_effects_cycle_mode()          // 切换效果模式

app_logic: VOL± 短按
  → app_led_effects_adjust_brightness(±20) // 调节亮度
```

### 4.9 PC 端 (`pc_tools/windows_media_server.py`)

**依赖**: `pip install winsdk websockets requests`

**架构** (asyncio 三协程):
- `poll_media_loop()` — 每秒读 SMTC，检测换歌/状态变化，查歌词
- `position_loop()` — 每秒广播 `position` 消息
- WebSocket Server — 接受连接，推/收消息

**歌词获取 (多源兜底 v0.9 调整)**:

| 优先级 | 数据源 | 说明 |
|--------|--------|------|
| 1 | **QQ Music API** (`musicu.fcg`) | 主源：SMTC Track ID 直达，base64 解码 LRC，QQ音乐播放时最精准 |
| 2 | **NetEase Music API** (网易云) | 通用 fallback，中英文歌曲都全 |
| 3 | **LRCLIB** (`lrclib.net`) | 海外公共 API，国内可能不通，兜底备用 |

- 搜索方式：先用 SMTC `Genres` 字段提取播放器专属 Track ID（如 `BetterLyrics.QQMusicTrackID:123456`），有 ID 直接查；没有则按 `title + artist` 搜索
- 终端会打印歌词预览（前 6 行 + 时间戳），方便肉眼确认
- **v0.9 变更**: QQ Music 从 fallback 升为主源（因为用户 PC 端就是 QQ Music，歌词匹配更准确）；NetEase 降为第 2 优先级

**DeepSeek 用量采集 (v0.13 改为 WorkBuddy 文件模式)**:

支持两种数据源（文件模式优先）：

| 模式 | 参数 | 轮询间隔 | 数据内容 |
|------|------|----------|---------|
| **文件模式** (推荐) | `--deepseek-file D:\path\to\deepseek_usage_data.json` | 每 300s | 余额 + 月消费 + API请求 + Tokens + 每日消费 + 各模型用量 (来自 WorkBuddy) |
| **API 模式** (旧) | `--deepseek-key sk-xxx` | 每 60s | 仅余额 (API 不返回用量数据) |

- WorkBuddy 每 1h 自动采集 DeepSeek 平台数据，保存为 JSON 文件
- 文件格式：`{balance, currency, monthly_cost, models[{name,requests,tokens}], daily_usage[31], updated_at}`
- ESP32 端 DeepSeek 页面点击 Refresh 发送 `deepseek_refresh` 命令 → 立即读取文件
- 布局：导航栏(无Back) → 4卡片不等宽(175/175/135/235) → 消费金额标题 → 日消费柱状图(31天左右拖动) → 模型明细表(2行 Montserrat 20) → 底部栏(Refresh + Back to Music)
- 不再依赖 DeepSeek API Key（文件模式下完整数据）

**快捷键**: `P` = 暂停广播，`Q` = 退出

---

## 5. 构建与烧录

### 5.1 首次构建

```bash
# 1. 配置 WiFi 和 PC IP (必须!)
code main/main.c
# 修改:
#   #define WIFI_SSID       "你的WiFi"
#   #define WIFI_PASSWORD   "你的密码"
#   #define PC_HOST         "192.168.x.xxx"   # 运行 python 的电脑 IP

# 2. 编译
idf.py build

# 3. 烧录 + 监控
idf.py flash monitor
```

### 5.2 环境

- **ESP-IDF**: 6.2.0
- **目标芯片**: `CONFIG_IDF_TARGET="esp32s31"` (RISC-V)
- **电源**: USB-C 5V/2A

### 5.3 sdkconfig 关键配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `CONFIG_LV_FONT_MONTSERRAT_14/20/28` | y | 三级英文字号（图标 + 拉丁字母 + 数字用） |
| `CONFIG_LV_FONT_FMT_TXT_LARGE` | y | 大字体索引（CJK 字库 >6K 字符必需） |
| `CONFIG_LV_USE_FONT_COMPRESSED` | y | RLE 压缩字库支持（两个 Bitmap 字体均依赖） |
| `CONFIG_SPIRAM` | y | 启用 PSRAM (16MB) |
| `CONFIG_LVGL_PORT_ENABLE_PPA` | y | PPA 硬件加速 (2D) |
| `CONFIG_LV_USE_CHART` (v0.12) | y | LVGL 柱状图控件 (DeepSeek 页面用) |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | 自定义分区表（8MB factory，嵌入字体需要） |
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | y | 16MB Flash 容量 |

> **v0.8 变更**: 已移除 `CONFIG_LV_USE_FREETYPE`、`CONFIG_LV_FREETYPE_CACHE_SIZE`、`CONFIG_LV_FREETYPE_FONT_BPP`。
> `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 恢复默认 4096（不再需要 FreeType autofitter 大栈）。
> SPIFFS 相关配置全部移除（不再使用外部字体存储）。

### 5.4 托管组件 (`main/idf_component.yml`)

| 组件 | 版本 | 用途 |
|------|------|------|
| 组件 | 版本 | 用途 |
|------|------|------|
| `lvgl/lvgl` | ~8.4.0 | LVGL 图形库 |
| `espressif/esp_websocket_client` | ^1.6.0 | WebSocket 客户端 (IDF v6 移出到注册表) |
| `espressif/cjson` | ^1.7.19 | JSON 解析 (IDF v6 移出到注册表) |
| `espressif/esp_lcd_touch_gt1151` | ^1 | 触摸驱动（GT1151） |

> **注意**: FreeType 不再从注册表拉取（`espressif/freetype` 不存在），改用本地组件 `components/freetype/`。

### 5.5 PC 端启动

```bash
cd pc_tools/
pip install winsdk websockets requests
# 基础模式（仅音乐控制）:
python windows_media_server.py
# 启用 DeepSeek 用量可视化（文件模式推荐，无需 API Key）:
python windows_media_server.py --deepseek-file "D:\claude_code\deepseek_usage_data.json"
# 旧版 API Key 模式（仅余额，无用量数据）:
# python windows_media_server.py --deepseek-key "sk-xxxxxxxxxxxx"
```

输出示例:
```
SMTC session manager ready.
WebSocket server: ws://0.0.0.0:8765
Now: 白色风车 - 周杰伦
Lyrics lines: 32
Press [P] to pause  |  Press [Q] to quit
```

---

## 6. 通信协议

### 6.1 传输层

| 项目 | 值 |
|------|-----|
| 协议 | WebSocket `ws://` (无 TLS) |
| 端口 | 8765 |
| 格式 | JSON (UTF-8) |

### 6.2 PC → ESP32 消息

**song_info** (换歌/状态变化):
```json
{
  "type": "song_info",
  "title": "晴天",
  "artist": "周杰伦",
  "album": "叶惠美",
  "duration_ms": 263000,
  "position_ms": 45000,
  "state": "playing",
  "shuffle": false,
  "repeat_mode": "off"
}
```

**lyrics** (换歌时):
```json
{
  "type": "lyrics",
  "lines": [
    { "time_ms": 12000, "text": "故事的小黄花" },
    { "time_ms": 16000, "text": "从出生那年就飘着" }
  ]
}
```

**position** (每秒，v0.9 移除 state 字段):
```json
{
  "type": "position",
  "position_ms": 47000,
  "duration_ms": 263000
}
```
> **v0.9 变更**: 移除了 `state` 字段，避免位置消息在 PC 端异步切换状态时触发竞态条件（图标闪回）。播放状态仅由 `song_info` 消息携带。参见已知问题 #P1。

**server_status** (暂停/恢复):
```json
{
  "type": "server_status",
  "paused": false
}
```

**deepseek_usage** (v0.13 WorkBuddy 文件模式):
```json
{
  "type": "deepseek_usage",
  "balance": "2.45",
  "currency": "CNY",
  "monthly_cost": "4.92",
  "total_requests": 891,
  "total_tokens": 64769014,
  "daily_usage": [0.0, 0.05, 0.12, 0.33, 0.08, 0.0, 0.0, ...],
  "models": [
    {"name": "deepseek-v4-pro", "requests": 6, "tokens": 115395},
    {"name": "deepseek-v4-flash", "requests": 885, "tokens": 64653619}
  ],
  "last_sync": "2026-07-05 17:46:16"
}
```

### 6.3 ESP32 → PC 消息

**command**:
```json
{
  "type": "command",
  "action": "play_pause"   // play_pause | next | prev | deepseek_refresh (v0.13)
}
```

---

## 7. 开发路线图

| Phase | 内容 | 状态 | 涉及文件 |
|-------|------|------|---------|
| **1** | PC 端 SMTC + 歌词 + WebSocket 服务器（多源兜底 QQ Music → NetEase → LRCLIB + shuffle/repeat/seek） | ✅ 完成 | `windows_media_server.py` |
| **2** | ESP32 WiFi + WS 客户端 + 全屏歌词 + 中文字体 | ✅ 完成 | `app_network.*`, `app_ws_client.*`, `app_music_screen.*` |
| **3** | 顶部状态栏 + 歌词列表 + 中文字体渲染 | ✅ 完成 | `app_music_screen.c` |
|  | 歌词滚动动画 + 触摸交互 + 多模式切换 | ✅ 完成 (v0.8) | `app_music_screen.c` 增强 |
| **4** | WS2812 氛围灯 (歌词脉冲/呼吸/彩虹/关) + 亮度调节 + 播放状态感知 | ✅ 完成 (v0.9) | `app_led_effects.*`, `app_logic.c`, `app_music_screen.c` |
| **5** | 按键映射完善 + 歌词点击跳转 + Info 动态更新 | ✅ 完成 | `app_logic.c`, `app_music_screen.c`, `app_ws_client.c/h` |
| **6a** | 语音命令 (ESP-SR) | ⚠️ 待验证 | `app_voice_cmd.*` |
| **6b** | SD 卡集成 (SDMMC 4-bit, FATFS) | ✅ 完成 (v0.12) | `hal_sdcard.*` |
|  | DeepSeek API 用量可视化 (WorkBuddy 文件源, 4卡片不等宽+日消费柱状图可拖动+模型明细表) | ✅ 完成 (v0.13) | `app_deepseek_screen.*`, `windows_media_server.py` |

### 下一步工作建议

#### SD 卡集成规划（Phase 6b — 基础驱动已完成 ✅）

SD 卡驱动层 (SDMMC 4-bit → FATFS) 已在 v0.12 完成，挂载到 `/sdcard`。
后续可基于此逐层往上做：

| 阶段 | 内容 | 状态 |
|------|------|------|
| **1. SD 卡驱动** | SDMMC 4-bit 驱动 + FATFS 挂载 `/sdcard` | ✅ 已完成 (v0.12) |
| **2. 封面缓存** | PC 端传封面 JPEG/PNG → 存 SD 卡 → 歌词页背景 | 🗓️ 待实施 |
| **3. 封面背景渲染** | LVGL 以缩放后的封面图作为歌词列表背景 | 🗓️ 待实施 |
| **4. 歌词持久化** | PC 端传 LRC 歌词 → 存 SD 卡，断网离线加载 | 🗓️ 待实施 |
| **5. 配置持久化** | WiFi 配网信息、亮度/音量设置存 SD 卡 JSON | 🗓️ 待实施 |

> 💡 **封面背景效果设想**：
>
> ```text
> ┌─────────────────────────────────┐
> │  ███████████████████████████████ │  ← 模糊/半透明封面填满屏幕
> │  ██  如果我的音乐听起来悲伤    ██ │
> │  ██  那是因为我的心已经死了    ██ │  ← 歌词叠加在封面上方
> │  ██  在很久很久以前              ██ │
> │  ███████████████████████████████ │
> │      [状态栏] [播放/暂停]        │  ← 底部控制栏保持原样
> └─────────────────────────────────┘
> ```

#### 远期愿景

1. **用户习惯学习** — 记录用户常听的曲风、常调的音量、偏好的显示模式，越用越"懂"用户
2. **Visualizer 模式** — 接入 FFT 数据实时渲染频谱（需 PC 端发送频域数据）
3. **ESP-SR 语音命令验证** — 确认 RISC-V 兼容性后集成 5 个命令词（下一曲/上一曲/播放暂停/调高音量/调低音量）
4. **触摸长按进阶** — 底部按钮增加长按功能（如长按 shuffle→全部关闭）
5. **WiFi 配网** — 从硬编码 SSID 改为 SmartConfig/BLE 配网流程
6. **Now Playing 封面显示** — 在 Now Playing 模式展示当前封面

---

## 8. 已知问题与风险

### 8.1 技术风险

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| **ESP-SR 不兼容 RISC-V** | 中 | 语音功能无法启用 | 降级为按键+触摸，项目不受影响 |
| **LCD 与音频引脚冲突** (GPIO 9/16) | 中 | 麦克风/音频无法同时用 | 需确认原理图，跳线或分时复用 |
| **WiFi 断连** | 低 | 无数据更新 | ✅ 已修：显式重连 + 连接状态圆点指示 |

### 8.2 当前限制

1. **底部按钮无长按反馈** — 触摸有 press/release 视觉反馈，但无长按功能
2. **不支持 wss://** — 局域网场景下问题不大
3. **WiFi 凭据硬编码** — 明文写在 `main.c` 中 (技术债)
4. **PC 端只支持 Windows** — 因使用 WinRT SMTC API
5. **字体文件巨大** — `font_noto_sc_28.c` 约 33MB / `font_noto_sc_20.c` 约 5MB，均在固件中
6. **固件过大** — 需 8MB factory 分区（分区表已适配）

### 8.3 已解决问题 🆕

| # | 问题 | 版本 | 修复方式 |
|---|------|------|----------|
| P1 | Play/pause 图标闪回：点击后短暂显示 ⏸，又被服务器位置消息回退到 ▶ | v0.9 | PC 端 `position_loop` 移除 `state` 字段，状态仅由 `song_info` 携带；ESP32 端添加本地即时图标切换 |
| P2 | 英文歌词超出标签宽度被截断（...） | v0.9 | 当前行启用 `LV_LABEL_LONG_SCROLL_CIRCULAR` 横向滚动，40px/s，停 2s 循环 |
| P3 | 歌词行切换闪烁 | v0.8 | 添加平滑上移动画 (250ms ease-out)，跳转 >2 行时跳过动画 |
| P4 | WiFi 断线不重连 | v0.3 | 显式 `esp_wifi_connect()` + EventGroup 同步 |

---

## 9. 快速排错

### 9.1 编译报错

| 错误 | 原因 | 解决 |
|------|------|------|
| `esp_websocket_client.h: No such file` | 托管组件未拉取 | 检查 `main/idf_component.yml`，运行 `idf.py reconfigure` |
| `cJSON.h: No such file` | cjson 未在依赖中 | 检查 `main/CMakeLists.txt` 有 `cjson` |
| undefined reference to `esp_websocket_client_*` | 库未链接 | `idf.py fullclean` 后重新 `idf.py build` |
| `ESP_RETURN_ON_ERROR` implicit | 缺少 esp_check.h | 已在 `app_network.c` 中添加 (v0.3) |
| `ft2build.h: No such file` | lv_freetype.c 找不到 FreeType 头文件 | `main/CMakeLists.txt` 需 `target_include_directories(__idf_lvgl__lvgl PRIVATE "${freetype_dir}/include")` |
| `undefined reference to tt_driver_class` | ftmodule.h 与实际编译模块不匹配 | 检查 `components/freetype/include/freetype/config/ftmodule.h` 是否只包含已编译模块 |
| `ftmac.c: Carbon.h not found` | 编译了 Mac-only 的 `ftmac.c` | ✅ 已在 CMakeLists.txt 中跳过 |
| `FT_ERR_PREFIXInvalid_Face_Handle` / `FT_ERR_PREFIX` 相关错误 | `fterrors.h` 撤消了 `FT_ERR_PREFIX` 宏 | ✅ 在 `components/freetype/CMakeLists.txt` 添加 `target_compile_definitions(PRIVATE FT2_BUILD_LIBRARY)` |
| `undefined reference to FT_Vector_Length` | 缺少 `fttrigon.c`（三角学函数） | ✅ 在 `components/freetype/CMakeLists.txt` 添加 `src/base/fttrigon.c` |
| `undefined reference to FT_Set_Named_Instance` | 缺少 `ftmm.c`（多主字体支持） | ✅ 在 `components/freetype/CMakeLists.txt` 添加 `src/base/ftmm.c` |
| `lv_freetype.h: No such file` | app_music_screen.c 找不到 LVGL FreeType 头 | ✅ `main/CMakeLists.txt` 添加 `target_include_directories(PRIVATE "${lvgl_dir}/src/extra/libs/freetype")` |
| `implicit declaration of 'lv_freetype_font_create'` | 用了 LVGL 9.x API，项目是 8.4 | ✅ 改为 `lv_ft_font_init(lv_ft_info_t*)` / `FT_FONT_STYLE_NORMAL` |
| SPIFFS 镜像 `SpiffsFullError` | `spiffs_data/` 目录大小超出 8MB 分区（如备份大文件残留） | 清理目录，只留 `noto_sans_sc.ttf` + `README.txt` |

### 9.2 运行时异常

| 现象 | 原因 | 解决 |
|------|------|------|
| WiFi 连不上 | SSID/密码错或不在范围 | 检查 `main.c` 中 `WIFI_SSID` / `WIFI_PASSWORD` |
| WiFi 连上但 WS 连不上 | PC IP 不对或 PC 没运行脚本 | 检查 `PC_HOST`，确认 `python windows_media_server.py` 在运行 |
| 屏幕只有 "No Music" | WiFi 没连上或 PC 没在放歌 | 看串口日志搜索 `WiFi 连接` |
| 歌词一直不显示 | PC 端没在放歌，或 SMTC 没读到 | 确认 QQ音乐/网易云在播放，检查 PC 脚本日志 |
| 歌词一直不显示（PC脚本有返回） | ESP32 WebSocket 接收缓冲区不够大导致 JSON 被截断 | ✅ 已修 (v0.4)：buffer_size=8192 + 分片消息累积 |
| 中文/歌词显示方框 | 字体使用了 Montserrat（无中文字形） | ✅ 已修 (v0.4)：歌名/歌手/歌词全部改用 Noto Sans SC 20px |
| 黑屏、`Guru Meditation Error: Stack protection fault` 出现在 `af_autofitter_load_glyph` 或 `af_latin_metrics_init_widths` | FreeType autofitter 初始化需要大量栈空间 | ✅ `CONFIG_ESP_MAIN_TASK_STACK_SIZE=24576`（默认 4096，需增大） |
| 黑屏、FreeType 字体加载成功后崩溃 | 同上（autofitter 在第一次字形渲染时触发） | 同上 |
| 字体加载极慢（>10s） | TTF 字体过大，或 SPIFFS 文件系统读写慢 | 用 `tools/prepare_font.py` 裁剪字库（当前 2.1MB，加载约 3s） |
| 中文字形全是方框 | `CONFIG_LV_FONT_FMT_TXT_LARGE` 未启用 | 检查 `sdkconfig` 中 `CONFIG_LV_FONT_FMT_TXT_LARGE=y` |

### 9.3 串口日志关键信息

**正常启动日志**:
```
app_network: WiFi STA 已启动，开始连接...
app_network: === WiFi 已连接，IP: 192.168.x.xxx ===
app_ws: WebSocket 已连接 → ws://192.168.x.xxx:8765/
app_ws: ♪ 晴天 - 周杰伦
app_ws: 歌词已加载: 58 行
```

**WiFi 未连接**（音乐屏幕照常显示，离线状态）:
```
app_network: ⚠ WiFi 连接超时！检查 SSID/密码
```

---

## 10. 变更记录

### v0.4 (2026-06-27) — 中文字体 + 歌词修复 + 多源歌词

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **WiFi 状态指示** | `app_music_screen.c` | 顶部状态栏显示 SSID + ✓/✗，WiFi 连接/断开时自动变色 |
| **思源黑体中文字体** | `font_noto_sc_20.c`, `CMakeLists.txt` | Noto Sans SC 20px，全界面中文渲染支持 |
| **PC 端多源歌词** | `windows_media_server.py` | NetEase Music API → LRCLIB 两级兜底，终端打印歌词预览 |
| **SMTC Genres 解析** | `windows_media_server.py` | 读取 QQ音乐/网易云 Track ID，精准歌词查询 |
| **WebSocket 大消息支持** | `app_ws_client.c` | 歌词 JSON 分片自动累积，buffer_size 提至 8KB |

#### Bug 修复

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| P1 | 中文显示方框（歌名/歌手用 Montserrat 字体，无中文字形） | `app_music_screen.c` | 歌名和歌手标签改用 `&font_noto_sc_20` |
| P2 | 歌词 JSON 被截断（默认 WebSocket buffer 仅 1024 字节） | `app_ws_client.c` | `buffer_size=8192` + `payload_len`/`fin` 分片累积 |
| P3 | LRCLIB 从国内不通，歌词始终为 0 | `windows_media_server.py` | 加入 NetEase Music API 为主源，LRCLIB 作 fallback |
| P4 | `CONFIG_LV_FONT_FMT_TXT_LARGE` 未生效（`sdkconfig` vs `sdkconfig.defaults` 不一致） | `sdkconfig` | 运行 `idf.py reconfigure` 应用默认配置 |
| P5 | WS2812 启动后常亮（RMT 初始化未发送黑帧） | `hal_led.c` | 初始化末尾调用 `led_hw_set_rgb(0,0,0)` |
| P6 | `led_hw_set_rgb` 编译错误（static 函数使用在前、定义在后） | `hal_led.c` | 添加 static 前向声明 |

#### 配置变更

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_LV_FONT_FMT_TXT_LARGE` | `y` | 大字体索引（CJK 自定义字体必需） |

### v0.3 (2026-06-27) — Phase 2 重构完成

#### 重构 (GIF demo 完全清除)

| 项目 | 之前 | 现在 |
|------|------|------|
| `app_ui.c` | 1360 行 (GIF播放器+RGB面板+HUD) | **70 行** (仅 LVGL 端口初始化) |
| `app_ui.h` | 8 个接口 | 1 个 `app_ui_init(lcd, touch)` |
| `app_logic.c` | GIF速度/LED同步/GIF播放暂停 | **纯音乐控制** (play_pause/next/prev + LED 亮度) |
| `app_music_screen` | 透明"叠加层"，GIF 从底下透出 | **不透明全屏 UI**，启动即显示，无 GIF 背景 |
| `main/gif/` | 3 个 GIF 文件 ~1MB | **已删除** |
| `bsp_board.h` | 155 行 (GIF区域/面板/按钮/遮幅/链接器符号) | **100 行** (仅 LCD 引脚 + LED + 按键) |
| `CMakeLists.txt` | 嵌入 3 个 GIF | 不嵌入 GIF；移除 `esp_driver_tsens` |
| `sdkconfig.defaults` | `CONFIG_LV_USE_GIF=y` | **已移除** |
| `main.c` | 音乐界面等 WiFi 连上才显示 | **启动即显示**，WiFi 后台异步 |

### Bug 修复

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| P1 | WiFi 断线不重连 (ESP-IDF v5+ 默认不自动重连) | `app_network.c` | DISCONNECTED 处理器添加 `esp_wifi_connect()` |
| P2 | `wait_connected` 死锁 (位不清除) | `app_network.c` | `xEventGroupWaitBits` 改为 `pdTRUE` 清位 |
| P3 | 死代码 `app_music_screen_set_visible()` | `app_music_screen.c` | 删除 |
| P4 | 进度条挂错父节点 | `app_music_screen.c` | 从 `lv_scr_act()` 改为 `s_container` |
| P5 | 歌词查找 O(n) | `app_music_screen.c` | 改为二分查找 O(log n) |
| P7 | WS 发送超时 2000ms | `app_ws_client.c` | 改为 500ms |
| — | emoji 渲染为白色方块 | `app_music_screen.c` | 全部改为 ASCII |

### 协议一致性 ✅

PC 端 `windows_media_server.py` 发送 `type`: `song_info` / `lyrics` / `position` / `server_status`；ESP32 `dispatch_message` 处理全部 4 种。
ESP32 → PC 发送 `play_pause` / `next` / `prev`；PC 端 `handle_command` 认全部 3 个（`prev`=`previous` 别名）。
**协议完全对齐，无遗漏。**

---

### v0.5 (2026-06-27) — UI 优化：布局/符号/歌词高亮

#### 新增/修改功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **布局右移** | `app_music_screen.c` | 歌名 (x=135→200)、歌手 (x=325→470)、歌词 (x=20→140) 整体右移，填充右侧空白 |
| **底部栏符号** | `app_music_screen.c` | `\|</>\|\|/>\|` → ⏮▶⏭，`S/R/M/V` → 🔀🔁🔇🔊（LVGL 内置 FontAwesome，零依赖） |
| **当前行青蓝高亮** | `app_music_screen.c` | 当前歌词行从灰度 (255) 改为青蓝色 (80, 220, 255)，视觉上更突出 |
| **指示符同步** | `app_music_screen.c` | ▶ 指示符位置随歌词右移 (x=15→135)，颜色同步青蓝 (80, 220, 255) |

#### Bug 修复

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| P4 | `CONFIG_LV_USE_FONT_COMPRESSED` 缺失导致所有中文字体渲染空白 | `sdkconfig.defaults` | 添加 `CONFIG_LV_USE_FONT_COMPRESSED=y`（P4 在 v0.4 标记为未生效，现已在 sdkconfig.defaults 中修复） |

#### 配置变更

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_LV_USE_FONT_COMPRESSED` | `y` | 压缩字库支持（`font_noto_sc_20.c` 的 `bitmap_format=1` 必需） |

---

### v0.6 (2026-06-28) — 字体系统迁移：位图 → FreeType + SPIFFS

#### 架构变更

| 项目 | 之前 | 现在 |
|------|------|------|
| **字体引擎** | LVGL 内置位图字体 | LVGL FreeType 引擎（`lv_freetype.c`） |
| **字体文件** | `font_noto_sc_20.c` 嵌入固件（133K 行，~5MB） | `noto_sans_sc.ttf` 存 SPIFFS（7.2MB，不占固件空间） |
| **字号** | 固定 20px | 3 级可变：14px / 20px / 28px（从同一 TTF 渲染） |
| **抗锯齿** | 无（位图预渲染） | ✅ 8bpp 抗锯齿（FreeType smooth 渲染器） |
| **CJK 支持** | ✅ 全字集（20K+ 字符） | ✅ 全字集 + 可变字号 |
| **字体占用固件** | ~5MB | 0（字体在 SPIFFS 分区） |

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **SPIFFS 文件系统** | `app_spiffs.c/h` | 挂载 8MB SPIFFS 分区，管理 TTF 字体文件 |
| **FreeType 字体引擎** | `components/freetype/` | 2.14.3 本地组件，精简 32 源文件，仅 TrueType + CJK + AA |
| **TTF 字体加载** | `app_music_screen.c` | `freetype_fonts_init()` 从 SPIFFS 加载 3 级字号 |
| **字体回退** | `app_music_screen.c` | TTF 不可用时回退 LVGL 内置 Montserrat |
| **字体裁剪脚本** | `tools/prepare_font.py` | 从 Google Fonts 下载 + pyftsubset 裁剪 Noto Sans SC |
| **FreeType 头文件传播** | `main/CMakeLists.txt` | `target_link_libraries` 桥接 lvgl → freetype 编译依赖 |

#### 文件变更

| 文件 | 状态 |
|------|------|
| `main/app_spiffs.c` | 🆕 新建 |
| `main/app_spiffs.h` | 🆕 新建 |
| `components/freetype/CMakeLists.txt` | 🆕 新建（ESP-IDF 组件注册） |
| `components/freetype/include/freetype/config/ftmodule.h` | 🆕 定制（8 模块） |
| `components/freetype/include/freetype/config/ftoption.h` | 🆕 定制（禁用 Mac/LZW/SVG/zlib） |
| `tools/prepare_font.py` | 🆕 新建 |
| `spiffs_data/noto_sans_sc.ttf` | 🆕 新建（7.2MB） |
| `partitions.csv` | 🔄 修改：3MB factory → 4MB factory + 8MB SPIFFS |
| `sdkconfig.defaults` | 🔄 修改：添加 FreeType/SPIFFS 配置 |
| `main/CMakeLists.txt` | 🔄 修改：添加 freetype/spiffs 依赖 + SPIFFS 镜像 |
| `main/idf_component.yml` | 🔄 修改：移除 `espressif/freetype` |
| `main/main.c` | 🔄 修改：Step 0 SPIFFS 初始化 |
| `main/app_music_screen.c` | 🔄 修改：嵌入式字体 → FreeType 句柄 |
| `font_noto_sc_20.c` | ❌ 删除（133K 行位图字体） |
| `main/idf_component.yml` | 🔄 修改：移除 `espressif/freetype` 注册表项 |

#### 配置变更

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_LV_USE_FREETYPE` | `y` | 启用 FreeType 字体引擎 |
| `CONFIG_LV_FREETYPE_CACHE_SIZE` | `128` | FreeType 缓存上限（KB） |
| `CONFIG_LV_FREETYPE_FONT_BPP` | `8` | 抗锯齿 8bpp |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | 自定义分区表 |
| `CONFIG_SPIFFS_USE_MAGIC` | `y` | SPIFFS 魔术字 |
| `CONFIG_SPIFFS_USE_MAGIC_LENGTH` | `y` | SPIFFS 魔术字长度 |
| `CONFIG_SPIFFS_META_LENGTH` | `4` | SPIFFS 元数据长度 |
| `CONFIG_LV_FONT_FMT_TXT_LARGE` | **已移除** | 不再需要 |

---

### v0.7 (2026-06-28) — FreeType + SPIFFS 稳定化

#### Bug 修复

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| B1 | `FT2_BUILD_LIBRARY` 未定义，`fterrors.h` 导致 `FT_ERR_PREFIX` 编译错误 | `components/freetype/CMakeLists.txt` | 添加 `target_compile_definitions(PRIVATE FT2_BUILD_LIBRARY)` |
| B2 | 缺少 `fttrigon.c` / `ftmm.c` 导致链接失败（未定义 `FT_Vector_Length` / `FT_Set_Named_Instance`） | `components/freetype/CMakeLists.txt` | 添加两个源文件 |
| B3 | `lv_freetype.h` 找不到 / `lv_freetype_font_create` 隐式声明（LVGL 9.x API 误用于 8.4） | `main/CMakeLists.txt` + `app_music_screen.c` | 添加 include 路径；改用 `lv_ft_font_init()` / `FT_FONT_STYLE_NORMAL` / `lv_ft_font_destroy()` |
| B4 | **Stack protection fault**: `af_latin_metrics_init_widths()` 遍历 ~6600 CJK 字形，需要约 16KB 栈 | `sdkconfig` / `sdkconfig.defaults` | `CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096` → **24576** |
| B5 | SPIFFS 镜像 `SpiffsFullError`（备份大文件残留在 `spiffs_data/`） | `spiffs_data/` | 删除 `noto_sans_sc_full.ttf`（7.2MB 备份） |

#### 优化

| 项目 | 文件 | 说明 |
|------|------|------|
| **字库裁剪** | `spiffs_data/noto_sans_sc.ttf` | 完整 Noto Sans SC 7.2MB → **pyftsubset 裁剪后 2.1MB**（ASCII + CJK 常用 6600 字 + 标点），加载从 ~3.8s 缩短到 ~3s |
| **裁剪脚本改进** | `tools/prepare_font.py` | CJK 范围从 `0x4E00-0x9FFF`（2 万字）缩小至 `0x4E00-0x67FF`（前 6600 常用字） |
| **ADC 校准变量** | `components/hal_key/hal_key.c` | 将全局变量移到 `#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED` 块内，消除 `-Wunused-variable` 警告 |
| **deinit 函数** | `main/app_music_screen.c` | 添加 `__attribute__((unused))` 消除 `-Wunused-function` 警告 |

#### 文件变更

| 文件 | 状态 |
|------|------|
| `components/freetype/CMakeLists.txt` | 🔄 修改：添加 `FT2_BUILD_LIBRARY`、`fttrigon.c`、`ftmm.c` |
| `main/CMakeLists.txt` | 🔄 修改：添加 freetype include 路径 → LVGL 目标 |
| `main/app_music_screen.c` | 🔄 修改：LVGL 8.4 FreeType API 适配 + `__attribute__((unused))` |
| `sdkconfig.defaults` | 🔄 修改：`CONFIG_ESP_MAIN_TASK_STACK_SIZE=24576` |
| `sdkconfig` | 🔄 修改：同步更新 main task 栈配置 |
| `components/hal_key/hal_key.c` | 🔄 修改：ADC 校准变量移入 `#if` 块消除警告 |
| `spiffs_data/noto_sans_sc.ttf` | 🔄 修改：7.2MB → 2.1MB（pyftsubset 裁剪） |
| `tools/prepare_font.py` | 🔄 修改：CJK 范围缩小至常用 6600 字 |

---

### v0.8 (2026-06-28) — Phase 3 收尾 + Bitmap 字体回归

#### 架构回退

| 项目 | 之前 (v0.6-0.7) | 现在 (v0.8) |
|------|------|------|
| **字体引擎** | LVGL FreeType (lv_freetype.c) | LVGL 内置 Bitmap 字体 (lv_font_t) |
| **字体文件** | `noto_sans_sc.ttf` 存 SPIFFS (2.1MB) | `font_noto_sc_20.c` (5MB) + `font_noto_sc_28.c` (33MB) 嵌入固件 |
| **一级大纲级别数** | 3 级可调 (14/20/28px) | 2 级 (20/28px) |
| **抗锯齿** | FreeType smooth 渲染 8bpp | 位图预渲染 4bpp |
| **外部存储** | SPIFFS 8MB 分区 | 无（字体在固件中） |
| **编译产物** | 32 个 freetype 源文件 | 零外部依赖 |
| **Flash 占用** | 4MB factory + 8MB SPIFFS | 8MB factory |

#### 新增功能 (Phase 3 收尾)

| 项目 | 文件 | 说明 |
|------|------|------|
| **歌词滚动动画** | `main/app_music_screen.c` | 行切换时 LVGL 动画平滑上移 (250ms ease-out, 44px)，跳 >2 行时跳过动画 |
| **底部按钮触摸热区** | `main/app_music_screen.c` | ⏮▶⏭🔀🔁 可点击，press/release 视觉反馈，LV_EVENT_CLICKED 发送 WebSocket 命令 |
| **4 显示模式** | `main/app_music_screen.c/h` | Lyrics / Now Playing / Visualizer / Info，MODE 长按循环切换 |
| **Now Playing 模式** | `main/app_music_screen.c` | 居中封面占位框 (200×200) + 歌名 28px + 歌手 20px + 专辑 + 进度条 |
| **Visualizer 模式** | `main/app_music_screen.c` | "频谱可视化" 占位，待 FFT 数据接入 |
| **Info 模式** | `main/app_music_screen.c` | WiFi IP/SSID、WS 连接状态 (绿/红)、固件版本、运行时间 |
| **PC 端命令扩展** | `pc_tools/windows_media_server.py` | `toggle_shuffle` (SMTC try_change_shuffle_active_async)、`toggle_repeat` (off→all→one 循环)、`seek` (SMTC try_change_playback_position_async) |

#### 按键映射变更

| 按键 | 动作 | 之前 | 现在 |
|------|------|------|------|
| MODE 短按 | next | ✅ next | ✅ next (不变) |
| MODE 长按 | prev | ✅ prev | 🔄 循环切换显示模式 (Lyrics→NP→Viz→Info) |

#### 28px 字体生成与修复

LVGL Font Converter 生成的 `.c` 文件有 3 处 LVGL 9.x 语法错误，必须手动修复：

| # | 问题 | 修复 |
|---|------|------|
| 1 | `extern const lv_font_t Noto Sans SC;` | 删除此行 |
| 2 | `.static_bitmap = 0,` | 删除此行 (LVGL 9.x 字段) |
| 3 | `.fallback = &Noto Sans SC,` | 改为 `.fallback = NULL,` |

生成参数: `--bpp 4 --size 28 --range 32-127,0x3000-0x303F,0xFF01-0xFF5E,0x4E00-0x9FFF`

#### 文件变更

| 文件 | 状态 |
|------|------|
| `main/font_noto_sc_28.c` | 🔄 替换: 7.8MB/0x4E00-0x62FF → 33MB/0x4E00-0x9FFF (全 CJK 基本区) |
| `main/font_noto_sc_20.c` | ✅ 保持 |
| `main/app_music_screen.c` | 🔄 修改: 歌词动画 + 触摸热区 + 4 模式 + Info/NowPlaying UI |
| `main/app_music_screen.h` | 🔄 新增: mode 枚举 + 3 个 API |
| `main/app_logic.c` | 🔄 修改: MODE 长按 → 模式切换 |
| `pc_tools/windows_media_server.py` | 🔄 新增: toggle_shuffle / toggle_repeat / seek |
| `components/freetype/` | ❌ 删除 (~855 文件) |
| `main/app_spiffs.c/h` | ❌ 删除 |
| `spiffs_data/` | ❌ 删除 |
| `docs/handover_phase2.md` | 🔄 更新至 v0.8 |

---

### v0.9 (2026-06-30) — Phase 4 WS2812 氛围灯 + QQ Music 歌词 + 稳定性修复

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **WS2812 氛围灯引擎** | `main/app_led_effects.c/h` | 独立 FreeRTOS 任务，4 模式：脉冲/呼吸/彩虹/关，~33fps 渲染 |
| **歌词脉冲触发** | `app_music_screen.c` | 歌词行切换时触发青蓝脉冲 (80,220,255) → `app_led_trigger_pulse()` |
| **播放状态同步** | `app_music_screen.c` | `app_music_screen_set_play_state()` 同步到灯光引擎，暂停时 PULSE 模式自动切彩虹 |
| **氛围灯亮度调节** | `app_logic.c` | VOL± 短按增减亮度 (±20) |
| **氛围灯模式切换** | `app_logic.c` | SET 长按循环切换模式 (脉冲→呼吸→彩虹→关) |
| **QQ Music 歌词主源** | `windows_media_server.py` | 新增 `_fetch_qqmusic_lyrics()` 通过 `musicu.fcg` API + base64 LRC 解码 |
| **歌词横向滚动** | `app_music_screen.c` | 当前行超出标签宽度时 `SCROLL_CIRCULAR` 循环滚动 (40px/s, 停 2s) |

#### Bug 修复

| # | 问题 | 文件 | 修复 |
|---|------|------|------|
| P1 | Play/pause 图标闪回：按钮变 ⏸ 后又被服务器位置消息覆盖回 ▶ | `windows_media_server.py` | `position_loop` 移除 `state` 字段，状态仅由 `song_info` 携带；ESP32 端添加本地即时切换 |
| P2 | `lv_label_set_scroll_animation_time` / `lv_obj_set_style_anim_wait_time` 在 LVGL 8.4 不存在导致编译报错 | `app_music_screen.c` | 替换为 `lv_obj_set_style_anim_speed` + `lv_obj_set_style_anim` + 静态 `lv_anim_t` 模板 |

#### 文件变更

| 文件 | 状态 |
|------|------|
| `main/app_led_effects.c` | 🆕 新建（280 行，4 模式效果引擎） |
| `main/app_led_effects.h` | 🆕 新建（LED 效果 API 声明） |
| `main/app_music_screen.c` | 🔄 修改：歌词脉冲触发、播放状态同步、当前行横向滚动、按钮即时图标切换 |
| `main/app_logic.c` | 🔄 修改：SET 长按 → 氛围灯模式循环、VOL± ±20 亮度 |
| `main/main.c` | 🔄 修改：添加 `app_led_effects_init()` 初始化 |
| `main/CMakeLists.txt` | 🔄 修改：添加 `app_led_effects.c` 到 SRCS |
| `pc_tools/windows_media_server.py` | 🔄 修改：QQ Music 歌词 API + `position_loop` 移除 `state` 字段 |
| `docs/handover_phase2.md` | 🔄 更新至 v0.9 |

---

### v0.11 (2026-06-30) — 移除触摸手势检测

移除 `main/app_ui.c` 中的触摸手势检测代码。原因：GT1151 触摸屏滑动手势效果不佳。

| 变更 | 文件 | 说明 |
|------|------|--------|
| 删除手势检测 | `main/app_ui.c` | 移除手势检测定时器回调、相关静态变量和宏定义；移除多余 include |

### v0.10 (2026-06-30) — Phase 5 按键映射完善 + 歌词点击跳转 + Info 动态更新

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **触摸手势检测** | `main/app_ui.c` | LVGL 定时器轮询触摸坐标，左右滑切歌 (next/prev)，上下滑调音量 (±10) |
| **歌词点击跳转** | `main/app_music_screen.c` | 6 行歌词标签设为 CLICKABLE，点击任意行 → 发送 seek 到对应 time_ms |
| **VOL-/+ 长按映射** | `main/app_logic.c` | VOL- 长按 → prev（上一曲），VOL+ 长按 → next（下一曲） |
| **WebSocket 扩展命令** | `main/app_ws_client.c/h` | 新增 `app_ws_send_command_ex(action, extra_json)` + `app_ws_send_seek(ms)` |
| **Info 模式动态更新** | `main/app_music_screen.c` | LVGL 定时器每 2s 更新运行时间 + WiFi RSSI + IP 地址 |
| **音量手势** | `main/app_ui.c` | 本地音量变量 (0~100)，上/下滑 ±10，发送 `set_volume` 命令 |

#### 文件变更

| 文件 | 状态 |
|------|------|
| `main/app_ws_client.h` | 🔄 修改：新增 `app_ws_send_command_ex()` + `app_ws_send_seek()` 声明 |
| `main/app_ws_client.c` | 🔄 修改：实现 2 个新函数，基于 snprintf 构造 JSON 命令 |
| `main/app_music_screen.c` | 🔄 修改：歌词标签可点击 + seek 回调 + Info 定时器 + IP/RSSI/uptime 更新 + FW v0.9 |
| `main/app_logic.c` | 🔄 修改：VOL- 长按 → prev、VOL+ 长按 → next |
| `main/app_ui.c` | 🔄 修改：手势检测定时器 (30ms 轮询, 阈值 X:80/Y:60, 500ms 窗口) |
| `docs/handover_phase2.md` | 🔄 更新至 v0.10 |

#### 配置变更

无（Phase 5 无需新增 sdkconfig 项）

---

### v0.13 (2026-07-06) — DeepSeek 页面重设计：日消费柱状图 + 模型明细表 Montserrat 20 + 不等宽卡片

#### 架构变更

| 项目 | 之前 (v0.12) | 现在 (v0.13) |
|------|------|------|
| **DeepSeek 数据源** | 直接调用 DeepSeek API (需 API Key) | WorkBuddy 文件 `deepseek_usage_data.json` (无需 Key) |
| **PC 端轮询间隔** | 每 60s (API 模式) | 每 300s (文件模式) |
| **ESP32 页面布局** | 3 卡片 + 柱状图 + 模型进度条 | 4 卡片不等宽(175/175/135/235) + 日消费柱状图(左右拖动31天) + 模型明细表(2行 Montserrat 20) |
| **导航栏** | 含 ◀ Back 按钮 | **已移除**（底部栏 Back to Music 代替） |
| **模型对比条** | Tokens + API 请求两组水平对比条 | **已移除**（明细表取代） |
| **模型明细表** | 4 行 Montserrat 14, row_h=18 | **2 行 Montserrat 20, row_h=26** |
| **底部栏** | 含连接状态标签 | **已移除**（连接状态仅保留音乐屏幕） |
| **日消费柱状图** | `lv_chart` 固定显示 (已删除) | **重新添加**，`lv_obj` 矩形手动控高，水平拖动容器 |
| **数据门控 (has_data)** | 依赖 daily_usage 数组 (WorkBuddy 无此数据) | **已移除** |
| `deepseek_model_t` | `name[24]`, `tokens`, `percentage` | `name[28]`, `requests`, `tokens`（百分比由 ESP32 端计算） |
| **协议字段** | `balance_available/balance_total/today_tokens/month_tokens` | `balance/currency/monthly_cost/total_requests/total_tokens/daily_usage[]/models[].requests` |

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **4 卡片不等宽** | `main/app_deepseek_screen.c` | 充值余额(175)+7月消费(175)+API请求(135)+总Tokens(235)，尾部逗号格式化 |
| **日消费柱状图** | `main/app_deepseek_screen.c` | 31 天可左右拖动，`lv_obj` 矩形手动控高确保从底向上填充，内层 1116px 视口 800px |
| **模型明细表放大** | `main/app_deepseek_screen.c` | 数据行 Montserrat 20, row_h=26, 固定 2 行, 列宽重新分配 |
| **连接状态移除** | `main/app_deepseek_screen.c/h` | 底部栏和 `set_conn_state()` API 完全删除 |
| **导航栏简化** | `main/app_deepseek_screen.c` | 移除左上角 ◀ Back 按钮 |
| **Daily usage 解析** | `main/app_ws_client.c` | 新增 `daily_usage[]` JSON 数组解析，传入 ESP32 柱状图 |

#### 文件变更

| 文件 | 状态 |
|------|------|
| `main/app_deepseek_screen.h` | 🔄 重写：移除 `set_conn_state`；新增 `DAILY_USAGE_MAX_DAYS`；`update_data` 加 `daily_usage/daily_usage_count` 参数 |
| `main/app_deepseek_screen.c` | 🔄 重写：4 卡片不等宽 + 日消费柱状图(可拖动 lv_obj) + 模型明细表(Montserrat 20)；移除 ◀ Back + 模型对比条 + 连接状态 + `format_tokens`；改用纯 `lv_obj` 矩形替代 `lv_bar` 确保垂直填充方向正确 |
| `main/app_ws_client.c` | 🔄 修改：`handle_deepseek_usage()` 解析 `daily_usage[]` 数组；移除 2 处 `app_deepseek_screen_set_conn_state()` 调用 |
| `docs/handover_phase2.md` | 🔄 更新至 v0.13 重设计 |

#### 配置变更

无（不新增 sdkconfig 项 — 已移除 `CONFIG_LV_USE_CHART` 依赖）

---

### v0.12 (2026-07-04) — Phase 6b SD 卡集成 + DeepSeek API 用量可视化 (初版)

#### 新增功能

| 项目 | 文件 | 说明 |
|------|------|------|
| **SD 卡驱动 (SDMMC 4-bit)** | `components/hal_sdcard/` | GPIO20-25, FATFS 挂载到 `/sdcard` |
| **DeepSeek API 用量可视化** | `main/app_deepseek_screen.c/h` | 全屏覆盖层：导航栏 + 三统计卡片 + lv_chart 柱状图 + 模型进度条 |
| **▶▶ 下一页按钮** | `main/app_music_screen.c` | 底部栏 🎤 → ▶▶，点击跳转到 DeepSeek 页面 |
| **WebSocket deepseek_usage** | `main/app_ws_client.c` | 新增 `handle_deepseek_usage()` 消息分发 |
| **PC 端 DeepSeek 采集** | `windows_media_server.py` | `--deepseek-key` 参数，每 60s 轮询 /user/balance + /v1/usage |
| **SD 卡引脚宏** | `bsp_board.h` | 添加 `SDMMC_CLK/CMD/D0~D3_GPIO` 定义 |

#### 硬件确认

根据官方原理图 (ESP32-S3-Korvo-1 V1.1)，SD 卡槽使用 SDMMC 4-bit 模式，引脚分配如下：

| SDMMC 信号 | GPIO | 备注 |
|-----------|------|------|
| SDIO_CLK | GPIO24 | 空闲，不与 LCD 冲突 |
| SDIO_CMD | GPIO25 | 空闲，不与 LCD 冲突 |
| SDIO_DATA0 | GPIO20 | 空闲 |
| SDIO_DATA1 | GPIO21 | 空闲 |
| SDIO_DATA2 | GPIO22 | 空闲 |
| SDIO_DATA3 | GPIO23 | 空闲 |

> 注意：先前 SDMMC 方案（GPIO15/CMD 与 LCD_DATA7 冲突）已被否决。最终确认使用 GPIO20-25。

#### 文件变更

| 文件 | 状态 |
|------|------|
| `components/hal_sdcard/include/hal_sdcard/hal_sdcard.h` | 🆕 新建 |
| `components/hal_sdcard/hal_sdcard.c` | 🆕 新建 |
| `components/hal_sdcard/CMakeLists.txt` | 🆕 新建 |
| `components/hal_sdcard/idf_component.yml` | 🆕 新建 |
| `components/bsp/include/bsp/bsp_board.h` | 🔄 修改：添加 SDMMC 引脚宏 |
| `main/app_deepseek_screen.h` | 🆕 新建 |
| `main/app_deepseek_screen.c` | 🆕 新建 |
| `main/app_music_screen.c` | 🔄 修改：底部栏 🎤 → ▶▶ + 回调函数 |
| `main/app_ws_client.c` | 🔄 修改：添加 `handle_deepseek_usage()` |
| `main/main.c` | 🔄 修改：添加 SD 卡 + DeepSeek 页面初始化 |
| `main/CMakeLists.txt` | 🔄 修改：添加 `hal_sdcard` + `app_deepseek_screen.c` |
| `sdkconfig.defaults` | 🔄 修改：添加 `CONFIG_LV_USE_CHART=y` |
| `pc_tools/windows_media_server.py` | 🔄 修改：DeepSeek API 采集 + `--deepseek-key` 参数 |
| `docs/handover_phase2.md` | 🔄 更新至 v0.12 |

#### 配置变更

| 配置 | 值 | 说明 |
|------|-----|------|
| `CONFIG_LV_USE_CHART` | `y` | LVGL 柱状图控件 (DeepSeek 页面每日用量图表) |

---

> **编写**: Claude Code, 2026-07-04
