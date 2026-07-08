# Desktop Music Companion — ESP32-S31 桌面音乐伴侣

基于 **ESP32-S31-Korvo** + **LVGL 8.x** 的桌面副屏，通过 WiFi/WebSocket 连接 PC 端服务，实现**桌面歌词显示 + 氛围灯 + 音乐遥控器 + DeepSeek 用量监控**。

---

## 整体链路

```
QQ音乐/网易云 → Windows SMTC → windows_media_server.py → WiFi/WebSocket → ESP32 → LVGL 屏幕 + WS2812 灯带
```

| 端 | 职责 |
|---|---|
| **PC** | `pc_tools/windows_media_server.py` — 读取 Windows 系统媒体传输控制 (SMTC) 获取歌曲信息，多渠道查询歌词（QQ音乐/网易云/LRCLIB），通过 WebSocket (8765 端口) 广播给 ESP32 |
| **ESP32** | 接收 JSON 消息 → 渲染全屏 LVGL 界面 + WS2812 灯带效果，同时可反向发送播放控制命令 |

---

## 硬件要求

| 组件 | 型号/规格 |
|------|-----------|
| SoC | ESP32-S3 (ESP32-S31-Korvo 开发板) |
| LCD | ST7262E43, 800×480 RGB565 并行接口 |
| 触摸 | GT1151 (I2C, SDA=GPIO0, SCL=GPIO1) |
| LED | WS2812 RGB LED ×3 (RMT 驱动, GPIO37) |
| 按键 | ADC 按键阵列 ×4 (GPIO42, 电压分压) |
| SD 卡 | microSD 卡槽 — SDMMC 4-bit 模式 (GPIO20~25) |
| PSRAM | 必须 (LVGL 显示缓冲) |

### 引脚一览

| 功能 | GPIO | 备注 |
|------|------|------|
| **LCD RGB565 数据** | GPIO8~36 | 16 位并行数据总线 |
| LCD 同步信号 | HSYNC=44, VSYNC=45, DE=43, PCLK=40 | — |
| 触摸 I2C | SDA=0, SCL=1 | GT1151 控制器 |
| WS2812 LED | GPIO37 | RMT TX |
| ADC 按键 | GPIO42 | 4 按键分压阵列 |
| SD 卡 SDMMC | CLK=24, CMD=25, D0=20, D1=21, D2=22, D3=23 | 4-bit 模式 |

---

## 软件架构

```
┌───────────────────────────────────────────────────────────────────┐
│                          main.c                                   │
│        系统初始化编排 (按顺序: BSP → LCD → LVGL → LED → 网络)      │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─────────────────┐  ┌──────────────────────────────────────┐   │
│  │    应用层 (main/) │  │    硬件抽象层 (components/)          │   │
│  │                  │  │                                      │   │
│  │  app_music_screen│  │  hal_display  — LCD + 触摸驱动       │   │
│  │  app_deepseek_   │  │  hal_led      — WS2812 RMT 驱动     │   │
│  │    screen        │  │  hal_key      — ADC 按键扫描         │   │
│  │  app_led_effects │  │  hal_sdcard   — SD 卡 FATFS 挂载    │   │
│  │  app_ws_client   │  │  bsp          — 板级硬件配置+验证    │   │
│  │  app_network     │  │                                      │   │
│  │  app_logic       │  │                                      │   │
│  │  app_ui          │  │                                      │   │
│  └────────┬─────────┘  └──────────────────┬───────────────────┘   │
│           │                               │                       │
│           │     ┌─────────────────────┐   │                       │
│           └─────┤ managed_components  ├───┘                       │
│                 │ lvgl 8.x + esp_lvgl_port                      │
│                 │ esp_websocket_client + cJSON                   │
│                 └─────────────────────┘                           │
│                                                                   │
│  FreeRTOS 任务:                                                    │
│  ┌──────────┬──────────┬───────────┬──────────┬────────────┐     │
│  │ LVGL     │ 按键扫描  │ LED 效果   │ WS 接收  │ WiFi 连接   │     │
│  │ 主循环   │ hal_key  │ app_led   │ app_ws   │ network    │     │
│  │ prio: -  │ prio: 5  │ effects   │ prio: 4  │ prio: 3    │     │
│  │          │ stack:4K │ prio: 3   │ stack:8K │ stack:8K   │     │
│  └──────────┴──────────┴───────────┴──────────┴────────────┘     │
└───────────────────────────────────────────────────────────────────┘
```

