"""Render two Chinese mechanism diagrams for the Notion network-sync article.

The diagrams are authored as SVG in Python and rasterized locally. Run:
    python Scripts/RenderNotionMechanisms.py
"""

from __future__ import annotations

from html import escape
from pathlib import Path
from xml.etree import ElementTree

from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "Docs" / "Exports" / "figures"
FONT = "Noto Sans SC, Microsoft YaHei, sans-serif"
FONT_REGULAR = Path(r"C:\Windows\Fonts\msyh.ttc")
FONT_BOLD = Path(r"C:\Windows\Fonts\msyhbd.ttc")


def svg_open(width: int, height: int, title: str) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" role="img" aria-label="{escape(title)}">',
        "<defs>",
        '<marker id="arrow-blue" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" orient="auto"><path d="M 0 0 L 10 5 L 0 10 z" fill="#277CA5"/></marker>',
        '<marker id="arrow-amber" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" orient="auto"><path d="M 0 0 L 10 5 L 0 10 z" fill="#C4862D"/></marker>',
        '<marker id="arrow-violet" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" orient="auto"><path d="M 0 0 L 10 5 L 0 10 z" fill="#7A64B0"/></marker>',
        '<filter id="shadow" x="-10%" y="-20%" width="120%" height="150%"><feDropShadow dx="0" dy="7" stdDeviation="9" flood-color="#30445C" flood-opacity="0.11"/></filter>',
        "</defs>",
        '<rect width="100%" height="100%" fill="#F5F8FC"/>',
    ]


def rect(parts: list[str], x: int, y: int, w: int, h: int, fill: str, radius: int = 18,
         stroke: str = "none", stroke_width: int = 2, shadow: bool = False) -> None:
    effect = ' filter="url(#shadow)"' if shadow else ""
    parts.append(
        f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" '
        f'fill="{fill}" stroke="{stroke}" stroke-width="{stroke_width}"{effect}/>'
    )


def label(parts: list[str], value: str, x: int, y: int, size: int = 30,
          color: str = "#172B42", weight: int = 400, anchor: str = "start",
          letter_spacing: int = 0) -> None:
    parts.append(
        f'<text x="{x}" y="{y}" text-anchor="{anchor}" fill="{color}" '
        f'font-family="{FONT}" font-size="{size}" font-weight="{weight}" '
        f'letter-spacing="{letter_spacing}">{escape(value)}</text>'
    )


def lines(parts: list[str], content: list[str], x: int, y: int, size: int = 26,
          gap: int = 43, color: str = "#43566D", weight: int = 400) -> None:
    for index, line in enumerate(content):
        label(parts, line, x, y + index * gap, size, color, weight)


def card(parts: list[str], x: int, y: int, w: int, h: int, title: str,
         body: list[str], accent: str, tint: str = "#FFFFFF",
         title_size: int = 33, body_size: int = 26, body_gap: int = 43) -> None:
    rect(parts, x, y, w, h, tint, 22, "#E2EAF1", 2, True)
    rect(parts, x, y, 10, h, accent, 5)
    label(parts, title, x + 35, y + 58, title_size, "#172B42", 700)
    lines(parts, body, x + 35, y + 108, body_size, body_gap)


def arrow(parts: list[str], x1: int, y1: int, x2: int, y2: int,
          color: str = "blue", width: int = 5, dashed: bool = False) -> None:
    palette = {"blue": "#277CA5", "amber": "#C4862D", "violet": "#7A64B0"}
    dash = ' stroke-dasharray="13 10"' if dashed else ""
    parts.append(
        f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
        f'stroke="{palette[color]}" stroke-width="{width}" stroke-linecap="round" '
        f'marker-end="url(#arrow-{color})"{dash}/>'
    )


def section(parts: list[str], number: str, title: str, y: int, color: str,
            width: int = 2200) -> None:
    rect(parts, 78, y - 39, 56, 56, color, 14)
    label(parts, number, 106, y, 26, "#FFFFFF", 750, "middle")
    label(parts, title, 156, y, 35, "#172B42", 700)
    parts.append(f'<line x1="78" y1="{y + 28}" x2="{width + 78}" y2="{y + 28}" stroke="#DDE6EF" stroke-width="2"/>')


