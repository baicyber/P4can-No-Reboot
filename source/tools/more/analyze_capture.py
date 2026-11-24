#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
抓包文件分析器（找「改设置 → 哪个帧变了」）
==========================================

用法：

    python tools/more/analyze_capture.py P4-CAN_capture.csv
    python tools/more/analyze_capture.py cap.csv --id 0x3C2
    python tools/more/analyze_capture.py cap.csv --id 0x249 --window 8000,20000
    python tools/more/analyze_capture.py cap.csv --top 20

输入就是固件页面「设置 → 抓包 → 下载 CSV」导出的文件，表头是：

    seq,ms,bus,id_hex,id_dec,dlc,d0,d1,d2,d3,d4,d5,d6,d7

输出三块：

    1) 概览      行数 / 时间跨度 / 每条总线的帧数 / ID 个数
    2) 候选帧    按「有几个字节是稀疏变化的」排序，这类帧最可能是设置状态帧
    3) 变化时间轴 每个候选 ID 在什么时刻、哪个字节从什么变成了什么

为什么这么筛：

    车身状态帧里绝大部分字节是「活跃」的 —— 计数器每帧 +1、CRC 每帧重算、
    车速/电压一直在动。这些字节的变化次数几乎等于帧数，没有分析价值。
    真正承载「设置项」的字节是稀疏的：整段抓包里只变 3~5 次，正好对应你在
    车机上点的那几下。脚本就是按这个特征把设置帧从噪音里挑出来。

    找氛围灯颜色帧时：把颜色切成红 → 绿 → 蓝各停 3 秒，正常会看到某个 ID 有
    1 个或 3 个字节跟着跳 3 次。3 个相邻字节同时跳 = RGB 直接编码；1 个字节跳
    = 色号/枚举。两种都要在时间轴上确认跳变时刻和你操作时刻对得上。
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path


# ── 数据结构 ──────────────────────────────────────────────────────────

@dataclass
class Frame:
    seq: int
    ms: int
    bus: str
    can_id: int
    dlc: int
    data: list[int]


@dataclass
class IdStat:
    can_id: int
    bus: str
    count: int = 0
    first_ms: int = 0
    last_ms: int = 0
    # 每个字节位置的变化次数
    byte_changes: list[int] = field(default_factory=lambda: [0] * 8)
    # 稀疏变化事件：(ms, {byte: (old, new)})
    events: list[tuple[int, dict[int, tuple[int, int]]]] = field(default_factory=list)
    # 每帧都在变的字节（计数器 / CRC）
    live_bytes: list[int] = field(default_factory=list)
    idle_bytes: list[int] = field(default_factory=list)

    @property
    def period_ms(self) -> float:
        if self.count < 2:
            return 0.0
        return (self.last_ms - self.first_ms) / float(self.count - 1)

    @property
    def sparse_bytes(self) -> list[int]:
        """变化次数在 1~12 次之间、帧数足够多的字节。"""
        out = []
        for i, n in enumerate(self.byte_changes):
            if 0 < n <= 12 and self.count >= 30:
                out.append(i)
        return out

    @property
    def score(self) -> tuple:
        """候选排序：稀疏字节多的靠前，帧数适中的靠前。"""
        n = len(self.sparse_bytes)
        return (-n, -len(self.events), self.count)


# ── 读取 ──────────────────────────────────────────────────────────────

def load_csv(path: Path) -> list[Frame]:
    raw = path.read_bytes()
    for enc in ("utf-8-sig", "utf-8", "gbk"):
        try:
            text = raw.decode(enc)
            break
        except UnicodeDecodeError:
            continue
    else:
        raise SystemExit("无法识别文件编码：%s" % path)

    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines:
        raise SystemExit("文件是空的")

    header = [c.strip().lower() for c in lines[0].split(",")]
    need = ("ms", "id_hex", "dlc")
    if not all(k in header for k in need):
        raise SystemExit("表头不像抓包 CSV，缺少 %s\n实际表头：%s"
                         % ("/".join(need), lines[0]))

    idx = {k: header.index(k) for k in ("seq", "ms", "bus", "id_hex", "id_dec",
                                        "dlc", "d0", "d1", "d2", "d3",
                                        "d4", "d5", "d6", "d7") if k in header}

    out: list[Frame] = []
    for ln in lines[1:]:
        parts = [p.strip() for p in ln.split(",")]
        if len(parts) < len(header):
            continue
        try:
            ms = int(parts[idx["ms"]])
            can_id = int(parts[idx["id_hex"]].replace("0x", "").replace("0X", ""), 16)
        except (ValueError, KeyError):
            continue
        seq = int(parts[idx["seq"]]) if "seq" in idx else len(out)
        bus = parts[idx["bus"]] if "bus" in idx else "?"
        dlc = int(parts[idx["dlc"]]) if "dlc" in idx else 8
        dlc = max(0, min(8, dlc))
        data = []
        for i in range(8):
            key = "d%d" % i
            if key in idx:
                try:
                    data.append(int(parts[idx[key]]) & 0xFF)
                except ValueError:
                    data.append(0)
            else:
                data.append(0)
        out.append(Frame(seq, ms, bus, can_id, dlc, data))
    return out