### 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| **main** | `main.c` | 系统初始化编排，按顺序启动 BSP → LCD → LVGL → LED → 网络 → 按键 → WS |
| **app_ui** | `app_ui.c/.h` | LVGL 端口初始化（注册显示 + 触摸） |
| **app_music_screen** | `app_music_screen.c/.h` | 全屏音乐歌词界面：顶栏 + 6行歌词（4模式）+ 底栏控制 |
| **app_deepseek_screen** | `app_deepseek_screen.c/.h` | DeepSeek API 用量可视化页面：统计卡片 + 日消费柱状图 + 模型明细表 |
| **app_ws_client** | `app_ws_client.c/.h` | WebSocket 客户端：连接/重连/JSON 消息分发/命令发送 |
| **app_network** | `app_network.c/.h` | WiFi STA 模式连接管理，FreeRTOS 事件组同步 |
| **app_logic** | `app_logic.c/.h` | 按键 → 音乐控制/模式切换调度层 |
| **app_led_effects** | `app_led_effects.c/.h` | WS2812 氛围灯效果引擎：脉冲/呼吸/彩虹/关闭 4 种模式 |
| **hal_display** | `components/hal_display/` | RGB LCD 初始化 + GT1151 触摸 I2C |
| **hal_led** | `components/hal_led/` | WS2812 底层 RMT 驱动（含编码器） |
| **hal_key** | `components/hal_key/` | ADC 按键扫描：分类、消抖、短按/长按判定、队列发送 |
| **hal_sdcard** | `components/hal_sdcard/` | SDMMC 4-bit → FATFS 挂载/卸载/查询容量 |
| **bsp** | `components/bsp/` | 板级引脚宏 + 编译时参数验证 |
| **font_noto_sc** | `font_noto_sc_20.c / font_noto_sc_28.c` | 预渲染 Noto Sans SC 中文 Bitmap 字库 (RLE 压缩) |

---

## 屏幕布局

### 主页面：音乐伴侣屏 (800×480)

```
┌──────────────────────────────────────────┐ y=0
│  ♪ WiFi标志  SSID名      歌名28px   时:分 │ y=48  顶栏 (h=48)
│                                          │       歌曲名用NotoSans 28px,
│                                          │       歌手名用NotoSans 20px
├──────────────────────────────────────────┤
│                                          │
│        歌词行0 (浅灰, 20px)               │ y=137
│        歌词行1 (浅灰, 20px)               │ y=179
│      ▶ 歌词行2 (青色, 28px) ◀           │ y=219  当前行高亮居中
│        歌词行3 (浅灰, 20px)               │ y=267
│        歌词行4 (浅灰, 20px)               │ y=313
│        歌词行5 (浅灰, 20px)               │ y=359
│                                          │      共 6 行, 最大字号行
│                                          │      上下间距略大以适配
├──────────────────────────────────────────┤
│  ⏮ ▶ ⏭    随机 循环    音量图标  [→]    │ y=424  底栏 (h=56)
│  ▓▓▓▓░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 2:43 │ y=474  进度条 (h=4)
└──────────────────────────────────────────┘ y=480
```

**4 种显示模式**（按 MODE 键循环切换）：

| 模式 | 说明 |
|------|------|
| **Lyrics** (0) | 6 行滚动歌词，当前行青色高亮 28px，其余灰色 20px。换行时触发 LED 脉冲 |
| **Now Playing** (1) | 大封面占位 + 歌名/歌手/专辑 |
| **Visualizer** (2) | 频谱可视化（预留） |
| **Info** (3) | 系统信息 |

### 副页面：DeepSeek 用量监控屏 (800×480)

```
┌──────────────────────────────────────────┐ y=0
│       DeepSeek 用量监控      同步:hh:mm   │ y=48  导航栏
├────────┬────────┬────────┬───────────────┤
│ ¥2.45  │ ¥6.60  │  1155  │   90.8M       │ y=158 4 张统计卡片 (不等宽)
│ 余额   │ 月消费  │ API请求 │   总Token     │
├────────┴────────┴────────┴───────────────┤
│        31天每日消费柱状图 (可横向拖动)     │ y=290 114px 高
├──────────────────────────────────────────┤
│  模型            请求次数     Token数     │ y=424 模型明细表
│  deepseek-v4-pro      6        115k      │
│  deepseek-v4-flash  1149      90.7M      │
├──────────────────────────────────────────┤
│       [刷新数据]            [返回音乐]    │ y=480 底栏
└──────────────────────────────────────────┘
```

- 右下角 `[→]` 按钮切换到此页，底栏 `[返回音乐]` 切回音乐屏
- 数据来源：PC 端 `windows_media_server.py` 读取 WorkBuddy 生成的 `deepseek_usage_data.json`