def authority_flow() -> tuple[str, int, int]:
    w, h = 2400, 1530
    p = svg_open(w, h, "UE Co-op 权威数据流")
    label(p, "UE Co-op：输入如何变成共享事实", 92, 95, 58, "#11263D", 760)
    label(p, "Listen Server 拥有最终判定；角色移动协议、业务请求与复制状态各走自己的路径。",
          94, 155, 29, "#51657A")

    # Three columns keep ownership and authority visible across all paths.
    for x, title, subtitle, fill in [
        (100, "所属客户端", "Owning Client", "#E4F1F9"),
        (900, "Listen Server", "Authority", "#EAF5EB"),
        (1700, "其他客户端", "Remote Client", "#F0EBF9"),
    ]:
        rect(p, x, 190, 600, 74, fill, 17)
        label(p, title, x + 28, 239, 32, "#173049", 750)
        label(p, subtitle, x + 575, 237, 23, "#617287", 500, "end")

    section(p, "01", "角色移动：引擎 CMC 的专用协议", 316, "#277CA5")
    card(p, 100, 370, 600, 207, "本地预测与保存移动", ["输入立即驱动 CMC", "保留 SavedMoves 供校正后重放"], "#277CA5")
    card(p, 900, 370, 600, 207, "服务器重演与校验", ["按服务器状态模拟移动", "确认结果，必要时返回校正"], "#4C9863")
    card(p, 1700, 370, 600, 207, "远端角色表现", ["接收权威移动结果", "SimulatedProxy 做网络平滑"], "#7A64B0")
    arrow(p, 712, 454, 888, 454)
    label(p, "ServerMove", 800, 404, 22, "#216F96", 700, "middle")
    label(p, "Packed / 非可靠", 800, 434, 20, "#216F96", 500, "middle")
    arrow(p, 888, 540, 712, 540, "amber", 4, True)
    label(p, "确认 / 校正", 800, 572, 21, "#A36F24", 600, "middle")
    arrow(p, 1512, 474, 1688, 474)
    label(p, "移动复制", 1600, 446, 23, "#216F96", 600, "middle")

    section(p, "02", "业务输入与规则判定", 660, "#C4862D")
    card(p, 100, 718, 600, 183, "主动请求：如重开", ["由拥有的 Pawn / Controller 发起", "Server RPC 只提交意图"], "#C4862D", "#FFFDF8", 32, 25)
    card(p, 900, 704, 600, 220, "服务器规则", ["机关 Actor 判定钥匙 / 踏板", "GameMode 复核目标与胜利", "结果写入权威状态"], "#4C9863", "#FBFFFB", 33, 25, 37)
    card(p, 1700, 718, 600, 183, "客户端只展示结果", ["本地画面和 UI 不决定胜负", "不能直接改写共享进度"], "#7A64B0", "#FCFAFF", 32, 25)
    arrow(p, 712, 809, 888, 809, "amber")
    label(p, "Server RPC", 800, 784, 23, "#A36F24", 700, "middle")
    label(p, "服务器碰撞触发的机关事件，可直接进入服务器规则，无须额外请求。",
          925, 965, 25, "#617287")

    section(p, "03", "持续状态向客户端复制", 1025, "#7A64B0")
    card(p, 900, 1080, 600, 276, "权威状态", ["GameState：目标进度 / 胜利", "机关 Actor：门、板等状态", "平台：服务器变换 RepMovement"], "#4C9863", "#FBFFFB", 33, 26, 45)
    card(p, 100, 1100, 600, 236, "所属客户端接收", ["OnRep / 本地 UI 与表现", "接收平台服务器位置", "角色自身继续走 CMC 路径"], "#277CA5", "#FFFFFF", 32, 25, 40)
    card(p, 1700, 1100, 600, 236, "远端客户端接收", ["OnRep / 本地 UI 与表现", "门板按状态播放过渡", "平台接收服务器变换"], "#7A64B0", "#FFFFFF", 32, 25, 40)
    arrow(p, 888, 1230, 712, 1230, "violet")
    arrow(p, 1512, 1230, 1688, 1230, "violet")
    label(p, "属性 / 位置", 800, 1202, 22, "#705AA8", 650, "middle")
    label(p, "属性 / 位置", 1600, 1202, 22, "#705AA8", 650, "middle")

    rect(p, 92, 1410, 2216, 79, "#E7EDF5", 15)
    label(p, "边界：平台沿轨道由服务器移动并复制变换；它没有角色 CMC 的 SavedMoves 预测与重放。",
          124, 1460, 27, "#334A62", 600)
    p.append("</svg>")
    return "\n".join(p), w, h


