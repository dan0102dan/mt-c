#!/usr/bin/env python3
"""Rebuild README router charts in the original borderless, theme-aware SVG style.

Requires only Python 3 stdlib. Reads original JSONL (not the historical
host-only baseline) and plots median + min/max for three UDP capacity runs.
RAM-guarded cases are marked as unmeasured instead of plotting zeros.
"""

import argparse
import json
import math
import statistics
from pathlib import Path
from xml.sax.saxutils import escape


# Borderless, theme-aware plots based on the earlier README charts. The
# logarithmic paper grid (decades + subdivisions 2..9) replaces textual
# "log" qualifiers so the scale is readable on sight.
BLUE = "#4f73c6"
PALE_BLUE = "#c8d9fa"
LEFT, RIGHT, TOP, BOTTOM = 150, 1120, 218, 550
X_MIN, X_MAX = 85, 130000
LABELED_RULES = {100, 1000, 10000, 20000}


def logarithmic_divisions(low, high):
    """(coordinate, tier) for full log-paper 1..9 divisions of each decade."""
    for power in range(math.floor(math.log10(low)) - 1,
                       math.ceil(math.log10(high)) + 1):
        decade = 10 ** power
        for digit in range(1, 10):
            position = digit * decade
            if low <= position <= high:
                tier = "major" if digit == 1 else "mid" if digit in (2, 5) else "minor"
                yield position, tier

# Keep the existing measurement units and log-scaled axes: the real-router
# results span orders of magnitude, unlike the three-point historic plots.
CHARTS = (
    ("rps", "Пропускная способность DNS", "Успешные запросы / с",
     "benchmark-throughput.svg", 15, 1300, (20, 50, 100, 200, 500, 1000)),
    ("cost", "CPU на один успешный DNS-запрос", "CPU демона, мс / запрос",
     "benchmark-cpu.svg", 0.55, 160, (1, 2, 5, 10, 20, 50, 100)),
    ("rss", "Потребление памяти (RSS)", "RSS демона, MiB",
     "benchmark-memory.svg", 2, 40, (3, 5, 10, 20, 30)),
)


def num(n):
    """Stable, compact numeric SVG attributes."""
    return f"{n:.3f}".rstrip("0").rstrip(".")


def text(x, y, label, css="", extra=""):
    return (f'<text x="{num(x)}" y="{num(y)}" class="{css}" {extra}>'
            f'{escape(str(label))}</text>')


def measured(row, kind):
    if kind == "rps":
        return row["rps"]
    if kind == "rss":
        return row["after"]["VmRSS"] / 1024
    # CPU pct: 100% means one full logical core. Successful answers only in
    # denominator, while CPU also includes processing failed requests.
    return (row["cpu_pct"] / 100 * row["cpu_interval_sec"]
            / row["ok"] * 1000)


def readable_value(kind, value):
    if kind == "rps":
        return f"{value:.0f}"
    if kind == "cost":
        return f"{value:.2f}" if value < 10 else f"{value:.1f}"
    return f"{value:.1f}"


