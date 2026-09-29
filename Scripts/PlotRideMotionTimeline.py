"""Draw a client-local RideMotion Jump timeline from an actual runner report."""
import argparse
import csv
import html
import json
import math
from pathlib import Path
import tempfile


def valid(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"Invalid {name}: {value!r}")
    return float(value)


def csv_out(path, rows):
    if not rows:
        return
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def pair_moves(case, corrections, client_moves):
    hosts = [e["metrics"] for e in case.get("motionMetrics", [])
             if e.get("role") in ("Host", "Server") and e.get("metrics", {}).get("phase") == "Jump"]
    if len(hosts) != 1 or not client_moves or not hosts[0].get("serverMoveEvents"):
        return []
    server_moves = hosts[0]["serverMoveEvents"]

    def unique_match(moves, timestamp):
        matches = [move for move in moves if abs(valid(move.get("moveTimestamp"), "moveTimestamp") - timestamp) < .001]
        return matches[0] if len(matches) == 1 else None

    rows = []
    for correction in corrections:
        timestamp = correction["moveTimestamp"]
        client = unique_match(client_moves, timestamp)
        server = unique_match(server_moves, timestamp)
        row = {"correctionLocalT": correction["t"], "moveTimestamp": timestamp,
               "correctionErrorCm": correction["errorCm"],
               "matchStatus": "paired" if client and server else "missing-or-ambiguous-move",
               "clientWorldSeconds": client.get("clientWorldSeconds") if client else None,
               "serverWorldSeconds": server.get("serverWorldSeconds") if server else None,
               "clientStartX": client.get("startX") if client else None,
               "clientEndX": client.get("endX") if client else None,
               "clientRelativeX": client.get("relativeX") if client else None,
               "clientVelocityX": client.get("endVelocityX") if client else None,
               "clientPlatformX": client.get("platformX") if client else None,
               "clientPlatformVelocityX": client.get("platformVelocityX") if client else None,
               "clientStartBased": client.get("startBased") if client else None,
               "clientEndBased": client.get("endBased") if client else None,
               "clientJumpPressed": client.get("jumpPressed") if client else None,
               "clientStartBase": client.get("startBase") if client else None,
               "clientEndBase": client.get("endBase") if client else None,
               "serverX": server.get("serverX") if server else None,
               "serverRelativeX": server.get("serverRelativeX") if server else None,
               "serverVelocityX": server.get("serverVelocityX") if server else None,
               "serverPlatformX": server.get("platformX") if server else None,
               "serverPlatformVelocityX": server.get("platformVelocityX") if server else None,
               "serverReportedX": server.get("reportedX") if server else None,
               "serverBased": server.get("serverBased") if server else None,
               "serverFalling": server.get("serverFalling") if server else None,
               "serverReportedBased": server.get("reportedBased") if server else None,
               "serverBase": server.get("serverBase") if server else None,
               "serverReportedBase": server.get("reportedBase") if server else None}
        # 新版保留 XYZ、速度和模拟步长；旧报告缺少的列保持空白，不用 0 冒充测量值。
        for source, prefix, fields in ((client, "client", ("end", "relative", "endVelocity", "platform", "platformVelocity")),
                                       (server, "server", ("server", "serverRelative", "serverVelocity", "platform", "platformVelocity"))):
            for field in fields:
                for axis in "XYZ":
                    key = prefix + field[0].upper() + field[1:] + axis
                    row[key] = source.get(field + axis) if source else None
            row[prefix + "DeltaSeconds"] = source.get("deltaSeconds") if source else None
        # 两端时钟不可相减；此处仅比较同一 Move 对应的状态快照。
        row["platformPositionGapCm"] = server["platformX"] - client["platformX"] if client and server else None
        row["worldPositionGapCm"] = server["serverX"] - client["endX"] if client and server else None
        row["relativePositionGapCm"] = server["serverRelativeX"] - client["relativeX"] if client and server and client["endBased"] and server["serverBased"] else None
        for axis in "YZ":
            for name, server_key, client_key in (("worldGap", "server", "end"), ("velocityGap", "serverVelocity", "endVelocity")):
                sv = server.get(server_key + axis) if server else None
                cv = client.get(client_key + axis) if client else None
                row[name + axis] = sv - cv if sv is not None and cv is not None else None
        rows.append(row)
    return rows


