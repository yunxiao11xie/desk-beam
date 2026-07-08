---
name: deepseek-usage-monitor
description: 通过浏览器 CDP 自动抓取 DeepSeek 开放平台用量页面数据，生成可视化 HTML 报告。当用户提到"DeepSeek 用量""监控 DeepSeek""查看 DeepSeek 余额""获取 DeepSeek 用量数据""deepseek usage""deepseek balance"等意图时触发。
agent_created: true
---

# DeepSeek 用量监控

## 概述

DeepSeek 官方不提供用量统计 API（仅提供余额查询 API `/user/balance`）。用量数据只能通过 `platform.deepseek.com/usage` 网页端获取。本 skill 通过浏览器 CDP（原生 WebSocket）自动抓取该页面数据，生成带图表的可视化 HTML 报告，并输出固定路径的 JSON 供 ESP32 等外部设备读取。

## 关键事实

- **余额 API**: `GET https://api.deepseek.com/user/balance`（官方公开，需 API Key）
- **用量 API**: ❌ 官方未提供
- **用量数据来源**: `https://platform.deepseek.com/usage`（需浏览器登录态）
- **内部接口（不稳定）**: `platform.deepseek.com/api/v0/usage/amount` 和 `/cost`（需网页 Token，非 API Key，仅作增强明细用）
- **数据输出路径**: `D:\claude_code\deepseek_usage_data.json`（固定路径，供 ESP32 等外部设备读取）
- **自动化刷新**: 已配置每天 09:00 / 13:00 / 17:00 / 21:00 自动抓取并更新 JSON（Automation ID: `automation-1783237800780`）
- **抓取脚本**: `deepseek_cdp_scraper.py`（原生 CDP WebSocket，无第三方浏览器驱动依赖）

## 架构与连接策略

脚本通过 Edge 的 CDP 调试端口连接，**优先使用独立抓取实例**，避免打扰用户日常浏览器：

| 优先级 | 端口 | 说明 |
|--------|------|------|
| 1 | `127.0.0.1:3456` | 独立抓取实例（命令行 `--remote-debugging-port=3456 --user-data-dir=D:\claude_code\edge_scraper_profile` 启动，预授权无弹窗） |
| 2 | 自动拉起 | 若 3456 未运行，脚本自动拉起上述独立实例 |
| 3 | `127.0.0.1:9222` | 兜底：用户日常 Edge 调试端口（需手动开启远程调试） |

- **浏览器级 WebSocket 地址**动态从 `http://127.0.0.1:<port>/json/version` 的 `webSocketDebuggerUrl` 读取，兼容新版 Edge 带 UUID 的 `/devtools/browser/<uuid>` 端点（旧式固定 `/devtools/browser` 在新版会 404）。
- **IPv4 绑定**：CDP 地址必须用 `127.0.0.1` 而非 `localhost`，否则 websocket-client 可能优先解析到 IPv6 `::1` 导致连接超时。

## 前置要求

1. **独立抓取实例首次登录**：首次运行前，需在 3456 端口对应的独立 Edge 窗口中手动登录一次 `platform.deepseek.com`，cookie 会持久化到 `D:\claude_code\edge_scraper_profile`，之后自动化静默读取，无需重复登录。
2. **日常 Edge 兜底（可选）**：若仅用 9222 兜底，则需在用户日常 Edge 中登录并开启远程调试（`edge://inspect/#remote-debugging`）。但推荐用 3456 独立实例，无确认弹窗、不打扰日常浏览。
3. **Python 依赖**：`websocket-client`（建议用隔离 venv 安装，Windows 下 venv 脚本在 `Scripts/` 目录）。

## 执行步骤

脚本 `deepseek_cdp_scraper.py` 完成全流程，无需手动分步：

1. **连接 CDP** — 按上述优先级连接浏览器，必要时自动拉起独立实例。
2. **创建空白 tab** — `Target.createTarget` + `Target.attachToTarget`（flatten），获取 `sessionId`。
3. **启用域并导航** — 启用 `Network`/`Page` 域后 `Page.navigate` 到用量页，等待 `Page.loadEventFired`。
4. **提取数据** — `Runtime.evaluate` 执行 `document.body.innerText` 提取页面文本；同时经 `Network.getResponseBody` 捕获 `usage/cost` 与 `usage/amount` 内部 API 的按天明细。
5. **解析与保存** — 解析余额、消费、各模型请求次数/Tokens，连同按天明细写入 `D:\claude_code\deepseek_usage_data.json`。
6. **清理** — 关闭自建 tab（`Target.closeTarget`），不影响用户原有 tab。