def draw_chart(loads, failures, kind, title, ylabel, min_y, max_y, y_ticks):
    def x_pos(value):
        return LEFT + math.log(value / X_MIN) / math.log(X_MAX / X_MIN) * (RIGHT - LEFT)

    def y_pos(value):
        return BOTTOM - math.log(value / min_y) / math.log(max_y / min_y) * (BOTTOM - TOP)

    svg = []
    put = svg.append
    put('<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="553" '
        'viewBox="0 0 1200 553" role="img" aria-labelledby="title desc">')
    put(f'  <title id="title">{escape(title)} — mt-c и MagiTrickle</title>')
    put('  <desc id="desc">Медиана, min–max трёх двадцатисекундных прогонов. '
        'Нагрузка реального роутера MT7621; UDP, 10 клиентов; логарифмические оси. '
        'Точки на 50000 и 100000 правилах остановлены защитой памяти и не измерены.</desc>')
    put('''  <style>
    :root {
      color-scheme: light dark;
      --ink: #424750;
      --muted: #6b7280;
      --axis: #59606b;
      --grid-major: #bac5d1;
      --grid-mid: #d7dee7;
      --grid-minor: #eef1f4;
      --blue: #4f73c6;
      --pale-blue: #8ea9d9;
    }
    @media (prefers-color-scheme: dark) {
      :root {
        --ink: #c9cdd3;
        --muted: #a6abb3;
        --axis: #b5bac2;
        --grid-major: #505965;
        --grid-mid: #404854;
        --grid-minor: #303740;
        --blue: #4f73c6;
        --pale-blue: #c8d9fa;
      }
    }
    text { font-family: Inter, ui-sans-serif, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; fill: var(--ink); }
    .axis-label { font-size: 17px; font-weight: 600; }
    .tick { font-size: 16px; fill: var(--ink); }
    .legend { font-size: 18px; font-weight: 600; }
    .value { font-size: 15px; font-weight: 700; }
    .axis { stroke: var(--axis); }
    .grid-major { stroke: var(--grid-major); stroke-width: 1.35; }
    .grid-mid { stroke: var(--grid-mid); stroke-width: 1.05; }
    .grid-minor { stroke: var(--grid-minor); stroke-width: 0.85; }
    .c-line { stroke: var(--blue); fill: none; }
    .go-line { stroke: var(--pale-blue); fill: none; }
    .c-point { fill: var(--blue); }
    .go-point { fill: var(--pale-blue); }
    .c-whisker { stroke: var(--blue); }
    .go-whisker { stroke: var(--pale-blue); }
  </style>''')
    # Preserve a transparent background and the original unframed style.
    # Two independent translations reduce the large empty header and the
    # gap between legend and plot without rescaling any measured value.
    put('<g transform="translate(0 -75)">')
    put('<circle cx="80" cy="116" r="7" class="c-point"/>')
    put(text(97, 122, "mt-c", "legend"))
    put('<circle cx="208" cy="116" r="7" class="go-point"/>')
    put(text(225, 122, "Go MagiTrickle 0.8.2", "legend"))
    put(text(590, 122, "×", "legend"))
    put(text(615, 122, "Остановлено по памяти", "legend"))
    put('</g>')
    put('<g transform="translate(0 -95)">')

    # Logarithmic paper: every 10x decade is a strong line, and each decade
    # has 2..9 subdivisions. Slightly stronger 2x/5x lines aid interpolation.
    put('<g aria-hidden="true">')
    for value, tier in logarithmic_divisions(X_MIN, X_MAX):
        xx = num(x_pos(value))
        put(f'  <line class="grid-{tier}" x1="{xx}" y1="{TOP}" '
            f'x2="{xx}" y2="{BOTTOM}"/>')
    for value, tier in logarithmic_divisions(min_y, max_y):
        yy = num(y_pos(value))
        put(f'  <line class="grid-{tier}" x1="{LEFT}" y1="{yy}" '
            f'x2="{RIGHT}" y2="{yy}"/>')
    put('</g>')

    # Axis strokes and minor/major ticks live above the grid.
    put(f'<line class="axis" x1="{LEFT}" y1="{TOP}" x2="{LEFT}" y2="{BOTTOM}" stroke-width="2"/>')
    put(f'<line class="axis" x1="{LEFT}" y1="{BOTTOM}" x2="{RIGHT}" y2="{BOTTOM}" stroke-width="2"/>')
    put('<g class="axis" stroke-width="1.1" aria-hidden="true">')
    for value, tier in logarithmic_divisions(X_MIN, X_MAX):
        xx = num(x_pos(value))
        tick_length = 9 if tier == "major" else 6 if tier == "mid" else 3
        put(f'  <line x1="{xx}" y1="{BOTTOM}" x2="{xx}" '
            f'y2="{BOTTOM+tick_length}"/>')
    for value, tier in logarithmic_divisions(min_y, max_y):
        yy = num(y_pos(value))
        tick_length = 9 if tier == "major" else 6 if tier == "mid" else 3
        put(f'  <line x1="{LEFT-tick_length}" y1="{yy}" '
            f'x2="{LEFT}" y2="{yy}"/>')
    put('</g>')
    put('<g class="tick" text-anchor="end">')
    for tick in y_ticks:
        put('  ' + text(132, y_pos(tick) + 5, f"{tick:g}"))
    put('</g>')
    put('<g class="tick" text-anchor="middle">')
    for tick, label in ((100, "100"), (1000, "1 000"),
                        (10000, "10 000"), (100000, "100 000")):
        put('  ' + text(x_pos(tick), 583, label))
    put('</g>')
    put(text(635, 620, "Количество правил", "axis-label", 'text-anchor="middle"'))
    put(text(56, 384, ylabel, "axis-label",
             'text-anchor="middle" transform="rotate(-90 56 384)"'))

    for backend in ("original", "mt-c"):
        line_class = "go-line" if backend == "original" else "c-line"
        point_class = "go-point" if backend == "original" else "c-point"
        whisker_class = "go-whisker" if backend == "original" else "c-whisker"
        sizes = sorted({r["rules"] for r in loads if r["backend"] == backend})
        points = []
        for size in sizes:
            trials = [r for r in loads if r["backend"] == backend and r["rules"] == size]
            if len(trials) != 3:
                raise ValueError(f"expected three repeats for {backend}, {size}")
            values = sorted(measured(r, kind) for r in trials)
            points.append((size, values[0], statistics.median(values), values[-1]))
        put(f'<g class="{whisker_class}" stroke-width="1.7" opacity="0.65" fill="none">')
        for size, minimum, _, maximum in points:
            xx, high, low = x_pos(size), y_pos(maximum), y_pos(minimum)
            put(f'<path d="M{num(xx)} {num(high)} V{num(low)} '
                f'M{num(xx-4)} {num(high)} H{num(xx+4)} '
                f'M{num(xx-4)} {num(low)} H{num(xx+4)}"/>')
        put('</g>')
        d = ' '.join(("M" if i == 0 else "L") + f'{num(x_pos(size))} {num(y_pos(mid))}'
                     for i, (size, _, mid, _) in enumerate(points))
        put(f'<path d="{d}" class="{line_class}" fill="none" '
            'stroke-width="4.5" stroke-linecap="round" stroke-linejoin="round"/>')
        put(f'<g class="{point_class}">')
        for size, _, median, _ in points:
            put(f'<circle cx="{num(x_pos(size))}" cy="{num(y_pos(median))}" r="7"/>')
        put('</g>')
        # Like the original charts, annotate medians, but use key points only
        # so 8-point measured series remain readable without label collisions.
        put('<g class="value" text-anchor="middle">')
        for size, _, median, _ in points:
            if size not in LABELED_RULES:
                continue
            # Values outside the denser side of each chart reduce overlap.
            if kind == "rps":
                below = backend == "original"
            else:
                below = backend == "mt-c"
            offset = 28 if below else -19
            put('  ' + text(x_pos(size), y_pos(median) + offset,
                             readable_value(kind, median)))
        put('</g>')

    # Position RAM markers *outside* the plot, to avoid implying a zero rate,
    # CPU time or RSS at rules where both processes were stopped.
    failure_sizes = sorted({r["rules"] for r in failures})
    for size in failure_sizes:
        xx = x_pos(size)
        put(f'<path d="M{num(xx-5)} 186 l10 10 m-10 0 l10 -10" '
            'stroke="var(--axis)" stroke-width="2" fill="none"/>')

    put('</g>')
    put('</svg>')
    return '\n'.join(svg) + '\n'


def plot(sources, output):
    measurements = [json.loads(line) for path in sources
                    for line in path.read_text(encoding="utf-8").splitlines()
                    if line.strip()]
    failures = [r for r in measurements if r["metric"] == "failure"
                and r["scenario"] == "capacity" and r["proto"] == "udp"]
    rows = [r for r in measurements if r["metric"] == "load"
            and r["scenario"] == "capacity" and r["proto"] == "udp"]
    sizes = sorted({r["rules"] for r in rows + failures})
    for backend in ("original", "mt-c"):
        for size in sizes:
            count = sum(r["backend"] == backend and r["rules"] == size for r in rows)
            failed = any(r["backend"] == backend and r["rules"] == size for r in failures)
            if count != 3 and not (count == 0 and failed):
                raise ValueError(f"missing/incomplete samples: {backend}, {size} (got {count})")
    output.mkdir(parents=True, exist_ok=True)
    for kind, title, ylabel, filename, min_y, max_y, ticks in CHARTS:
        (output / filename).write_text(
            draw_chart(rows, failures, kind, title, ylabel, min_y, max_y, ticks),
            encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=Path("img"))
    args = parser.parse_args()
    plot(args.results, args.output)
