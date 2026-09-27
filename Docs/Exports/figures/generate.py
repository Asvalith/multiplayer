"""Render the three evidence charts embedded in the Notion technical blog.

Uses Pillow for static plotting. Source values are parsed from the published
appendix and the sanitized RideMotion evidence, rather than copied into art.
Run with Python 3 and Pillow installed:
    python Docs/Exports/figures/generate.py
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from statistics import median

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
APPENDIX = ROOT / "Docs/Reports/2026-09-27/Appendix.md"
INERTIA = ROOT / "Tests/Evidence/2026-09-27/PlatformRide/inertia.json"

W, H = 1800, 1080
BG = "#F7FAFC"
INK = "#17304A"
MUTED = "#526A80"
GRID = "#D7E2EC"
BLUE = "#397BB7"
TEAL = "#16A39A"
ORANGE = "#DC8743"
PANEL = "#FFFFFF"


def font(size: int) -> ImageFont.FreeTypeFont:
    for path in (
        "C:/Windows/Fonts/NotoSansSC-VF.ttf",
        "C:/Windows/Fonts/msyh.ttc",
    ):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    raise RuntimeError("A Chinese font is required for these charts")


F = {size: font(size) for size in (20, 23, 25, 27, 28, 30, 32, 36, 40, 48, 52)}


def text(draw: ImageDraw.ImageDraw, xy: tuple[int, int], label: str,
         size: int, fill: str = INK, anchor: str | None = None) -> None:
    draw.text(xy, label, font=F[size], fill=fill, anchor=anchor)


def base(title: str, subtitle: str) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("RGB", (W, H), BG)
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((52, 42, 1748, 1038), radius=28, fill=PANEL)
    draw.rectangle((52, 42, 64, 1038), fill=TEAL)
    text(draw, (115, 76), title, 48)
    text(draw, (117, 153), subtitle, 28, MUTED)
    return image, draw


def appendix_rows() -> dict[int, list[str]]:
    rows: dict[int, list[str]] = {}
    in_performance_table = False
    for line in APPENDIX.read_text(encoding="utf-8").splitlines():
        if line.startswith("### B.1 "):
            in_performance_table = True
        elif line.startswith("### B.3 "):
            in_performance_table = False
        if not in_performance_table:
            continue
        match = re.match(r"^\|\s*(\d{3})\s*\|", line)
        if match:
            number = int(match.group(1))
            if 1 <= number <= 42:
                rows[number] = [cell.strip() for cell in line.strip("|").split("|")]
    assert len(rows) == 42, "Appendix B.1/B.2 must contain 42 performance rows"
    return rows


def plot_grid(draw: ImageDraw.ImageDraw, x0: int, x1: int, top: int,
              bottom: int, maximum: float, ticks: list[float],
              formatter, axis_title: str) -> None:
    text(draw, (x0, top - 63), axis_title, 27, INK)
    for value in ticks:
        y = round(bottom - (value / maximum) * (bottom - top))
        draw.line((x0, y, x1, y), fill=GRID, width=2)
        text(draw, (x0 - 22, y), formatter(value), 25, MUTED, "rm")


def bar(draw: ImageDraw.ImageDraw, center: int, value: float,
        maximum: float, top: int, bottom: int, color: str,
        width: int = 100) -> int:
    y = round(bottom - (value / maximum) * (bottom - top))
    draw.rounded_rectangle((center - width // 2, y, center + width // 2, bottom),
                           radius=10, fill=color)
    return y


def legend(draw: ImageDraw.ImageDraw, entries: list[tuple[str, str]], x: int, y: int) -> None:
    for label, color in entries:
        draw.rounded_rectangle((x, y + 7, x + 27, y + 34), radius=5, fill=color)
        text(draw, (x + 42, y), label, 28)
        x += 230


def footer(draw: ImageDraw.ImageDraw, lead: str, note: str, source: str) -> None:
    draw.rounded_rectangle((118, 891, 1685, 967), radius=16, fill="#EAF4F6")
    text(draw, (145, 910), lead, 30)
    text(draw, (119, 982), note, 23, MUTED)
    text(draw, (119, 1011), source, 20, MUTED)


def static_plate(rows: dict[int, list[str]]) -> None:
    groups = (0, 50, 200, 500)
    data = {
        (n, state): median(
            float(row[8]) for key, row in rows.items()
            if key <= 24 and int(row[1]) == n and row[2] == state
        )
        for n in groups for state in ("唤醒", "休眠")
    }
    assert round(data[500, "唤醒"], 3) == 1.210
    assert round(data[500, "休眠"], 3) == 0.133

    image, draw = base("静止压力板：休眠与唤醒的复制 CPU 均值",
                       "每组 3 轮取中位数  ·  30 Hz 复制频率上限  ·  正式性能构建 A")
    top, bottom = 309, 771
    plot_grid(draw, 225, 1630, top, bottom, 1.5,
              [0, .3, .6, .9, 1.2, 1.5],
              lambda v: f"{v:.1f}", "ServerReplicateActors 复制均值（ms）")
    legend(draw, [("唤醒", BLUE), ("休眠", TEAL)], 1235, 234)

    for center, n in zip((370, 725, 1080, 1435), groups):
        for dx, state, color in ((-69, "唤醒", BLUE), (69, "休眠", TEAL)):
            value = data[n, state]
            y = bar(draw, center + dx, value, 1.5, top, bottom, color, 104)
            text(draw, (center + dx, y - 21), f"{value:.3f}", 27, INK, "mm")
        text(draw, (center, 808), str(n), 30, INK, "mm")
    text(draw, (930, 850), "新增压力板数量", 27, MUTED, "mm")
    footer(draw,
           "500 板：1.210 → 0.133 ms（减少 89.0%）",
           "该差值仅对应服务器复制函数的均值；0 板两组存在自然波动，不代表总 CPU 耗时。",
           "数据：Appendix.md B.1、B.4｜每组 n=3；柱高为逐轮均值的中位数，无误差条。")
    image.save(OUT / "01-static-plate-cpu.png", optimize=True)


def moving_platform(rows: dict[int, list[str]]) -> None:
    groups = (1, 5, 20)
    data = {
        (n, hz): median(
            float(row[7]) for key, row in rows.items()
            if 25 <= key <= 42 and int(row[1]) == n and row[2] == hz
        )
        for n in groups for hz in ("100 Hz", "30 Hz")
    }
    expected = (2436.7, 2186.4, 4499.6, 3414.2, 12269.3, 7830.6)
    assert tuple(data[n, hz] for n in groups for hz in ("100 Hz", "30 Hz")) == expected

    image, draw = base("运动平台：复制频率上限与服务器出站量",
                       "每组 3 轮取中位数  ·  100 / 30 Hz 是上限，不是实测发送频率  ·  构建 A")
    top, bottom = 309, 771
    plot_grid(draw, 225, 1630, top, bottom, 15000,
              [0, 3000, 6000, 9000, 12000, 15000],
              lambda v: f"{v:,.0f}", "引擎发送计数除以采样时间（B/s）")
    legend(draw, [("100 Hz 上限", BLUE), ("30 Hz 上限", TEAL)], 1110, 234)

    for center, n in zip((430, 930, 1430), groups):
        for dx, hz, color in ((-86, "100 Hz", BLUE), (86, "30 Hz", TEAL)):
            value = data[n, hz]
            y = bar(draw, center + dx, value, 15000, top, bottom, color, 126)
            text(draw, (center + dx, y - 23), f"{value:,.1f}", 27, INK, "mm")
        text(draw, (center, 808), str(n), 30, INK, "mm")
    text(draw, (930, 850), "新增运动平台数量", 27, MUTED, "mm")
    footer(draw,
           "20 平台：12,269.3 → 7,830.6 B/s（减少 36.2%）",
           "出站量是 UE 引擎计数，不是网卡抓包量或客户端成功接收量。",
           "数据：Appendix.md B.2、B.4｜每组 n=3；柱高为逐轮 B/s 的中位数，无误差条。")
    image.save(OUT / "02-moving-platform-bytes.png", optimize=True)


def jump_corrections() -> None:
    report = json.loads(INERTIA.read_text(encoding="utf-8"))
    profiles = ("Normal", "Moderate", "Harsh")
    records = {}
    for case in report["cases"]:
        if case["profile"] not in profiles:
            continue
        found = [entry["metrics"] for entry in case["motionMetrics"]
                 if entry["role"] == "Client" and entry["metrics"]["phase"] == "Jump"]
        assert len(found) == 1 and case["status"] == "passed"
        records[case["profile"]] = found[0]
    assert tuple((round(records[p]["maxCorrectionCm"], 2),
                  records[p]["correctionCount"]) for p in profiles) == (
                      (4.75, 2), (43.68, 8), (72.12, 13))

    image, draw = base("运动平台跳跃：客户端可比校正随模拟档位变化",
                       "PlatformInertia  ·  30 Hz 平台复制上限  ·  编辑器 -game / NullRHI 专项运行")
    text(draw, (143, 264), "客户端 Jump 最大可比校正误差（cm）", 30)
    text(draw, (1494, 264), "校正次数", 30)
    draw.line((1360, 286, 1360, 792), fill=GRID, width=2)
    colors = (BLUE, TEAL, ORANGE)
    labels = ("Normal", "Moderate", "Harsh")
    for profile, label, color, y in zip(profiles, labels, colors, (391, 555, 719)):
        metric = records[profile]
        value = metric["maxCorrectionCm"]
        width = round(value / 80 * 890)
        text(draw, (337, y), label, 32, INK, "rm")
        draw.rounded_rectangle((402, y - 27, 1292, y + 27), radius=20, fill="#EDF2F6")
        draw.rounded_rectangle((402, y - 27, 402 + max(width, 32), y + 27),
                               radius=20, fill=color)
        text(draw, (min(402 + width + 18, 1270), y), f"{value:.2f}", 30, INK, "lm")
        draw.rounded_rectangle((1480, y - 32, 1610, y + 33), radius=16,
                               fill="#EAF4F6")
        text(draw, (1545, y), str(metric["correctionCount"]), 36, INK, "mm")

    for tick in (0, 20, 40, 60, 80):
        x = 402 + round(tick / 80 * 890)
        draw.line((x, 806, x, 820), fill=MUTED, width=2)
        text(draw, (x, 844), str(tick), 25, MUTED, "mm")
    draw.line((402, 806, 1292, 806), fill=MUTED, width=2)
    draw.rounded_rectangle((118, 895, 1685, 971), radius=16, fill="#EAF4F6")
    text(draw, (144, 911), "模拟参数（双端）：Normal 0/0/0；Moderate 100/20/2%；Harsh 200/50/5%", 27)
    text(draw, (119, 983), "参数顺序为 PktLag(ms) / PktLagVariance(ms) / PktLoss；不是实测 RTT。每档 1 例。", 23, MUTED)
    text(draw, (119, 1012), "数据：PlatformRide/inertia.json｜可比历史 Move 校正；不表示屏幕拉回距离，NullRHI 不验证画面平滑。", 20, MUTED)
    image.save(OUT / "03-ride-jump-corrections.png", optimize=True)


def main() -> None:
    rows = appendix_rows()
    static_plate(rows)
    moving_platform(rows)
    jump_corrections()
    for path in sorted(OUT.glob("0*.png")):
        with Image.open(path) as image:
            print(f"{path}: {image.width} x {image.height}")


if __name__ == "__main__":
    main()
