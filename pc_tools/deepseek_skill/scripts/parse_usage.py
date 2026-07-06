#!/usr/bin/env python3
"""
DeepSeek 用量页面文本解析器
从 CDP 提取的 document.body.innerText 中解析结构化数据

用法:
  python parse_usage.py --input raw_text.txt --output data.json
"""

import json
import argparse
import re
import sys


def parse_usage_text(text: str) -> dict:
    """从 DeepSeek 用量页面的 innerText 解析数据"""

    result = {
        "balance": "0.00",
        "currency": "CNY",
        "monthly_cost": "0.00",
        "month": "",
        "year": "",
        "models": []
    }

    # 解析余额
    balance_match = re.search(r'充值余额\s*\n¥([\d.]+)\n(CNY|USD)', text)
    if balance_match:
        result["balance"] = balance_match.group(1)
        result["currency"] = balance_match.group(2)

    # 解析消费和月份
    cost_match = re.search(r'(\d+)\s*-\s*(\d+)月消费.*?\n¥([\d.]+)\n', text)
    if cost_match:
        result["year"] = cost_match.group(1)
        result["month"] = cost_match.group(2) + "月"
        result["monthly_cost"] = cost_match.group(3)

    # 解析模型数据
    # 文本格式：模型名\nAPI 请求次数\n数值\n...\nTokens\n数值\n...
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        # 检测模型名（deepseek-v4-pro 或 deepseek-v4-flash）
        if line.startswith('deepseek-v4'):
            model_name = line
            model_data = {"name": model_name, "requests": 0, "tokens": 0}

            # 查找请求次数
            j = i + 1
            while j < len(lines) and j < i + 10:
                if lines[j].strip() == 'API 请求次数':
                    # 下一个非空行是数值
                    k = j + 1
                    while k < len(lines) and lines[k].strip() == '':
                        k += 1
                    if k < len(lines):
                        val_str = lines[k].strip().replace(',', '')
                        try:
                            model_data["requests"] = int(val_str)
                        except ValueError:
                            pass
                    break
                j += 1

            # 查找 Tokens
            j = i + 1
            while j < len(lines) and j < i + 15:
                if lines[j].strip() == 'Tokens':
                    k = j + 1
                    while k < len(lines) and lines[k].strip() == '':
                        k += 1
                    if k < len(lines):
                        val_str = lines[k].strip().replace(',', '')
                        try:
                            model_data["tokens"] = int(val_str)
                        except ValueError:
                            pass
                    break
                j += 1

            result["models"].append(model_data)
            i += 1
        else:
            i += 1

    return result


def main():
    parser = argparse.ArgumentParser(description="DeepSeek 用量文本解析器")
    parser.add_argument("--input", required=True, help="输入文本文件路径")
    parser.add_argument("--output", required=True, help="输出 JSON 文件路径")
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        text = f.read()

    data = parse_usage_text(text)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

    print(f"✅ 解析完成: {args.output}")
    print(json.dumps(data, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
