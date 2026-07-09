#!/usr/bin/env python3
"""
Render obmm_addr_hash_probe logs as a standalone HTML/SVG report.

The script intentionally uses only the Python standard library so it can run on
target hosts without matplotlib.
"""

import argparse
import html
import math
import os
import re
import statistics
import sys


STEP_PREFIX = "OBMM_ADDR_HASH_STEP"
CONFIG_PREFIX = "OBMM_ADDR_HASH_PROBE_CONFIG"
BASELINE_PREFIX = "OBMM_ADDR_HASH_BASELINE"
PAIR_RE = re.compile(r"(\w+)=([^ ]+)")

MATCH_KEYS = ["match64", "match4k", "match64k", "match512k", "match1m", "match2m"]
TOP_COLUMNS = [
    "step",
    "aggr_offset",
    "first_xor_low2m",
    "match64",
    "match4k",
    "match64k",
    "match512k",
    "match1m",
    "match2m",
    "victim_ns_per_op",
    "slowdown",
    "aggr_ops_per_sec",
]


def parse_scalar(value):
    if value.startswith(("0x", "0X")):
        try:
            return int(value, 16)
        except ValueError:
            return value

    if any(ch in value for ch in ".eE"):
        try:
            return float(value)
        except ValueError:
            return value

    try:
        return int(value, 10)
    except ValueError:
        return value


def parse_pairs(line):
    return {key: parse_scalar(value) for key, value in PAIR_RE.findall(line)}