### 手动生成 HTML 报告（可选）

若需可视化报告，用 `scripts/generate_report.py`：

```bash
python scripts/generate_report.py --data D:\claude_code\deepseek_usage_data.json --output report.html
```

报告包含余额/消费概览卡片、模型请求次数与 Tokens 对比柱状图（Chart.js）、模型明细表格。

## 数据提取与解析

从 `document.body.innerText` 提取的文本包含以下结构化数据：

- **余额**: 格式 `充值余额 \n¥X.XX\nCNY`
- **消费**: 格式 `七月消费（按 UTC+0 时间）\n¥X.XX\nCNY`
- **模型数据**（每个模型 2 个指标）：
  - 模型名（如 `deepseek-v4-pro`）
  - `API 请求次数` + 数值
  - `Tokens` + 数值

解析逻辑内置于 `deepseek_cdp_scraper.py` 的 `parse_inner_text()`；独立文本解析器见 `scripts/parse_usage.py`（供调试用）。

## 注意事项

- 内部接口 `api/v0/usage/*` 随时可能变更，脚本以页面文本解析为主、API 明细为辅，API 失败不影响核心字段。
- **未登录检测**：若核心字段（余额/消费/模型）全空，脚本记录错误并提示登录，不写垃圾数据。
- 脚本用 `suppress_origin=True` 建立 WebSocket，规避部分 Edge 版本的 Origin 校验。
- 日常 Edge 远程调试端口（9222）连接可能触发"是否确认自动控制"弹窗并阻塞；因此优先使用 3456 独立实例（预授权无弹窗）。

## ESP32 桌面搭子集成方案

### 数据流

```
独立 Edge 实例 (已登录 platform.deepseek.com, 端口 3456)
    ↓ (CDP 抓取，每天 09:00/13:00/17:00/21:00 自动刷新)
D:\claude_code\deepseek_usage_data.json
    ↓ (你的 WebSocket 脚本定时读取)
PC / 服务器 (WebSocket 服务端)
    ↓ (WebSocket 推送)
ESP32 屏幕 (显示用量数据)
```

### JSON 数据格式（固定路径）

文件路径：`D:\claude_code\deepseek_usage_data.json`

```json
{
  "balance": "6.69",
  "currency": "CNY",
  "monthly_cost": "10.68",
  "month": "7",
  "year": "2026",
  "models": [
    {"name": "deepseek-v4-pro", "requests": 6, "tokens": 115395},
    {"name": "deepseek-v4-flash", "requests": 1740, "tokens": 145489957}
  ],
  "cost_detail": {"daily_cost": [{"date": "2026-07-01", "total_cost": "1.04", "models": [...]}]},
  "usage_detail": {"daily_usage": [{"date": "2026-07-01", "models": [...]}]},
  "updated_at": "2026-07-08 17:02:05"
}
```

> 注：`month` 为数字串（如 `"7"`），非 `"7月"`；`cost_detail`/`usage_detail` 为按天明细（最多 31 天），ESP32 端可按需忽略。

### 你的 WebSocket 脚本需要做的

1. **定时读取 JSON** — 每 5~10 分钟读取一次 `D:\claude_code\deepseek_usage_data.json`
2. **解析数据** — 提取余额、消费、模型用量
3. **通过 WebSocket 推送** — 发给 ESP32 屏幕显示
4. **屏幕显示建议** — 显示余额、本月消费、主要模型用量和更新时间

### 数据刷新机制

- **自动化任务** — 每天 09:00 / 13:00 / 17:00 / 21:00 自动抓取一次，更新 JSON 文件（仅白天，避免深夜无意义抓取）
- **手动触发** — 用户说"查一下 DeepSeek 用量"即可立即刷新
- **ESP32 读取** — 你的 WebSocket 脚本独立定时读取，不依赖自动化触发

### 注意事项

- 独立 Edge 实例首次需手动登录一次，cookie 持久化后自动化静默运行
- JSON 文件在自动化执行失败时保留上次成功数据，ESP32 不会显示空白
- 若独立实例未运行且 9222 也未开启，自动化会记录错误并退出，等待下次重试