---

## WebSocket 消息协议

### PC → ESP32 (下行)

| type | 携带数据 | 说明 |
|------|---------|------|
| `song_info` | title, artist, album, duration_ms, position_ms, state, shuffle, repeat_mode | 歌曲完整状态 |
| `lyrics` | lines[{time_ms, text}] | 带时间戳的歌词行数组 |
| `position` | position_ms, duration_ms | 播放进度（每秒 1 次） |
| `server_status` | paused, message | 服务器暂停/恢复广播 |
| `deepseek_usage` | balance, monthly_cost, total_requests, total_tokens, models[], daily_usage[], last_sync | DeepSeek 用量数据 |

### ESP32 → PC (上行)

```json
{ "type": "command", "action": "<action>" }
```

| action | 说明 |
|--------|------|
| `play_pause` | 播放/暂停切换 |
| `next` / `prev` | 上一首/下一首 |
| `seek` | 跳转进度 (附带 position_ms) |
| `toggle_shuffle` / `toggle_repeat` | 切换随机/循环模式 |
| `deepseek_refresh` | 手动刷新 DeepSeek 用量 |

---

## WS2812 氛围灯效果

3 颗 WS2812 灯珠，由 `app_led_effects` 专用任务每 ~30ms 渲染一帧：

| 模式 | 行为 |
|------|------|
| **脉冲** (默认) | 歌词换行时触发：ATTACK 200ms (40%→100%) → DECAY 400ms (100%→60%) → 微光 (8%)。暂停时自动切换彩虹循环 |
| **呼吸** | 暖色 (255,140,80) 正弦波呼吸，2s 周期 |
| **彩虹** | HSV 色相循环，8s 完整周期，40% 亮度 |
| **关闭** | 全灭 |

音量键可调节 LED 全局亮度 (0-255)，默认 200。

> 颜色序为 GRB（WS2812 标准顺序），`hal_led` 使用 RMT 互斥锁保护多任务访问。

---

## 按键映射

| 按键 | 短按 | 长按 (≥1s) |
|------|------|------------|
| **SET** | 下一首 ⏭ | 切换 LED 效果模式 |
| **MODE** | 切换音乐显示模式 (歌词→NowPlaying→可视化→信息) | 切换 LED 开关 |
| **VOL-** | 音量减 | LED 亮度减 (步进 20) |
| **VOL+** | 音量加 | LED 亮度加 (步进 20) |

### ADC 按键电压参考

| 按键 | 原始 ADC 值 | 分档范围 | 原理 |
|------|-------------|----------|------|
| SET | ~315 | < 600 | 10K 上拉到 3.3V, 不同按键接不同下拉电阻 |
| MODE | ~848 | 600~1150 | — |
| VOL- | ~1371 | 1150~1600 | — |
| VOL+ | ~1815 | 1600~2300 | — |
| 空闲 | ~0 / >2300 | — | 开路高阻 |

---

## 启动流程

```
app_main()
  ├─ bsp_board_init()          ┐
  ├─ xTaskCreate(network_task)  │  step 1: BSP + WiFi 异步连接
  ├─ hal_display_init()         │  step 2: LCD 显示初始化
  ├─ app_ui_init()              │  step 3: LVGL 端口 (显示+触摸)
  ├─ hal_led_init()             │  step 4: WS2812 底层 RMT
  ├─ app_music_screen_init()    ┐
  ├─ app_deepseek_screen_init() │  step 5: UI 创建
  ├─ app_led_effects_init()     │  step 6: LED 效果任务
  ├─ xTaskCreate(key_scan)      │  step 7: 按键扫描任务
  ├─ xTaskCreate(ws_recv)       │  step 8: WebSocket 连接
  └─ while(1) {                 ┘  LVGL 主循环
      lv_timer_handler();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
```

### 任务列表

| 任务 | 优先级 | 栈大小 | 核心 | 职责 |
|------|--------|--------|------|------|
| `app_main` → LVGL 主循环 | (无显式) | 继承 | — | `lv_timer_handler()` 驱动 |
| `hal_key` | 5 | 4096 | — | ADC 扫描 + 消抖 + 队列发送 |
| `app_led_effects` | 3 | 2048 | — | WS2812 效果帧渲染 |
| `app_ws` | 4 | 8192 | 0 | WebSocket 收发 |
| `network_task` | 3 | 8192 | — | WiFi 连接 + 状态监控 |

---

## PC 端服务

### 启动方式