def platform_ride() -> tuple[str, int, int]:
    w, h = 2400, 1530
    p = svg_open(w, h, "移动平台载人：顺序、速度和空中规则的因果链")
    label(p, "移动平台载人：三个环节，三种不同故障", 92, 95, 58, "#11263D", 760)
    label(p, "从“站着能跟随”到“走动、换向、跳起并落回”，还要检查更新顺序、基座速度与空中制动。",
          94, 155, 29, "#51657A")
    rect(p, 100, 194, 985, 62, "#FCECEC", 15)
    rect(p, 1315, 194, 985, 62, "#E8F6ED", 15)
    label(p, "旧行为 / 中间对照", 130, 237, 31, "#A74747", 730)
    label(p, "修复路径 / 最终规则", 1345, 237, 31, "#287246", 730)

    section(p, "01", "更新顺序：运动基座要在角色使用它之前更新", 320, "#277CA5")
    card(p, 100, 380, 985, 210, "旧：平台组件 Tick 较晚", ["CMC：TG_PrePhysics → 先读取基座", "Transporter：TG_DuringPhysics → 后移动", "仅识别到基座，不保证先更新平台"], "#D46262", "#FFFDFD", 33, 27, 40)
    card(p, 1315, 380, 985, 210, "修：Transporter 改为 TG_PrePhysics", ["与 CMC 处于同一 TickGroup", "基座依赖可安排平台先更新", "不额外附着或搬动角色"], "#4C9863", "#FCFFFD", 33, 27, 40)
    arrow(p, 1105, 477, 1295, 477)

    section(p, "02", "基座速度：位置在动，不等于根组件报告了速度", 640, "#277CA5")
    card(p, 100, 700, 985, 238, "旧：SetActorLocation 只改变位置", ["根 ComponentVelocity 可能仍为 0", "站立跟随主要读取基座变换", "起跳继承速度时却读不到有效基座速度"], "#D46262", "#FFFDFD", 33, 27, 44)
    card(p, 1315, 686, 985, 270, "修：服务器与接收端都补齐速度", ["服务器：本帧位移 / Δt → 根组件速度", "RepMovement 发送线速度与变换", "客户端：PostNetReceiveVelocity 写回根速度", "CMC 起跳时读取有效基座速度"], "#4C9863", "#FCFFFD", 33, 26, 43)
    arrow(p, 1105, 820, 1295, 820)

    section(p, "03", "空中规则：继承的速度还要在飞行中保得住", 1036, "#C4862D")
    card(p, 100, 1096, 985, 230, "中间方案：空中制动 = 1500", ["起跳前基座速度 150 cm/s", "无输入后约 0.1 s 刹停；0.25 s 后为 0", "平台继续前进，跳跃对照仍失败"], "#D46262", "#FFFDFD", 33, 26, 43)
    card(p, 1315, 1096, 985, 230, "最终玩法：空中制动 = 0", ["保留继承的 150 cm/s 水平惯性", "0.25 s 后仍为 150 cm/s", "三档网络下双端动作检查通过"], "#4C9863", "#FCFFFD", 33, 26, 43)
    arrow(p, 1105, 1200, 1295, 1200, "amber")

    rect(p, 90, 1360, 2220, 124, "#233D58", 20)
    label(p, "结论是功能正确性：Harsh 跳跃仍有 13 次校正，最大可比较误差 72.12 cm。",
          128, 1411, 30, "#FFFFFF", 700)
    label(p, "NullRHI 不呈现画面；这些记录不能证明弱网下镜头与角色表现已经平滑。",
          128, 1456, 27, "#CBD9E7")
    p.append("</svg>")
    return "\n".join(p), w, h