def parse_log(path):
    rows = []
    config = {}
    baseline = {}

    with open(path, "r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            line = line.strip()
            if line.startswith(STEP_PREFIX):
                rows.append(parse_pairs(line))
            elif line.startswith(CONFIG_PREFIX):
                config.update(parse_pairs(line))
            elif line.startswith(BASELINE_PREFIX):
                baseline.update(parse_pairs(line))

    return config, baseline, rows


def as_float(row, key, default=0.0):
    value = row.get(key, default)
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def as_int(row, key, default=0):
    value = row.get(key, default)
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def fmt_num(value, digits=3):
    if value is None:
        return "n/a"
    if isinstance(value, str):
        return value
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        return "n/a"
    if abs(value) >= 1000.0 or (0.0 < abs(value) < 0.001):
        return f"{value:.{digits}e}"
    return f"{value:.{digits}f}"


def fmt_field(value):
    if isinstance(value, int) and value > 4096:
        return f"{value} / 0x{value:x}"
    return html.escape(str(value))


def pearson(rows, x_key, y_key):
    xs = [as_float(row, x_key, math.nan) for row in rows]
    ys = [as_float(row, y_key, math.nan) for row in rows]
    pairs = [(x, y) for x, y in zip(xs, ys) if math.isfinite(x) and math.isfinite(y)]
    if len(pairs) < 2:
        return None

    xs, ys = zip(*pairs)
    x_mean = statistics.fmean(xs)
    y_mean = statistics.fmean(ys)
    x_var = sum((x - x_mean) ** 2 for x in xs)
    y_var = sum((y - y_mean) ** 2 for y in ys)
    if x_var == 0.0 or y_var == 0.0:
        return None
    cov = sum((x - x_mean) * (y - y_mean) for x, y in pairs)
    return cov / math.sqrt(x_var * y_var)


def metric_bounds(rows, key, zero_min=True):
    values = [as_float(row, key, math.nan) for row in rows]
    values = [value for value in values if math.isfinite(value)]
    if not values:
        return 0.0, 1.0

    low = min(values)
    high = max(values)
    if zero_min and low > 0:
        low = 0.0
    if low == high:
        pad = max(abs(low) * 0.1, 1.0)
        return low - pad, high + pad

    pad = (high - low) * 0.08
    return low - pad, high + pad


def scale(value, low, high, out_low, out_high):
    if high == low:
        return (out_low + out_high) / 2.0
    return out_low + ((value - low) / (high - low)) * (out_high - out_low)


def color_for(value, max_value):
    if max_value <= 0:
        return "#2f6fbd"
    t = max(0.0, min(1.0, float(value) / float(max_value)))
    r = int(48 + (205 - 48) * t)
    g = int(111 + (61 - 111) * t)
    b = int(189 + (52 - 189) * t)
    return f"#{r:02x}{g:02x}{b:02x}"


def tick_values(low, high, count=5):
    if count <= 1:
        return [low]
    return [low + (high - low) * i / (count - 1) for i in range(count)]


def fmt_tick(value, hex_mode=False):
    if hex_mode:
        return f"0x{int(round(value)):x}"
    if abs(value) >= 100000:
        return f"{value:.2e}"
    if abs(value - round(value)) < 1e-6:
        return str(int(round(value)))
    return f"{value:.2f}"


def svg_axes(width, height, margin, x_low, x_high, y_low, y_high, x_hex=False):
    left, top, right, bottom = margin
    plot_w = width - left - right
    plot_h = height - top - bottom
    out = []

    out.append(f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" class="axis"/>')
    out.append(f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" class="axis"/>')

    for tick in tick_values(x_low, x_high):
        x = scale(tick, x_low, x_high, left, left + plot_w)
        out.append(f'<line x1="{x:.2f}" y1="{top + plot_h}" x2="{x:.2f}" y2="{top + plot_h + 5}" class="tick"/>')
        out.append(
            f'<text x="{x:.2f}" y="{top + plot_h + 20}" text-anchor="middle" class="tick-label">'
            f'{html.escape(fmt_tick(tick, x_hex))}</text>'
        )

    for tick in tick_values(y_low, y_high):
        y = scale(tick, y_low, y_high, top + plot_h, top)
        out.append(f'<line x1="{left - 5}" y1="{y:.2f}" x2="{left}" y2="{y:.2f}" class="tick"/>')
        out.append(
            f'<text x="{left - 8}" y="{y + 4:.2f}" text-anchor="end" class="tick-label">'
            f'{html.escape(fmt_tick(tick))}</text>'
        )

    return "\n".join(out)


def svg_metric_by_x(rows, x_key, y_key, color_key, title, x_label, y_label, x_hex=False):
    width, height = 980, 360
    margin = (78, 38, 30, 58)
    left, top, right, bottom = margin
    plot_w = width - left - right
    plot_h = height - top - bottom

    data = []
    for row in rows:
        x = as_float(row, x_key, math.nan)
        y = as_float(row, y_key, math.nan)
        c = as_int(row, color_key, 0)
        if math.isfinite(x) and math.isfinite(y):
            data.append((x, y, c, row))

    if not data:
        return "<p>No plottable data.</p>"

    x_low = min(point[0] for point in data)
    x_high = max(point[0] for point in data)
    if x_low == x_high:
        x_low -= 1
        x_high += 1
    y_low, y_high = metric_bounds(rows, y_key)
    max_color = max(point[2] for point in data)

    points = []
    circles = []
    for x, y, color_value, row in data:
        sx = scale(x, x_low, x_high, left, left + plot_w)
        sy = scale(y, y_low, y_high, top + plot_h, top)
        points.append(f"{sx:.2f},{sy:.2f}")
        title_text = " ".join(
            f"{key}={row.get(key)}"
            for key in ("step", "aggr_offset", "first_xor_low2m", y_key, color_key)
            if key in row
        )
        circles.append(
            f'<circle cx="{sx:.2f}" cy="{sy:.2f}" r="3.2" '
            f'fill="{color_for(color_value, max_color)}"><title>'
            f'{html.escape(title_text)}</title></circle>'
        )

    return f"""
<section class="chart">
  <h2>{html.escape(title)}</h2>
  <svg viewBox="0 0 {width} {height}" role="img">
    <text x="{width / 2:.1f}" y="22" text-anchor="middle" class="chart-title">{html.escape(title)}</text>
    {svg_axes(width, height, margin, x_low, x_high, y_low, y_high, x_hex)}
    <polyline points="{' '.join(points)}" class="metric-line"/>
    {''.join(circles)}
    <text x="{left + plot_w / 2:.1f}" y="{height - 10}" text-anchor="middle" class="axis-label">{html.escape(x_label)}</text>
    <text x="18" y="{top + plot_h / 2:.1f}" transform="rotate(-90 18,{top + plot_h / 2:.1f})" text-anchor="middle" class="axis-label">{html.escape(y_label)}</text>
    <text x="{width - right}" y="22" text-anchor="end" class="legend">color = {html.escape(color_key)}</text>
  </svg>
</section>
"""


def svg_match_lines(rows):
    width, height = 980, 340
    margin = (78, 34, 120, 55)
    left, top, right, bottom = margin
    plot_w = width - left - right
    plot_h = height - top - bottom

    steps = [as_float(row, "step", math.nan) for row in rows]
    steps = [step for step in steps if math.isfinite(step)]
    if not steps:
        return "<p>No match-counter data.</p>"

    x_low, x_high = min(steps), max(steps)
    if x_low == x_high:
        x_low -= 1
        x_high += 1
    y_high = max(max(as_int(row, key, 0) for row in rows) for key in MATCH_KEYS)
    y_low = 0
    if y_high <= 0:
        y_high = 1

    colors = {
        "match64": "#577590",
        "match4k": "#43aa8b",
        "match64k": "#90be6d",
        "match512k": "#f9c74f",
        "match1m": "#f8961e",
        "match2m": "#f94144",
    }

    lines = []
    legend = []
    for idx, key in enumerate(MATCH_KEYS):
        pts = []
        for row in rows:
            x = as_float(row, "step", math.nan)
            y = as_float(row, key, math.nan)
            if math.isfinite(x) and math.isfinite(y):
                sx = scale(x, x_low, x_high, left, left + plot_w)
                sy = scale(y, y_low, y_high, top + plot_h, top)
                pts.append(f"{sx:.2f},{sy:.2f}")
        lines.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{colors[key]}" stroke-width="1.8"/>')
        legend_y = top + 18 + idx * 19
        legend.append(f'<line x1="{width - right + 18}" y1="{legend_y}" x2="{width - right + 42}" y2="{legend_y}" stroke="{colors[key]}" stroke-width="2"/>')
        legend.append(f'<text x="{width - right + 48}" y="{legend_y + 4}" class="legend">{html.escape(key)}</text>')

    return f"""
<section class="chart">
  <h2>Match Counters By Step</h2>
  <svg viewBox="0 0 {width} {height}" role="img">
    <text x="{width / 2:.1f}" y="22" text-anchor="middle" class="chart-title">Match counters by step</text>
    {svg_axes(width, height, margin, x_low, x_high, y_low, y_high)}
    {''.join(lines)}
    {''.join(legend)}
    <text x="{left + plot_w / 2:.1f}" y="{height - 10}" text-anchor="middle" class="axis-label">step</text>
    <text x="18" y="{top + plot_h / 2:.1f}" transform="rotate(-90 18,{top + plot_h / 2:.1f})" text-anchor="middle" class="axis-label">matching aggressors</text>
  </svg>
</section>
"""


def table(headers, rows):
    head = "".join(f"<th>{html.escape(str(header))}</th>" for header in headers)
    body = []
    for row in rows:
        body.append("<tr>" + "".join(f"<td>{cell}</td>" for cell in row) + "</tr>")
    return f"<table><thead><tr>{head}</tr></thead><tbody>{''.join(body)}</tbody></table>"


def top_rows_table(rows, metric, top_n):
    ordered = sorted(rows, key=lambda row: as_float(row, metric, float("-inf")), reverse=True)
    body = []
    for row in ordered[:top_n]:
        body.append([fmt_field(row.get(col, "")) for col in TOP_COLUMNS])
    return table(TOP_COLUMNS, body)


def correlation_table(rows, metric):
    body = []
    for key in MATCH_KEYS:
        corr = pearson(rows, key, metric)
        values = [as_int(row, key, 0) for row in rows]
        max_value = max(values) if values else 0
        at_max = [as_float(row, metric, math.nan) for row in rows if as_int(row, key, -1) == max_value]
        other = [as_float(row, metric, math.nan) for row in rows if as_int(row, key, -1) != max_value]
        at_max = [value for value in at_max if math.isfinite(value)]
        other = [value for value in other if math.isfinite(value)]
        avg_max = statistics.fmean(at_max) if at_max else None
        avg_other = statistics.fmean(other) if other else None
        ratio = (avg_max / avg_other) if avg_max is not None and avg_other not in (None, 0.0) else None
        body.append([
            html.escape(key),
            str(max_value),
            str(len(at_max)),
            fmt_num(corr, 4),
            fmt_num(avg_max),
            fmt_num(avg_other),
            fmt_num(ratio),
        ])

    return table(
        ["counter", "max", "rows_at_max", f"corr({metric})", "avg_at_max", "avg_other", "ratio"],
        body,
    )


def key_value_table(title, mapping):
    if not mapping:
        return ""
    rows = [[html.escape(str(key)), fmt_field(value)] for key, value in sorted(mapping.items())]
    return f"<h2>{html.escape(title)}</h2>{table(['field', 'value'], rows)}"


def render_html(log_path, config, baseline, rows, metric, top_n):
    title = f"OBMM address-index probe: {os.path.basename(log_path)}"
    metric_label = metric
    top_metric = max(rows, key=lambda row: as_float(row, metric, float("-inf"))) if rows else {}

    summary_rows = [
        ["log", html.escape(log_path)],
        ["rows", str(len(rows))],
        ["metric", html.escape(metric)],
    ]
    if top_metric:
        summary_rows.extend([
            ["max_metric", fmt_num(as_float(top_metric, metric, math.nan))],
            ["max_step", fmt_field(top_metric.get("step", ""))],
            ["max_aggr_offset", fmt_field(top_metric.get("aggr_offset", ""))],
            ["max_first_xor_low2m", fmt_field(top_metric.get("first_xor_low2m", ""))],
            ["max_match2m", fmt_field(top_metric.get("match2m", ""))],
        ])

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{html.escape(title)}</title>
<style>
body {{
  margin: 24px;
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  color: #17202a;
  background: #f8fafc;
}}
h1 {{ font-size: 24px; margin: 0 0 16px; }}
h2 {{ font-size: 18px; margin: 22px 0 10px; }}
p {{ max-width: 980px; line-height: 1.45; }}
table {{
  border-collapse: collapse;
  font-size: 13px;
  margin: 8px 0 18px;
  background: white;
}}
th, td {{
  border: 1px solid #d8dee9;
  padding: 5px 8px;
  text-align: right;
  white-space: nowrap;
}}
th:first-child, td:first-child {{ text-align: left; }}
th {{ background: #edf2f7; }}
.chart {{
  margin: 18px 0;
  padding: 12px;
  background: white;
  border: 1px solid #d8dee9;
}}
svg {{ width: 100%; max-width: 1040px; height: auto; display: block; }}
.axis, .tick {{ stroke: #34495e; stroke-width: 1; }}
.tick-label {{ fill: #34495e; font-size: 11px; }}
.axis-label {{ fill: #17202a; font-size: 13px; font-weight: 600; }}
.chart-title {{ fill: #17202a; font-size: 15px; font-weight: 650; }}
.metric-line {{ fill: none; stroke: #111827; stroke-width: 1.4; opacity: 0.75; }}
.legend {{ fill: #34495e; font-size: 12px; }}
.note {{
  padding: 10px 12px;
  background: #fff8db;
  border: 1px solid #e9d985;
  max-width: 980px;
}}
</style>
</head>
<body>
<h1>{html.escape(title)}</h1>
<p class="note">Look for peaks in <code>{html.escape(metric)}</code> that line up with high match counters,
especially <code>match2m</code>, or with repeated <code>first_xor_low2m</code> buckets. A flat metric while
aggressor traffic is high weakens the PA-low-bit contention hypothesis.</p>
<h2>Summary</h2>
{table(['field', 'value'], summary_rows)}
{key_value_table('Config', config)}
{key_value_table('Baseline', baseline)}
{svg_metric_by_x(rows, 'step', metric, 'match2m', f'{metric_label} by scan step', 'step', metric_label)}
{svg_metric_by_x(rows, 'first_xor_low2m', metric, 'match2m', f'{metric_label} by first_xor_low2m', 'first_xor_low2m', metric_label, x_hex=True)}
{svg_match_lines(rows)}
<h2>Match Counter Statistics</h2>
{correlation_table(rows, metric)}
<h2>Top {top_n} Rows By {html.escape(metric)}</h2>
{top_rows_table(rows, metric, top_n)}
</body>
</html>
"""


def default_output_path(input_path):
    if input_path == "-":
        return "addr-hash-report.html"
    base, _ = os.path.splitext(input_path)
    return base + ".html"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", help="addr-hash log path")
    parser.add_argument("-o", "--output", help="output HTML path")
    parser.add_argument(
        "--metric",
        default="slowdown",
        choices=["slowdown", "victim_ns_per_op", "victim_ops_per_sec", "aggr_ops_per_sec"],
        help="metric to emphasize in the plots",
    )
    parser.add_argument("--top", type=int, default=30, help="rows in the slowest-points table")
    args = parser.parse_args(argv)

    config, baseline, rows = parse_log(args.log)
    if not rows:
        print(f"no {STEP_PREFIX} rows found in {args.log}", file=sys.stderr)
        return 1

    output = args.output or default_output_path(args.log)
    report = render_html(args.log, config, baseline, rows, args.metric, args.top)
    with open(output, "w", encoding="utf-8") as stream:
        stream.write(report)

    print(f"wrote {output} ({len(rows)} step rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