```bash
cd pc_tools
pip install -r requirements.txt
python windows_media_server.py
```

启动后监听 `ws://0.0.0.0:8765`，打印局域网 IP 供 ESP32 连接。

### 歌词查询优先级

1. **QQ Music API** — SMTC Track ID 直达（最准）
2. **网易云音乐 API** — 标题+歌手通用搜索
3. **LRCLIB 公共 API** — 海外 fallback

### 快捷键

- `P` — 暂停/恢复广播到客户端
- `Q` — 退出服务器

---

## 构建与烧录

### 环境要求

- ESP-IDF v6.2+ (目标芯片 `esp32s31`)
- PSRAM 已启用 (Quad/Octal SPI)
- LVGL 8.x + esp_lvgl_port (IDF 组件管理器自动安装)

### 构建步骤

```bash
# 设置目标芯片
idf.py set-target esp32s31

# 编译
idf.py build

# 烧录
idf.py -p COMx flash monitor
```

### Kconfig 关键配置

- **PSRAM**: `Component config → ESP PSRAM → Quad/Octal SPI PSRAM`，必须开启
- **LVGL**: 使用组件默认配置即可
- **WiFi SSID/密码**: 在 `main.c` 中硬编码 `WIFI_SSID` / `WIFI_PASS` 宏

---

## 项目结构

```
.
├── CMakeLists.txt                        # 顶层项目定义
├── main/
│   ├── CMakeLists.txt                    # 应用层组件注册
│   ├── main.c                            # 入口 + 启动编排
│   ├── app_ui.c / app_ui.h               # LVGL 端口 (显示/触摸注册)
│   ├── app_music_screen.c / .h           # 全屏音乐歌词界面 (4 模式)
│   ├── app_deepseek_screen.c / .h        # DeepSeek 用量可视化页面
│   ├── app_ws_client.c / .h              # WebSocket 客户端 + JSON 协议
│   ├── app_network.c / .h                # WiFi STA 连接管理
│   ├── app_logic.c / .h                  # 按键 → 控制调度
│   ├── app_led_effects.c / .h            # WS2812 氛围灯效果引擎
│   ├── font_noto_sc_20.c                 # Noto Sans SC 20px Bitmap 字库 (RLE)
│   └── font_noto_sc_28.c                 # Noto Sans SC 28px Bitmap 字库 (RLE)
├── components/
│   ├── bsp/                              # 板级硬件配置 + 编译验证
│   │   ├── include/bsp/bsp_board.h       #   引脚/分辨率/亮度宏定义
│   │   └── bsp.c                         #   static_assert 编译验证
│   ├── hal_display/                      # LCD + 触摸硬件抽象
│   ├── hal_led/                          # WS2812 RMT 驱动 + 编码器
│   ├── hal_key/                          # ADC 按键阵列扫描
│   └── hal_sdcard/                       # SD 卡 SDMMC + FATFS
├── pc_tools/
│   ├── windows_media_server.py           # PC 端桥接服务 (SMTC→WebSocket)
│   └── requirements.txt                  # winsdk, websockets, requests
├── docs/                                 # 设计文档
├── managed_components/                   # IDF 组件管理器依赖
├── partitions.csv                        # 分区表
├── sdkconfig                             # 项目配置
└── README.md
```

---

## 关键设计要点

### LVGL 跨任务锁
`app_ws_client` 的消息分发在 WebSocket 任务上下文中直接调用 `app_music_screen_*` 接口，这些接口内部通过 `lvgl_port_lock()` 获取 LVGL 锁。带超时（100ms），避免与 LVGL 主循环死锁。

### WebSocket 分片接收
歌词 JSON 可能超过默认 8KB 缓冲区，`app_ws_client` 实现了分片累积：`WEBSOCKET_EVENT_DATA` 收到部分数据时累积到 `s_frag_buf`，待 `s_frag_len >= total` (优选) 或 `evt->fin` (兜底) 后一次性 `dispatch_message()`。

### 连接管理
WiFi 和 WebSocket 均在独立 FreeRTOS 任务中运行。WiFi 使用事件组同步；WebSocket 使用指数退避重连（1s → 2s → 4s → ... → 最大 30s）。

### PSRAM 使用
- LVGL 显示缓冲区 (PSRAM + DMA)
- WebSocket 接收缓冲区 (16KB)
- 歌词临时数组 (动态分配)

### 灯带颜色序
WS2812 使用 **GRB** 顺序（非 RGB），`hal_led_set_rgb()` 内部已按 G→R→B 写入。

---

## License

MIT