# ── 统计 ──────────────────────────────────────────────────────────────

def analyze(frames: list[Frame]) -> dict[int, IdStat]:
    by_id: dict[int, list[Frame]] = defaultdict(list)
    for f in frames:
        by_id[f.can_id].append(f)

    out: dict[int, IdStat] = {}
    for can_id, group in by_id.items():
        # 环形缓冲有可能回绕，按 seq 排才是真实时间顺序
        group.sort(key=lambda f: (f.seq, f.ms))
        st = IdStat(can_id=can_id, bus=group[0].bus, count=len(group),
                    first_ms=group[0].ms, last_ms=group[-1].ms)
        prev = None
        for f in group:
            if prev is not None:
                diff = {}
                for i in range(8):
                    if i >= f.dlc and i >= prev.dlc:
                        continue
                    if f.data[i] != prev.data[i]:
                        diff[i] = (prev.data[i], f.data[i])
                        st.byte_changes[i] += 1
                if diff:
                    # 同一时刻的多次变化合并成事件
                    if st.events and st.events[-1][0] == f.ms:
                        st.events[-1] = (f.ms, {**st.events[-1][1], **diff})
                    else:
                        st.events.append((f.ms, diff))
            prev = f

        if st.count >= 30:
            live, idle = [], []
            for i, n in enumerate(st.byte_changes):
                if n >= st.count * 0.5:
                    live.append(i)
                elif n == 0:
                    idle.append(i)
            st.live_bytes, st.idle_bytes = live, idle
        out[can_id] = st
    return out


# ── 输出 ──────────────────────────────────────────────────────────────

def fmt_bus_summary(frames: list[Frame]) -> str:
    per_bus: dict[str, int] = defaultdict(int)
    for f in frames:
        per_bus[f.bus] += 1
    span = (max(f.ms for f in frames) - min(f.ms for f in frames)) if frames else 0
    ids = len({f.can_id for f in frames})
    parts = ["%s=%d" % (b, n) for b, n in sorted(per_bus.items())]
    return ("行数 %d ｜ 时间跨度 %.1f s ｜ ID 个数 %d ｜ 总线 %s"
            % (len(frames), span / 1000.0, ids, " ".join(parts)))


def print_candidates(stats: dict[int, IdStat], top: int) -> list[IdStat]:
    ranked = sorted(stats.values(), key=lambda s: s.score)
    picked = [s for s in ranked if s.sparse_bytes][:top]
    if not picked:
        print("  （没有找到稀疏变化的帧）")
        return []

    print("  %-8s %-4s %7s %9s  %s" % ("ID", "BUS", "帧数", "周期ms", "稀疏变化字节"))
    print("  " + "-" * 76)
    for s in picked:
        names = " ".join("d%d(%d次)" % (i, s.byte_changes[i]) for i in s.sparse_bytes)
        bus = s.bus if s.bus else "?"
        print("  %-8s %-4s %7d %9.1f  %s"
              % ("0x%03X" % s.can_id, bus, s.count, s.period_ms, names))
    return picked


