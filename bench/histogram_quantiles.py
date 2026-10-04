#!/usr/bin/env python3
"""从 Prometheus 文本格式里算某个直方图的分位数（TASK-022）。

为什么不只在 Prometheus 侧算：
    `rgbt_http_request_seconds` 的直方图**不带 path 标签**（指标层刻意避免高基数，
    见 metrics.hpp 的说明），因此 Prometheus 给出的 P95 是"所有业务端点混在一起"
    的数字。要说清"是哪个端点慢"，只能从端点原始文本按 `path` 分组再分别算。
    两者都要：混在一起的那个用来对 SLO，分组后的用来定位瓶颈。

输入：`/metrics` 的原始文本（或文件）。输出：每个 (指标, 标签集) 一行百分位。
用法：python3 bench/histogram_quantiles.py <metrics-file> [指标名...]
"""

from __future__ import annotations

import sys
from typing import Dict, List, Tuple

DEFAULT_METRICS = ("rgbt_http_request_seconds",)


def parse_samples(path: str) -> List[Tuple[str, Dict[str, str], float]]:
    samples: List[Tuple[str, Dict[str, str], float]] = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            space = line.rfind(" ")
            if space < 0:
                continue
            name_part, value_part = line[:space], line[space + 1 :]
            try:
                value = float(value_part)
            except ValueError:
                continue
            if "{" in name_part:
                name, _, rest = name_part.partition("{")
                labels_text = rest.rstrip("}")
                labels: Dict[str, str] = {}
                for item in labels_text.split(","):
                    key, _, raw = item.partition("=")
                    labels[key.strip()] = raw.strip().strip('"')
            else:
                name, labels = name_part, {}
            samples.append((name, labels, value))
    return samples


def quantile_from_buckets(buckets: List[Tuple[float, float]], total: float, q: float) -> float:
    """按 Prometheus 的直方图约定插值求分位数。

    `buckets` 是 (le, 累计计数) 的升序列表（不含 +Inf，+Inf 由 `total` 给出）。
    """
    if total <= 0:
        return float("nan")
    target = q * total
    previous_le = 0.0
    previous_count = 0.0
    for le, cumulative in buckets:
        if cumulative >= target:
            span = cumulative - previous_count
            if span <= 0:
                return le
            # 桶内线性插值：这是 Prometheus histogram_quantile 的做法，
            # 保持"报告里的分位数与面板一致"。
            return previous_le + (le - previous_le) * (target - previous_count) / span
        previous_le, previous_count = le, cumulative
    return float("inf")


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = argv[1]
    wanted = set(argv[2:]) or set(DEFAULT_METRICS)

    samples = parse_samples(path)
    # 按 (指标名, 除 le 以外的标签) 分组收集桶。
    groups: Dict[Tuple[str, Tuple[Tuple[str, str], ...]], Dict[str, object]] = {}
    for name, labels, value in samples:
        if "_bucket" in name:
            base = name.replace("_bucket", "")
            if base not in wanted:
                continue
            le_text = labels.get("le")
            if le_text is None:
                continue
            key_labels = tuple(sorted((k, v) for k, v in labels.items() if k != "le"))
            group = groups.setdefault((base, key_labels), {"buckets": [], "count": 0.0, "sum": 0.0})
            if le_text == "+Inf":
                group["count"] = value
            else:
                group["buckets"].append((float(le_text), value))
        elif name.endswith("_count") or name.endswith("_sum"):
            base = name.rsplit("_", 1)[0]
            if base not in wanted:
                continue
            key_labels = tuple(sorted(labels.items()))
            group = groups.setdefault((base, key_labels), {"buckets": [], "count": 0.0, "sum": 0.0})
            group["count" if name.endswith("_count") else "sum"] = value

    for (base, key_labels), group in sorted(groups.items()):
        buckets = sorted(group["buckets"])  # type: ignore[arg-type]
        total = float(group["count"])  # type: ignore[arg-type]
        label_text = ",".join(f"{k}={v}" for k, v in key_labels) or "-"
        parts = [f"{base}{{{label_text}}}", f"count={int(total)}"]
        for q in (0.5, 0.9, 0.95, 0.99):
            seconds = quantile_from_buckets(buckets, total, q)
            parts.append(f"p{int(q * 100)}={seconds * 1000:.2f}ms")
        parts.append(f"sum={float(group['sum']):.3f}s")  # type: ignore[arg-type]
        print("  " + "  ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
