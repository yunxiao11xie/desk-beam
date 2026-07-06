# DeepSeek 用量数据管道

DeepSeek 官方不提供用量统计 API（仅提供余额查询 API）。本项目通过 WorkBuddy 的浏览器 CDP 自动化抓取用量页面，将数据写入本地 JSON 文件，再由 `windows_media_server.py` 定时读取并推送给 ESP32。

## 数据流

```
Edge 浏览器 (已登录 platform.deepseek.com)
    ↓ CDP 自动化抓取 (每小时一次，WorkBuddy 定时任务)
deepseek_usage_data.json (本地文件)
    ↓ windows_media_server.py 每 300s 读取
WebSocket (ws://PC_IP:8765)
    ↓ deepseek_usage 消息
ESP32 屏幕 (DeepSeek 用量页面)
```

## 文件说明

| 文件 | 用途 |
|------|------|
| `pc_tools/deepseek_skill/SKILL.md` | WorkBuddy Skill 完整说明，包含安装方式、执行流程、前置条件 |
| `pc_tools/deepseek_skill/scripts/parse_usage.py` | 从页面文本中解析余额、消费、模型用量 |
| `pc_tools/deepseek_skill/scripts/generate_report.py` | 生成可视化 HTML 报告（Chart.js 图表） |
| `pc_tools/windows_media_server.py` | PC 端桥接服务，`load_deepseek_usage_from_file()` 读取 JSON 并通过 WebSocket 推送 |

## WorkBuddy Skill 安装

```bash
# 在 WorkBuddy 中安装该 Skill，即可通过对话触发抓取
# 说 "查一下 DeepSeek 用量" 或 "deepseek usage" 即可
```

Skill 文件位于 `pc_tools/deepseek_skill/SKILL.md`，需要在 WorkBuddy 中注册使用。

## 前置条件

1. **Edge 浏览器**登录 [platform.deepseek.com](https://platform.deepseek.com)
2. Edge 开启远程调试：
   - 地址栏输入 `edge://inspect/#remote-debugging`
   - 勾选 "Allow remote debugging for this browser instance"
3. 确保 WorkBuddy 已配置该 Skill 并启用自动化

## JSON 数据格式

WorkBuddy 生成的 `deepseek_usage_data.json`：

```json
{
  "balance": "2.46",
  "currency": "CNY",
  "monthly_cost": "4.91",
  "month": "7月",
  "year": "2026",
  "models": [
    {"name": "deepseek-v4-pro", "requests": 6, "tokens": 115395},
    {"name": "deepseek-v4-flash", "requests": 880, "tokens": 64245138}
  ],
  "updated_at": "2026-07-05 15:49:37"
}
```

ESP32 收到 `deepseek_usage` WebSocket 消息后，`app_deepseek_screen.c` 渲染为 4 张统计卡片 + 日消费柱状图 + 模型明细表。

## 自动化刷新

WorkBuddy 定时任务每小时自动执行一次抓取，更新 JSON 文件。也可在对话中手动触发："查一下 DeepSeek 用量"。

## 无需该功能？

如果不需要 DeepSeek 用量显示，在 `windows_media_server.py` 启动时不传 `--deepseek-file` 参数即可，ESP32 会显示"No Data"占位。