def render(svg: str, width: int, height: int, target: Path) -> None:
    # The SVG uses only rectangles, lines and text. Drawing those primitives at
    # 2x gives a sharp PNG without depending on a browser or external images.
    scale = 2
    source = ElementTree.fromstring(svg)
    elements = [el for el in source if el.tag.rsplit("}", 1)[-1] != "defs"]
    canvas = Image.new("RGB", (width * scale, height * scale), "#F5F8FC")
    shadows = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(shadows)

    def integer(el: ElementTree.Element, attr: str, default: int = 0) -> int:
        value = el.attrib.get(attr, str(default))
        if value.endswith("%"):
            extent = width if attr == "width" else height
            return round(extent * float(value[:-1]) / 100 * scale)
        return round(float(value) * scale)

    for el in elements:
        if el.tag.rsplit("}", 1)[-1] == "rect" and "filter" in el.attrib:
            x, y = integer(el, "x"), integer(el, "y")
            box = (x + 7 * scale, y + 9 * scale,
                   x + integer(el, "width") + 7 * scale,
                   y + integer(el, "height") + 9 * scale)
            sd.rounded_rectangle(box, radius=integer(el, "rx"), fill=(35, 58, 79, 45))
    canvas = Image.alpha_composite(canvas.convert("RGBA"), shadows.filter(ImageFilter.GaussianBlur(12 * scale)))
    draw = ImageDraw.Draw(canvas)
    fonts: dict[tuple[int, bool], ImageFont.FreeTypeFont] = {}

    for el in elements:
        kind = el.tag.rsplit("}", 1)[-1]
        if kind == "rect":
            x, y = integer(el, "x"), integer(el, "y")
            box = (x, y, x + integer(el, "width"), y + integer(el, "height"))
            fill = el.attrib.get("fill", "#FFFFFF")
            if fill == "none":
                fill = None
            draw.rounded_rectangle(box, radius=integer(el, "rx"), fill=fill,
                                   outline=None if el.attrib.get("stroke", "none") == "none" else el.attrib["stroke"],
                                   width=integer(el, "stroke-width", 2))
        elif kind == "line":
            x1, y1 = integer(el, "x1"), integer(el, "y1")
            x2, y2 = integer(el, "x2"), integer(el, "y2")
            color = el.attrib.get("stroke", "#277CA5")
            line_width = integer(el, "stroke-width", 2)
            if "stroke-dasharray" in el.attrib and y1 == y2:
                step = 23 * scale
                segment = 13 * scale
                direction = 1 if x2 > x1 else -1
                for offset in range(0, abs(x2 - x1), step):
                    start = x1 + direction * offset
                    stop = x1 + direction * min(offset + segment, abs(x2 - x1))
                    draw.line((start, y1, stop, y2), fill=color, width=line_width)
            else:
                draw.line((x1, y1, x2, y2), fill=color, width=line_width)
            if "marker-end" in el.attrib:
                direction = 1 if x2 > x1 else -1
                draw.polygon([(x2, y2), (x2 - direction * 18 * scale, y2 - 10 * scale),
                              (x2 - direction * 18 * scale, y2 + 10 * scale)], fill=color)
        elif kind == "text":
            size = integer(el, "font-size", 28)
            bold = int(el.attrib.get("font-weight", "400")) >= 600
            key = (size, bold)
            if key not in fonts:
                fonts[key] = ImageFont.truetype(str(FONT_BOLD if bold else FONT_REGULAR), size)
            anchor = {"start": "ls", "middle": "ms", "end": "rs"}[el.attrib.get("text-anchor", "start")]
            draw.text((integer(el, "x"), integer(el, "y")), el.text or "", font=fonts[key],
                      anchor=anchor, fill=el.attrib.get("fill", "#172B42"))

    canvas.convert("RGB").resize((width, height), Image.Resampling.LANCZOS).save(target, "PNG", optimize=True)
    with Image.open(target) as image:
        if image.size != (width, height):
            raise RuntimeError(f"Unexpected PNG size for {target}: {image.size}")
        print(f"{target} — {image.width} × {image.height}")


def main() -> None:
    if not FONT_REGULAR.is_file() or not FONT_BOLD.is_file():
        raise FileNotFoundError("Microsoft YaHei font files are required")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for builder, filename in [
        (authority_flow, "04-ue-authority-flow.png"),
        (platform_ride, "05-platform-ride-mechanism.png"),
    ]:
        svg, width, height = builder()
        render(svg, width, height, OUTPUT / filename)


if __name__ == "__main__":
    main()
