---
name: deepseek-usage-monitor
description: 通过浏览器 CDP 自动抓取 DeepSeek 开放平台用量页面数据，生成可视化 HTML 报告。当用户提到"DeepSeek 用量""监控 DeepSeek""查看 DeepSeek 余额""获取 DeepSeek 用量数据""deepseek usage""deepseek balance"等意图时触发。
agent_created: true
---

# DeepSeek 用量监控

## 概述

DeepSeek 官方不提供用量统计 API（仅提供余额查询 API `/user/balance`）。用量数据只能通过 `platform.deepseek.com/usage` 网页端获取。本 skill 通过浏览器 CDP 自动抓取该页面数据，生成带图表的可视化 HTML 报告。

## 关键事实

- **余额 API**: `GET https://api.deepseek.com/user/balance`（官方公开，需 API Key）
- **用量 API**: ❌ 官方未提供
- **用量数据来源**: `https://platform.deepseek.com/usage`（需浏览器登录态）
- **内部接口（不稳定）**: `platform.deepseek.com/api/v0/usage/amount` 和 `/cost`（需网页 Token，非 API Key）
- **数据输出路径**: `D:\claude_code\deepseek_usage_data.json`（固定路径，供 ESP32 等外部设备读取）
- **自动化刷新**: 已配置每小时自动抓取并更新 JSON 数据（Automation ID: `automation-1783237800780`）

## 工作流程

### 前置要求

1. 用户必须在 Edge 浏览器中登录 `platform.deepseek.com`
2. Edge 必须开启远程调试：地址栏访问 `edge://inspect/#remote-debugging`，勾选 "Allow remote debugging for this browser instance"

### 执行步骤

1. **加载 web-access skill** — 获取 CDP 操作指引
2. **检查 CDP 连接** — 运行 `check-deps.mjs --browser edge`
   - 若失败，提示用户开启 Edge 远程调试（见前置要求）
3. **打开用量页面** — 创建新 tab 访问 `https://platform.deepseek.com/usage`
   - 使用 `POST /new` 或 `POST /navigate`，URL 走 POST body
4. **提取数据** — 使用 `/eval` 执行 `document.body.innerText` 提取页面文本
   - 解析余额、消费、各模型请求次数和 Tokens
5. **生成 HTML 报告** — 使用 `scripts/generate_report.py` 生成带 Chart.js 图表的 HTML 文件
   - 输入：提取到的 JSON 数据
   - 输出：可视化 HTML 报告
6. **交付结果** — 使用 `present_files` 展示 HTML 报告
7. **清理** — 关闭自己创建的 tab

## 数据提取与解析

从 `document.body.innerText` 提取的文本包含以下结构化数据：

- **余额**: 格式 `充值余额 \n¥X.XX\nCNY`
- **消费**: 格式 `七月消费（按 UTC+0 时间）\n¥X.XX\nCNY`
- **模型数据**（每个模型 4 个指标）：
  - 模型名（如 `deepseek-v4-pro`）
  - `API 请求次数` + 数值
  - `Tokens` + 数值

解析脚本位于 `scripts/parse_usage.py`。

## 报告生成

报告模板 `assets/report_template.html` 包含：
- 余额/消费概览卡片
- 模型请求次数对比柱状图（Chart.js）
- 模型 Tokens 对比柱状图（Chart.js）
- 模型明细表格

生成命令：
```bash
python scripts/generate_report.py --data <json_file> --output <html_path>
```

## 注意事项

- 用量数据只有聚合值（无按天分布），因此无法生成时间趋势图
- CDP 抓取的数据和截图可能略有差异（页面实时刷新）
- 内部接口 `api/v0/usage/*` 随时可能变更，不依赖它们
- 每次执行需用户确认 Edge 已登录并开启远程调试

## ESP32 桌面搭子集成方案

### 数据流

```
Edge 浏览器 (已登录 platform.deepseek.com)
    ↓ (CDP 抓取，每小时自动刷新)
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

### 你的 WebSocket 脚本需要做的

1. **定时读取 JSON** — 每 5~10 分钟读取一次 `D:\claude_code\deepseek_usage_data.json`
2. **解析数据** — 提取余额、消费、模型用量
3. **通过 WebSocket 推送** — 发给 ESP32 屏幕显示
4. **屏幕显示建议** — 显示余额、本月消费、主要模型用量和更新时间

### 数据刷新机制

- **自动化任务** — 已配置每小时自动抓取一次，更新 JSON 文件
- **手动触发** — 用户说"查一下 DeepSeek 用量"即可立即刷新
- **ESP32 读取** — 你的 WebSocket 脚本独立定时读取，不依赖自动化触发

### 注意事项

- Edge 浏览器必须保持登录状态且远程调试开启，否则自动化会失败
- JSON 文件在自动化执行失败时保持上次成功数据，ESP32 不会显示空白
- 如果 Edge 未开启远程调试，自动化会静默失败，等待下次重试