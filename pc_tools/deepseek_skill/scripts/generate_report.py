#!/usr/bin/env python3
"""
DeepSeek 用量报告生成器
从 JSON 数据生成可视化 HTML 报告

用法:
  python generate_report.py --data data.json --output report.html
"""

import json
import argparse
import sys
from pathlib import Path


def generate_report(data: dict, output_path: str, screenshot_path: str = None):
    """从解析后的数据生成 HTML 报告"""

    balance = data.get("balance", "0.00")
    currency = data.get("currency", "CNY")
    cost = data.get("monthly_cost", "0.00")
    month = data.get("month", "本月")
    year = data.get("year", "2026")
    models = data.get("models", [])

    total_requests = sum(m.get("requests", 0) for m in models)
    total_tokens = sum(m.get("tokens", 0) for m in models)

    # Chart.js 数据
    model_names = [m["name"] for m in models]
    requests_data = [m.get("requests", 0) for m in models]
    tokens_data = [m.get("tokens", 0) for m in models]

    # 截图区域
    screenshot_section = ""
    if screenshot_path:
        img_name = Path(screenshot_path).name
        screenshot_section = f"""
    <div class="chart-section">
      <div class="chart-title">每月用量 — 消费金额</div>
      <div style="text-align: center; padding: 16px 0;">
        <img src="{img_name}" alt="每月用量消费金额柱状图" style="max-width: 100%; border-radius: 8px; border: 1px solid #e8e8e8;">
        <p style="font-size: 13px; color: #888; margin-top: 12px;">数据来源: platform.deepseek.com/usage · {year}年{month} · 按 UTC+0 时间</p>
      </div>
    </div>"""

    # 表格行
    table_rows = ""
    for m in models:
        name = m["name"]
        req = m.get("requests", 0)
        tok = m.get("tokens", 0)
        tag_class = "tag-pro" if "pro" in name.lower() else "tag-flash"
        tag_text = "Pro" if "pro" in name.lower() else "Flash"
        ratio = (tok / total_tokens * 100) if total_tokens > 0 else 0
        table_rows += f"""
          <tr>
            <td>
              <span class="model-name">{name}</span>
              <span class="tag {tag_class}">{tag_text}</span>
            </td>
            <td class="metric">{req:,}</td>
            <td class="metric">{tok:,}</td>
            <td class="metric-secondary">{ratio:.2f}%</td>
          </tr>"""

    html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <title>DeepSeek 用量监控报告</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    * {{ margin: 0; padding: 0; box-sizing: border-box; }}
    body {{
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      background: #f5f7fa;
      color: #1a1a1a;
      padding: 40px 20px;
    }}
    .container {{
      max-width: 960px;
      margin: 0 auto;
    }}
    h1 {{
      font-size: 24px;
      font-weight: 600;
      margin-bottom: 8px;
      color: #1a1a1a;
    }}
    .subtitle {{
      font-size: 13px;
      color: #888;
      margin-bottom: 32px;
    }}
    .overview {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 16px;
      margin-bottom: 32px;
    }}
    .card {{
      background: #fff;
      border-radius: 12px;
      padding: 20px 24px;
      box-shadow: 0 1px 3px rgba(0,0,0,0.06);
      border: 1px solid #e8e8e8;
    }}
    .card-label {{
      font-size: 13px;
      color: #888;
      margin-bottom: 8px;
    }}
    .card-value {{
      font-size: 28px;
      font-weight: 600;
      color: #1a1a1a;
    }}
    .card-value.red {{ color: #e53935; }}
    .card-unit {{
      font-size: 14px;
      color: #888;
      margin-left: 4px;
    }}
    .chart-section {{
      background: #fff;
      border-radius: 12px;
      padding: 24px;
      box-shadow: 0 1px 3px rgba(0,0,0,0.06);
      border: 1px solid #e8e8e8;
      margin-bottom: 24px;
    }}
    .chart-title {{
      font-size: 16px;
      font-weight: 600;
      margin-bottom: 20px;
      color: #1a1a1a;
    }}
    .chart-row {{
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(400px, 1fr));
      gap: 24px;
    }}
    .chart-box {{
      position: relative;
      height: 280px;
    }}
    .detail-section {{
      background: #fff;
      border-radius: 12px;
      padding: 24px;
      box-shadow: 0 1px 3px rgba(0,0,0,0.06);
      border: 1px solid #e8e8e8;
    }}
    table {{
      width: 100%;
      border-collapse: collapse;
      font-size: 14px;
    }}
    th {{
      text-align: left;
      padding: 12px 16px;
      color: #888;
      font-weight: 500;
      border-bottom: 1px solid #e8e8e8;
      font-size: 13px;
    }}
    td {{
      padding: 16px;
      border-bottom: 1px solid #f0f0f0;
    }}
    tr:last-child td {{ border-bottom: none; }}
    .model-name {{
      font-weight: 600;
      color: #1a1a1a;
    }}
    .metric {{
      font-size: 16px;
      font-weight: 500;
      color: #1a1a1a;
    }}
    .metric-secondary {{
      font-size: 13px;
      color: #888;
    }}
    .tag {{
      display: inline-block;
      padding: 2px 8px;
      border-radius: 4px;
      font-size: 12px;
      font-weight: 500;
      margin-left: 8px;
    }}
    .tag-pro {{ background: #fff3e0; color: #e65100; }}
    .tag-flash {{ background: #e3f2fd; color: #1565c0; }}
  </style>
</head>
<body>
  <div class="container">
    <h1>DeepSeek 用量监控</h1>
    <p class="subtitle">数据来自 platform.deepseek.com · {year}年{month} · 按 UTC+0 时间</p>

    <div class="overview">
      <div class="card">
        <div class="card-label">充值余额</div>
        <div class="card-value">¥{balance}<span class="card-unit">{currency}</span></div>
      </div>
      <div class="card">
        <div class="card-label">{month}消费</div>
        <div class="card-value red">¥{cost}<span class="card-unit">{currency}</span></div>
      </div>
      <div class="card">
        <div class="card-label">总 API 请求</div>
        <div class="card-value">{total_requests:,}<span class="card-unit">次</span></div>
      </div>
      <div class="card">
        <div class="card-label">总 Tokens</div>
        <div class="card-value">{total_tokens/1_000_000:.2f}M<span class="card-unit">Tokens</span></div>
      </div>
    </div>

    {screenshot_section}

    <div class="chart-section">
      <div class="chart-title">模型对比</div>
      <div class="chart-row">
        <div class="chart-box">
          <canvas id="requestsChart"></canvas>
        </div>
        <div class="chart-box">
          <canvas id="tokensChart"></canvas>
        </div>
      </div>
    </div>

    <div class="detail-section">
      <div class="chart-title">模型明细</div>
      <table>
        <thead>
          <tr>
            <th>模型</th>
            <th>API 请求次数</th>
            <th>Tokens</th>
            <th>占比</th>
          </tr>
        </thead>
        <tbody>
          {table_rows}
        </tbody>
      </table>
    </div>
  </div>

  <script>
    const chartColors = {{
      pro: 'rgba(229, 57, 53, 0.8)',
      proBg: 'rgba(229, 57, 53, 0.15)',
      flash: 'rgba(33, 150, 243, 0.8)',
      flashBg: 'rgba(33, 150, 243, 0.15)',
    }};

    const bgColors = {model_names}.map(n => n.includes('pro') ? chartColors.proBg : chartColors.flashBg);
    const borderColors = {model_names}.map(n => n.includes('pro') ? chartColors.pro : chartColors.flash);

    new Chart(document.getElementById('requestsChart'), {{
      type: 'bar',
      data: {{
        labels: {json.dumps(model_names)},
        datasets: [{{
          label: 'API 请求次数',
          data: {requests_data},
          backgroundColor: bgColors,
          borderColor: borderColors,
          borderWidth: 2,
          borderRadius: 6,
          barThickness: 60,
        }}]
      }},
      options: {{
        responsive: true,
        maintainAspectRatio: false,
        plugins: {{
          legend: {{ display: false }},
          tooltip: {{
            callbacks: {{
              label: ctx => ctx.raw.toLocaleString() + ' 次'
            }}
          }}
        }},
        scales: {{
          y: {{
            beginAtZero: true,
            grid: {{ color: '#f0f0f0' }},
            ticks: {{ color: '#888', font: {{ size: 12 }} }}
          }},
          x: {{
            grid: {{ display: false }},
            ticks: {{ color: '#555', font: {{ size: 13, weight: '500' }} }}
          }}
        }}
      }}
    }});

    new Chart(document.getElementById('tokensChart'), {{
      type: 'bar',
      data: {{
        labels: {json.dumps(model_names)},
        datasets: [{{
          label: 'Tokens',
          data: {tokens_data},
          backgroundColor: bgColors,
          borderColor: borderColors,
          borderWidth: 2,
          borderRadius: 6,
          barThickness: 60,
        }}]
      }},
      options: {{
        responsive: true,
        maintainAspectRatio: false,
        plugins: {{
          legend: {{ display: false }},
          tooltip: {{
            callbacks: {{
              label: ctx => ctx.raw.toLocaleString() + ' Tokens'
            }}
          }}
        }},
        scales: {{
          y: {{
            beginAtZero: true,
            grid: {{ color: '#f0f0f0' }},
            ticks: {{
              color: '#888',
              font: {{ size: 12 }},
              callback: val => {{
                if (val >= 1000000) return (val / 1000000).toFixed(1) + 'M';
                if (val >= 1000) return (val / 1000).toFixed(0) + 'K';
                return val;
              }}
            }}
          }},
          x: {{
            grid: {{ display: false }},
            ticks: {{ color: '#555', font: {{ size: 13, weight: '500' }} }}
          }}
        }}
      }}
    }});
  </script>
</body>
</html>"""

    Path(output_path).write_text(html, encoding="utf-8")
    print(f"Report generated: {output_path}")


def main():
    parser = argparse.ArgumentParser(description="DeepSeek 用量报告生成器")
    parser.add_argument("--data", required=True, help="JSON 数据文件路径")
    parser.add_argument("--output", required=True, help="输出 HTML 文件路径")
    parser.add_argument("--screenshot", default=None, help="月度用量截图路径 (可选)")
    args = parser.parse_args()

    with open(args.data, "r", encoding="utf-8") as f:
        data = json.load(f)

    generate_report(data, args.output, args.screenshot)


if __name__ == "__main__":
    main()