def draw(case, output):
    matches = [e["metrics"] for e in case.get("motionMetrics", [])
               if e.get("role") == "Client" and e.get("metrics", {}).get("phase") == "Jump"]
    if len(matches) != 1:
        raise ValueError(f"{case['id']}: expected one client Jump metric")
    metrics = matches[0]
    samples = metrics.get("timelineSamples", [])
    corrections = metrics.get("correctionEvents", [])
    if len(samples) < 10 or metrics.get("droppedTimelineSamples", 0) or metrics.get("droppedCorrectionEvents", 0):
        raise ValueError(f"{case['id']}: missing or truncated timeline")
    for row in samples:
        for field in ("t", "platformX", "riderX", "platformStepCm"):
            valid(row.get(field), field)
    for row in corrections:
        for field in ("t", "moveTimestamp", "errorCm"):
            valid(row.get(field), field)
    times = [row["t"] for row in samples]
    if times != sorted(times):
        raise ValueError("Sample times are not ordered")
    identifier = "".join(c if c.isalnum() or c in "-_" else "_" for c in str(case["id"]))
    base = output / f"timeline-{identifier}"
    csv_out(base.with_name(base.name + "-samples.csv"), samples)
    csv_out(base.with_name(base.name + "-corrections.csv"), corrections)
    pairs = pair_moves(case, corrections, metrics.get("clientMoveEvents", []))
    csv_out(base.with_name(base.name + "-move-pairs.csv"), pairs)
    if metrics.get("clientMoveEvents"):
        all_moves = [{"t": "", "moveTimestamp": move["moveTimestamp"], "errorCm": ""}
                     for move in metrics["clientMoveEvents"]]
        csv_out(base.with_name(base.name + "-all-moves.csv"),
                pair_moves(case, all_moves, metrics["clientMoveEvents"]))

    tmax = max(4.0, valid(metrics.get("elapsedSeconds", 4), "elapsedSeconds"))
    left, right = 100, 1080
    x = lambda t: left + (right - left) * max(0, min(tmax, t)) / tmax
    values = [row[field] for row in samples for field in ("platformX", "riderX")]
    low, high = min(values), max(values)
    padding = max(20, (high - low) * .08)
    low, high = low - padding, high + padding
    y = lambda value: 330 - (value - low) / (high - low) * 185
    escape = lambda value: html.escape(str(value), quote=True)
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1160" height="720" viewBox="0 0 1160 720">',
           '<rect width="100%" height="100%" fill="#fbfcfe"/>',
           '<style>text{font-family:Arial,"Microsoft YaHei",sans-serif;fill:#233042;font-size:13px}.title{font-size:21px;font-weight:bold}.small{font-size:11px;fill:#54647a}</style>',
           f'<text class="title" x="100" y="42">RideMotion / Jump · {escape(case["id"])}</text>',
           f'<text x="100" y="70">{escape(case.get("profile", ""))} · {escape(case.get("platformMotion", "Moving"))} · {escape(case.get("platformNetHz", ""))} Hz · {len(corrections)} 次客户端校正</text>',
           '<text class="small" x="100" y="92">客户端本地时钟；平台阶跃是位置采样变化，不是精确收包时刻。历史 Move 误差不等于屏幕拉回距离。</text>']
    if pairs:
        paired = sum(row["matchStatus"] == "paired" for row in pairs)
        svg.append(f'<text class="small" x="100" y="110">按 Move 时间戳配对：{paired}/{len(pairs)} 次校正；两端状态原值见 move-pairs.csv。</text>')
    for tick in range(5):
        t = tmax * tick / 4
        px = x(t)
        svg += [f'<line x1="{px:.1f}" y1="120" x2="{px:.1f}" y2="590" stroke="#e2e8f0"/>',
                f'<text class="small" x="{px-12:.1f}" y="615">{t:.1f}s</text>']
        value = low + (high - low) * tick / 4
        py = y(value)
        svg += [f'<line x1="{left}" y1="{py:.1f}" x2="{right}" y2="{py:.1f}" stroke="#e2e8f0"/>',
                f'<text class="small" x="28" y="{py+4:.1f}">{value:.0f}cm</text>']
    for field, color in (("platformX", "#138a78"), ("riderX", "#2d68c4")):
        points = " ".join(f'{x(row["t"]):.1f},{y(row[field]):.1f}' for row in samples)
        svg.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="2.5"/>')
    svg += ['<line x1="100" y1="385" x2="1080" y2="385" stroke="#bac6d5"/>',
            '<text x="100" y="377">跳跃 / 离开基座 / 落回 / 收到校正</text>',
            '<line x1="100" y1="535" x2="1080" y2="535" stroke="#bac6d5"/>',
            '<text x="100" y="560">平台每帧位置变化（cm）</text>']
    markers = []
    previous = None
    for row in samples:
        if previous:
            if not previous.get("jumpRequested") and row.get("jumpRequested"):
                markers.append((row["t"], "发起跳跃", "#ba6513", 415, True))
            if previous.get("based") and not row.get("based"):
                markers.append((row["t"], "离开基座", "#8f45aa", 440, True))
            if not previous.get("based") and row.get("based"):
                markers.append((row["t"], "落回基座", "#138a78", 465, True))
        previous = row
        if row["platformStepCm"] > .01:
            px = x(row["t"])
            bar = min(42, row["platformStepCm"] * 2)
            svg.append(f'<line x1="{px:.1f}" y1="535" x2="{px:.1f}" y2="{535-bar:.1f}" stroke="#138a78" stroke-opacity=".55"/>')
    for index, row in enumerate(corrections, 1):
        label = f'校正 {index}: {row["errorCm"]:.1f} cm' if row.get("comparable") else f'校正 {index}: 参考系不可比'
        markers.append((row["t"], label, "#d84a52", 495, not row.get("comparable") or row["errorCm"] > 1))
    for t, label, color, lane, show_label in markers:
        px = x(t)
        svg += [f'<circle cx="{px:.1f}" cy="{lane}" r="4" fill="{color}"><title>{escape(label)} @ {t:.3f}s</title></circle>',
                f'<line x1="{px:.1f}" y1="390" x2="{px:.1f}" y2="{lane}" stroke="{color}" stroke-opacity=".3"/>']
        if show_label:
            svg.append(f'<text class="small" x="{px+6:.1f}" y="{lane-7}">{escape(label)}</text>')
    svg += ['<line x1="100" y1="649" x2="128" y2="649" stroke="#138a78" stroke-width="3"/>',
            '<text x="135" y="654">平台 X 位移</text>',
            '<line x1="270" y1="649" x2="298" y2="649" stroke="#2d68c4" stroke-width="3"/>',
            '<text x="305" y="654">角色 X 位移</text>',
            '<circle cx="449" cy="649" r="4" fill="#d84a52"/>',
            '<text x="460" y="654">收到校正（悬停查看误差）</text>',
            '<text class="small" x="100" y="690">原始数据见同名 CSV；时间相关性不能单独证明因果。</text>',
            '</svg>']
    result = base.with_suffix(".svg")
    result.write_text("\n".join(svg), encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        with tempfile.TemporaryDirectory() as folder:
            rows = [{"t": i / 10, "platformX": i * 2, "riderX": i * 2 + 1,
                     "platformStepCm": 2, "based": i < 5 or i > 10,
                     "falling": 5 <= i <= 10, "jumpRequested": i >= 4} for i in range(20)]
            fixture = {"id": "self-test", "profile": "Synthetic", "platformNetHz": 30,
                       "motionMetrics": [{"role": "Client", "metrics": {"phase": "Jump",
                        "elapsedSeconds": 4, "timelineSamples": rows, "correctionEvents": [
                        {"t": .8, "moveTimestamp": 1.5, "errorCm": 10, "comparable": True}]}}]}
            result = draw(fixture, Path(folder))
            assert result.is_file() and result.with_name("timeline-self-test-samples.csv").is_file()
        print("Plot self-test passed (synthetic data; not gameplay evidence)")
        return
    if not args.report:
        parser.error("report.json is required")
    report = json.loads(args.report.read_text(encoding="utf-8-sig"))
    output = args.output_dir or args.report.parent
    output.mkdir(parents=True, exist_ok=True)
    cases = [case for case in report.get("cases", []) if case.get("mode") == "RideMotion"
             and any(event.get("role") == "Client" and event.get("metrics", {}).get("phase") == "Jump"
                     for event in case.get("motionMetrics", []))]
    if not cases:
        raise SystemExit("No measured client RideMotion/Jump case; no chart created")
    for case in cases:
        print(draw(case, output))


if __name__ == "__main__":
    main()