def print_timeline(s: IdStat, limit: int) -> None:
    print()
    print("  ◆ 0x%03X  BUS %s  %d 帧  %.1f ms/帧  总变化事件 %d"
          % (s.can_id, s.bus, s.count, s.period_ms, len(s.events)))
    if s.live_bytes:
        print("    每帧都在变（计数器/CRC，忽略）：%s"
              % " ".join("d%d" % i for i in s.live_bytes))
    if s.idle_bytes:
        print("    整段没变过：%s" % " ".join("d%d" % i for i in s.idle_bytes))
    print()

    shown = 0
    for ms, diff in s.events:
        # 只打印包含稀疏字节的事件，纯计数器变化跳过
        interesting = {i: v for i, v in diff.items() if i in s.sparse_bytes}
        if not interesting:
            continue
        keys = sorted(interesting)
        rgb_hint = ""
        # 红(FF,00,00) → 绿(00,FF,00) 只有两个字节真的变，所以 2 个也算 RGB 线索
        if len(keys) >= 2 and keys == list(range(keys[0], keys[0] + len(keys))):
            rgb_hint = "   ← 连续 %d 字节同时跳，疑似 RGB 分量" % len(keys)
        cells = "  ".join("d%d %02X→%02X" % (i, v[0], v[1])
                          for i, v in sorted(interesting.items()))
        print("    t=%8.3f s   %s%s" % (ms / 1000.0, cells, rgb_hint))
        shown += 1
        if shown >= limit:
            print("    …（还有 %d 个事件，--events 调大）"
                  % max(0, len([1 for _, d in s.events
                                if any(i in s.sparse_bytes for i in d)]) - limit))
            break
    if shown == 0:
        print("    （没有落在稀疏字节上的变化）")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="分析 P4 抓包 CSV，找出设置变化对应的帧与字节")
    ap.add_argument("csv", help="抓包导出的 CSV 文件")
    ap.add_argument("--id", default="", help="只看某个 ID，如 0x3C2")
    ap.add_argument("--window", default="", help="只看某时间窗，如 8000,20000（毫秒）")
    ap.add_argument("--top", type=int, default=12, help="列出多少个候选 ID（默认 12）")
    ap.add_argument("--events", type=int, default=20, help="每个 ID 最多打印多少变化事件")
    args = ap.parse_args()

    path = Path(args.csv)
    if not path.is_file():
        raise SystemExit("找不到文件：%s" % path)

    frames = load_csv(path)
    if not frames:
        raise SystemExit("没解析出任何帧")

    print("=" * 78)
    print("概览")
    print("=" * 78)
    print("  " + fmt_bus_summary(frames))

    if args.window:
        try:
            lo, hi = (int(x) for x in args.window.split(","))
        except ValueError:
            raise SystemExit("--window 需要形如 8000,20000")
        frames = [f for f in frames if lo <= f.ms <= hi]
        print("  时间窗过滤后：%d 帧（%d ~ %d ms）" % (len(frames), lo, hi))
        if not frames:
            raise SystemExit("时间窗内没有帧")

    stats = analyze(frames)

    if args.id:
        want = int(args.id.replace("0x", "").replace("0X", ""), 16)
        st = stats.get(want)
        if not st:
            near = ", ".join("0x%03X" % i for i in sorted(stats)[:20])
            raise SystemExit("抓包里没有 0x%03X。已有的前几个：%s" % (want, near))
        print()
        print("=" * 78)
        print("指定 ID 的变化时间轴")
        print("=" * 78)
        print_timeline(st, args.events)
        return 0

    print()
    print("=" * 78)
    print("候选帧（有稀疏变化字节的，最可能是设置状态帧）")
    print("=" * 78)
    picked = print_candidates(stats, args.top)

    if picked:
        print()
        print("=" * 78)
        print("候选帧的变化时间轴")
        print("=" * 78)
        for s in picked:
            print_timeline(s, args.events)

        print()
        print("=" * 78)
        print("怎么读这份结果")
        print("=" * 78)
        print("  1. 先在时间轴上找你操作的那一刻（比如切颜色的时刻）。")
        print("  2. 某个 ID 的字节恰好在那一刻跳、之后保持不动 → 这就是目标字段。")
        print("  3. 连续 3 个字节一起跳 → RGB 直接编码，记下这三个字节的位置。")
        print("     只有 1 个字节跳 → 色号/枚举，需要在车机上多切几种颜色再抓一次，")
        print("     把「颜色 ↔ 数值」对应关系补全。")
        print("  4. 若同一个 ID 有多个字节在**不同时刻**分别跳，说明它是多字段复合帧，")
        print("     必须先锁定 Mux（通常是 d0 的低 2 位）再动其它字节。")
        print()
        print("  下一步：把这份输出（或原始 CSV）发我，我按字段位置写注入代码。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
