#!/usr/bin/env python3
"""
OpenSparX AgentOS — Interactive TUI Demo
Showcases: Smart Cockpit & Drone Perception scenarios

Usage:
    python3 demo/tui_demo.py [--scenario cockpit|drone]

Controls:
    1 — Smart Cockpit scenario
    2 — Drone Perception scenario
    q — Quit
"""

import curses
import time
import threading
import argparse
import random
import sys

# ─── Banner ───────────────────────────────────────────────────────────────────

BANNER = r"""
   ____                  _____                 _  __
  / __ \___  ___ ___    / ___/__  ___ _____   | |/ /
 / /_/ / _ \/ -_) _ \  _\__ \/ _ \/ _ `/ __/   >  <
 \____/ .__/\__/_//_/ /____/ .__/\_,_/_/ ><  /_/\_\
     /_/                   /_/
       ___                __  ____  ___
      / _ |___ ____ ___  / /_/ __ \/ __/
     / __ / _ `/ -_) _ \/ __/ /_/ /\ \
    /_/ |_\_, /\__/_//_/\__/\____/___/
         /___/                          """

VERSION = "2.2.2"

# ─── Color Pairs ──────────────────────────────────────────────────────────────

C_BANNER = 1
C_BORDER = 2
C_TITLE = 3
C_PROGRESS_DONE = 4
C_PROGRESS_ACTIVE = 5
C_DIM = 6
C_ALERT = 7
C_SUCCESS = 8

def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(C_BANNER, curses.COLOR_GREEN, -1)
    curses.init_pair(C_BORDER, 245, -1)  # gray
    curses.init_pair(C_TITLE, curses.COLOR_CYAN, -1)
    curses.init_pair(C_PROGRESS_DONE, curses.COLOR_GREEN, -1)
    curses.init_pair(C_PROGRESS_ACTIVE, curses.COLOR_YELLOW, -1)
    curses.init_pair(C_DIM, 240, -1)
    curses.init_pair(C_ALERT, curses.COLOR_RED, -1)
    curses.init_pair(C_SUCCESS, curses.COLOR_GREEN, -1)


# ─── UI Primitives ────────────────────────────────────────────────────────────

def draw_box(win, y, x, h, w, title=""):
    """Draw a bordered box with optional title."""
    for row in range(y, y + h):
        for col in range(x, x + w):
            if row == y or row == y + h - 1:
                ch = "─"
            elif col == x or col == x + w - 1:
                ch = "│"
            else:
                continue
            try:
                win.addstr(row, col, ch, curses.color_pair(C_BORDER))
            except curses.error:
                pass
    # corners
    corners = [(y, x, "┌"), (y, x+w-1, "┐"), (y+h-1, x, "└"), (y+h-1, x+w-1, "┘")]
    for r, c, ch in corners:
        try:
            win.addstr(r, c, ch, curses.color_pair(C_BORDER))
        except curses.error:
            pass
    if title:
        t = f" {title} "
        try:
            win.addstr(y, x + 2, t, curses.color_pair(C_TITLE) | curses.A_BOLD)
        except curses.error:
            pass


def draw_progress(win, y, x, width, pct, label=""):
    """Draw a progress bar."""
    bar_w = width - len(label) - 8
    filled = int(bar_w * pct)
    bar = "█" * filled + "░" * (bar_w - filled)
    color = C_PROGRESS_DONE if pct >= 1.0 else C_PROGRESS_ACTIVE
    status = "done" if pct >= 1.0 else f"{int(pct*100):3d}%"
    try:
        win.addstr(y, x, f" ▶ {label:<16s}", curses.color_pair(C_DIM))
        win.addstr(y, x + 19, bar, curses.color_pair(color))
        win.addstr(y, x + 19 + bar_w + 1, status, curses.color_pair(color))
    except curses.error:
        pass


def draw_agent_status(win, y, x, name, status, icon="🧠"):
    """Draw agent status line."""
    colors = {"active": C_PROGRESS_ACTIVE, "done": C_SUCCESS, "idle": C_DIM, "wait": C_DIM}
    symbols = {"active": "⚡", "done": "✓", "idle": "○", "wait": "○"}
    c = colors.get(status, C_DIM)
    s = symbols.get(status, "○")
    try:
        win.addstr(y, x, f" {icon} {name:<14s} {s} {status}", curses.color_pair(c))
    except curses.error:
        pass

# ─── Scenario: Smart Cockpit ──────────────────────────────────────────────────

class CockpitScenario:
    """Smart cockpit: '我困了，帮我提提神' multi-agent coordination."""

    def __init__(self):
        self.step = 0
        self.max_steps = 60
        self.voice_input = "「我困了，帮我提提神」"
        self.agents = {
            "Planner":    {"icon": "🧠", "status": "idle"},
            "Voice":      {"icon": "🎙", "status": "idle"},
            "Climate":    {"icon": "❄️", "status": "idle"},
            "Media":      {"icon": "🎵", "status": "idle"},
            "Seat":       {"icon": "💺", "status": "idle"},
            "Verifier":   {"icon": "🔒", "status": "idle"},
        }
        self.tasks = {
            "语音意图解析": 0.0,
            "安全验证":     0.0,
            "空调调节":     0.0,
            "音乐切换":     0.0,
            "座椅按摩":     0.0,
        }
        self.logs = []
        self.verified = False
        self.latency_ms = 0

    def tick(self):
        self.step += 1
        s = self.step

        # Phase 1: Voice recognition (steps 1-10)
        if s <= 10:
            self.agents["Voice"]["status"] = "active"
            self.tasks["语音意图解析"] = min(1.0, s / 10)
            if s == 1:
                self.logs.append("[Voice] STT 启动，识别语音流...")
            if s == 5:
                self.logs.append("[Voice] 识别: '我困了帮我提提神'")
            if s == 10:
                self.agents["Voice"]["status"] = "done"
                self.agents["Planner"]["status"] = "active"
                self.logs.append("[Planner] 意图: FATIGUE_RELIEF → 拆解子任务")

        # Phase 2: Planning + Verification (steps 11-25)
        elif s <= 25:
            self.agents["Planner"]["status"] = "active" if s < 15 else "done"
            self.agents["Verifier"]["status"] = "active" if s >= 15 else "idle"
            self.tasks["安全验证"] = min(1.0, (s - 10) / 15)
            if s == 12:
                self.logs.append("[Planner] 子任务: Climate↓22° | Music→高能 | Seat→按摩")
            if s == 16:
                self.logs.append("[Verify] 检查: 空调+座椅功率 < 电池余量...")
            if s == 20:
                self.logs.append("[Verify] CTL: AG ¬(power_exceed) ✓ SAFE")
            if s == 25:
                self.agents["Verifier"]["status"] = "done"
                self.verified = True
                self.logs.append("[Verify] ✓ 全部约束通过，执行许可")

        # Phase 3: Parallel execution (steps 26-50)
        elif s <= 50:
            progress = min(1.0, (s - 25) / 25)
            self.tasks["空调调节"] = progress
            self.tasks["音乐切换"] = min(1.0, progress * 1.3)
            self.tasks["座椅按摩"] = min(1.0, progress * 1.1)
            # Agents finish at different times
            self.agents["Climate"]["status"] = "done" if s >= 50 else "active"
            self.agents["Media"]["status"] = "done" if s >= 38 else "active"
            self.agents["Seat"]["status"] = "done" if s >= 44 else "active"
            if s == 28:
                self.logs.append("[Climate] 目标温度: 26°→22° 风量: +2档")
            if s == 30:
                self.logs.append("[Media] 切换歌单: '提神电子' BPM>140")
            if s == 32:
                self.logs.append("[Seat] 启动腰部按摩 模式: 节奏型")
            if s == 38:
                self.logs.append("[Media] ✓ 已切换到高能歌单")
            if s == 44:
                self.logs.append("[Seat] ✓ 按摩模式已激活")
            if s == 50:
                self.logs.append("[Climate] ✓ 温度已降至 22°C")

        # Phase 4: Complete
        else:
            self.latency_ms = 187
            if s == 51:
                self.logs.append(f"[System] ✓ 全部完成 端到端延迟: {self.latency_ms}ms")

    def draw(self, win, max_y, max_x):
        # Title
        title = "场景: 智能座舱 — 多Agent疲劳缓解"
        try:
            win.addstr(1, 2, title, curses.color_pair(C_TITLE) | curses.A_BOLD)
            win.addstr(2, 2, f"语音输入: {self.voice_input}",
                       curses.color_pair(C_BANNER) | curses.A_BOLD)
        except curses.error:
            pass

        # ─── DAG Panel (core differentiator) ─────────────────────────────
        dag_y = 4
        draw_box(win, dag_y, 1, 9, 78, "Intent DAG — 有向无环图任务拆解")
        self._draw_dag(win, dag_y + 1)

        # Left panel: Task pipeline (only show tasks that DAG has revealed)
        panel_y = dag_y + 10
        draw_box(win, panel_y, 1, 8, 38, "任务流水线")
        row = panel_y + 1
        for label, pct in self.tasks.items():
            # Tasks only appear after Planner generates them
            if label == "语音意图解析":
                draw_progress(win, row, 3, 34, pct, label)
            elif label == "安全验证" and self.step >= 13:
                draw_progress(win, row, 3, 34, pct, label)
            elif label in ("空调调节", "音乐切换", "座椅按摩") and self.step >= 15:
                draw_progress(win, row, 3, 34, pct, label)
            elif self.step >= 15:
                draw_progress(win, row, 3, 34, pct, label)
            row += 1
        if self.verified:
            try:
                win.addstr(row, 3, " 🔒 Formal Verify: PASS",
                           curses.color_pair(C_SUCCESS) | curses.A_BOLD)
            except curses.error:
                pass

        # Right panel: Device + Latency
        draw_box(win, panel_y, 40, 8, 39, "设备 / 延迟")
        try:
            win.addstr(panel_y+1, 42, "NPU: SA8797P  Load: 34%",
                       curses.color_pair(C_DIM))
            win.addstr(panel_y+2, 42, "推理: 42 tok/s  Mem: 1.8GB",
                       curses.color_pair(C_DIM))
            win.addstr(panel_y+3, 42, "KV Cache Hit: 94%  Speculative: 3步",
                       curses.color_pair(C_DIM))
            if self.latency_ms > 0:
                win.addstr(panel_y+5, 42, f"端到端: {self.latency_ms}ms",
                           curses.color_pair(C_SUCCESS) | curses.A_BOLD)
                win.addstr(panel_y+6, 42, "云端对比: ~2400ms  加速: 12.8×",
                           curses.color_pair(C_DIM))
        except curses.error:
            pass

        # Log panel
        log_y = panel_y + 9
        draw_box(win, log_y, 1, 8, 78, "实时日志")
        visible_logs = self.logs[-6:]
        for i, log in enumerate(visible_logs):
            color = C_SUCCESS if "✓" in log else C_DIM
            try:
                win.addstr(log_y + 1 + i, 3, log[:74], curses.color_pair(color))
            except curses.error:
                pass

    def _draw_dag(self, win, y):
        """Draw the DAG topology progressively — nodes appear as Planner generates them."""
        edge_c = curses.color_pair(C_BORDER)
        s = self.step

        def node(name, col_offset, row_offset):
            agent = self.agents.get(name, {"status": "idle"})
            st = agent["status"]
            color_map = {"active": C_PROGRESS_ACTIVE, "done": C_SUCCESS, "idle": C_DIM}
            c = color_map.get(st, C_DIM)
            icon = agent.get("icon", "·")
            bold = curses.A_BOLD if st in ("active", "done") else 0
            label = f"[{icon}{name}]"
            try:
                win.addstr(y + row_offset, col_offset, label,
                           curses.color_pair(c) | bold)
            except curses.error:
                pass
            return col_offset + len(label)

        r = 3  # main row in DAG box

        try:
            # Phase: only Voice visible
            if s < 10:
                node("Voice", 4, r)
                win.addstr(y + 1, 4, "等待语音识别完成...",
                           curses.color_pair(C_DIM))

            # Phase: Voice done, Planner appears and starts thinking
            elif s < 13:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                node("Planner", end1 + 5, r)
                win.addstr(y + 1, 4, "Planner 正在分析意图，生成执行 DAG...",
                           curses.color_pair(C_PROGRESS_ACTIVE))
                # Show "thinking" dots animation
                dots = "." * ((s - 10) % 4)
                win.addstr(y + 5, 4, f"  规划中{dots}",
                           curses.color_pair(C_PROGRESS_ACTIVE))

            # Phase: Planner done → DAG unfolds! Verifier + downstream appear
            elif s < 15:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                end2 = node("Planner", end1 + 5, r)
                win.addstr(y + r, end2, " ──→ ", edge_c)
                end3 = node("Verifier", end2 + 5, r)
                # DAG just generated — show it expanding
                win.addstr(y + 1, 4, "✦ DAG 已生成 — 发现 3 个并行子任务",
                           curses.color_pair(C_SUCCESS) | curses.A_BOLD)
                win.addstr(y + r, end3, " ──┬→ ?", edge_c)
                win.addstr(y + r+1, end3, "   ├→ ?", edge_c)
                win.addstr(y + r+2, end3, "   └→ ?", edge_c)
                win.addstr(y + 6, 4, "验证安全约束后展开执行节点...",
                           curses.color_pair(C_DIM))

            # Phase: Verifier working → downstream nodes materialize
            elif s < 26:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                end2 = node("Planner", end1 + 5, r)
                win.addstr(y + r, end2, " ──→ ", edge_c)
                end3 = node("Verifier", end2 + 5, r)
                win.addstr(y + r, end3, " ──┬→ ", edge_c)
                node("Climate", end3 + 6, r)
                win.addstr(y + r + 1, end3, "   ├→ ", edge_c)
                node("Media", end3 + 6, r + 1)
                win.addstr(y + r + 2, end3, "   └→ ", edge_c)
                node("Seat", end3 + 6, r + 2)
                win.addstr(y + 1, 4,
                           "✦ DAG 展开: Voice→Planner→Verify→{Climate∥Media∥Seat}",
                           curses.color_pair(C_TITLE))
                win.addstr(y + 6, 4,
                           "并行度: 3  临界路径深度: 4  验证中...",
                           curses.color_pair(C_DIM))

            # Phase: Full execution — show complete DAG with all states
            else:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                end2 = node("Planner", end1 + 5, r)
                win.addstr(y + r, end2, " ──→ ", edge_c)
                end3 = node("Verifier", end2 + 5, r)
                win.addstr(y + r, end3, " ──┬→ ", edge_c)
                node("Climate", end3 + 6, r)
                win.addstr(y + r + 1, end3, "   ├→ ", edge_c)
                node("Media", end3 + 6, r + 1)
                win.addstr(y + r + 2, end3, "   └→ ", edge_c)
                node("Seat", end3 + 6, r + 2)
                win.addstr(y + 1, 4,
                           "✓ DAG 执行中: Voice→Planner→Verify→{Climate∥Media∥Seat}",
                           curses.color_pair(C_SUCCESS))
                win.addstr(y + 6, 4,
                           "并行度: 3  临界路径深度: 4  安全验证: PASS ✓",
                           curses.color_pair(C_DIM))
        except curses.error:
            pass
            try:
                win.addstr(log_y + 1 + i, 3, log[:74], curses.color_pair(color))
            except curses.error:
                pass

# ─── Scenario: Drone Perception ───────────────────────────────────────────────

class DroneScenario:
    """Drone perception: '帮我看看前面山区有没有火情隐患'"""

    def __init__(self):
        self.step = 0
        self.max_steps = 70
        self.voice_input = "「帮我看看前面山区有没有火情隐患」"
        self.agents = {
            "Planner":    {"icon": "🧠", "status": "idle"},
            "Voice":      {"icon": "🎙", "status": "idle"},
            "Camera":     {"icon": "📷", "status": "idle"},
            "IR Sensor":  {"icon": "🌡", "status": "idle"},
            "Analyzer":   {"icon": "👁", "status": "idle"},
            "Reporter":   {"icon": "📢", "status": "idle"},
        }
        self.tasks = {
            "语音意图解析": 0.0,
            "红外扫描":     0.0,
            "可见光分析":   0.0,
            "多模态融合":   0.0,
            "风险报告生成": 0.0,
        }
        self.detections = []
        self.logs = []
        self.frame_count = 0
        self.risk_level = ""

    def tick(self):
        self.step += 1
        s = self.step

        # Phase 1: Voice + Planning (1-12)
        if s <= 12:
            self.agents["Voice"]["status"] = "done" if s >= 10 else "active"
            self.tasks["语音意图解析"] = min(1.0, s / 10)
            if s == 1:
                self.logs.append("[Voice] 识别中...")
            if s == 6:
                self.logs.append("[Voice] 意图: FIRE_RISK_SCAN 区域: 前方山区")
            if s == 10:
                self.agents["Planner"]["status"] = "active"
            if s == 12:
                self.agents["Planner"]["status"] = "done"
                self.logs.append("[Planner] 启动双光融合扫描 → IR + RGB 并行")

        # Phase 2: Parallel sensing (13-45)
        elif s <= 45:
            self.agents["Camera"]["status"] = "done" if s >= 38 else "active"
            self.agents["IR Sensor"]["status"] = "done" if s >= 45 else "active"
            ir_progress = min(1.0, (s - 12) / 30)
            rgb_progress = min(1.0, (s - 12) / 28)
            self.tasks["红外扫描"] = ir_progress
            self.tasks["可见光分析"] = rgb_progress
            self.frame_count = (s - 12) * 4

            if s == 18:
                self.logs.append(f"[IR] 扫描中... 帧率: 15fps 分辨率: 640×480")
            if s == 25:
                self.detections.append(("热源A", "312°C", "31.24°N,104.73°E", 0.91))
                self.logs.append("[IR] ⚠ 异常热源 @ 31.24°N,104.73°E  温度: 312°C  置信: 0.91")
            if s == 33:
                self.detections.append(("枯木区B", "dry:0.87", "31.25°N,104.71°E", 0.87))
                self.logs.append("[RGB] ⚠ 枯木聚集区 dry_index=0.87 高风险")
            if s == 38:
                self.logs.append("[RGB] ✓ 可见光扫描完成 共 108 帧")
            if s == 45:
                self.agents["IR Sensor"]["status"] = "done"
                self.logs.append("[IR] 红外扫描完成 共 132 帧")

        # Phase 3: Fusion analysis (46-58)
        elif s <= 58:
            self.agents["Analyzer"]["status"] = "active"
            self.tasks["多模态融合"] = min(1.0, (s - 45) / 13)
            if s == 48:
                self.logs.append("[Fusion] 红外+可见光对齐，交叉验证热源...")
            if s == 52:
                self.logs.append("[Fusion] 热源A确认: 非自然热源，疑似阴燃")
            if s == 55:
                self.logs.append("[Fusion] 枯木区B: 含水率<15%，风险等级: HIGH")
            if s == 58:
                self.agents["Analyzer"]["status"] = "done"
                self.risk_level = "HIGH"

        # Phase 4: Report (59-70)
        elif s <= 70:
            self.agents["Reporter"]["status"] = "active"
            self.tasks["风险报告生成"] = min(1.0, (s - 58) / 10)
            if s == 60:
                self.logs.append("[Report] 生成风险评估报告...")
            if s == 65:
                self.logs.append("[Report] 语音播报: '发现2处火情隐患，建议派人确认'")
            if s == 68:
                self.logs.append("[Report] 坐标已标注并回传地面站")
            if s == 70:
                self.agents["Reporter"]["status"] = "done"
                self.logs.append("[System] ✓ 感知任务完成 检测隐患: 2处 风险: HIGH")

    def draw(self, win, max_y, max_x):
        try:
            win.addstr(1, 2, "场景: 无人机感知 — 山区火情隐患扫描",
                       curses.color_pair(C_TITLE) | curses.A_BOLD)
            win.addstr(2, 2, f"语音输入: {self.voice_input}",
                       curses.color_pair(C_BANNER) | curses.A_BOLD)
        except curses.error:
            pass

        # ─── DAG Panel ────────────────────────────────────────────────────
        dag_y = 4
        draw_box(win, dag_y, 1, 9, 78, "Perception DAG — 感知链路拓扑")
        self._draw_dag(win, dag_y + 1)

        # Left panel: Pipeline
        panel_y = dag_y + 10
        draw_box(win, panel_y, 1, 8, 38, "感知流水线")
        row = panel_y + 1
        for label, pct in self.tasks.items():
            draw_progress(win, row, 3, 34, pct, label)
            row += 1
        if self.risk_level:
            try:
                win.addstr(row, 3, f" ⚠ 风险等级: {self.risk_level}",
                           curses.color_pair(C_ALERT) | curses.A_BOLD)
            except curses.error:
                pass

        # Right panel: Detections + Device
        draw_box(win, panel_y, 40, 8, 39, "检测结果 / 设备")
        try:
            win.addstr(panel_y+1, 42,
                       f"帧数: {self.frame_count}  隐患: {len(self.detections)}",
                       curses.color_pair(C_DIM))
            for i, (name, val, gps, conf) in enumerate(self.detections[:2]):
                c = C_ALERT if conf > 0.9 else C_PROGRESS_ACTIVE
                win.addstr(panel_y+2+i, 42, f"🔴 {name} {val} cf:{conf:.2f}",
                           curses.color_pair(c))
            win.addstr(panel_y+5, 42, "NPU: SA8797P  Load: 67%",
                       curses.color_pair(C_DIM))
            win.addstr(panel_y+6, 42, "电量: 72%  GPS: 31.24°N  Alt: 120m",
                       curses.color_pair(C_DIM))
        except curses.error:
            pass

        # Log panel
        log_y = panel_y + 9
        draw_box(win, log_y, 1, 8, 78, "实时日志")
        visible_logs = self.logs[-6:]
        for i, log in enumerate(visible_logs):
            color = C_ALERT if "⚠" in log else (C_SUCCESS if "✓" in log else C_DIM)
            try:
                win.addstr(log_y + 1 + i, 3, log[:74], curses.color_pair(color))
            except curses.error:
                pass

    def _draw_dag(self, win, y):
        """Draw the perception DAG progressively as Planner generates it."""
        edge_c = curses.color_pair(C_BORDER)
        s = self.step

        def node(name, col_offset, row_offset):
            agent = self.agents.get(name, {"status": "idle"})
            st = agent["status"]
            color_map = {"active": C_PROGRESS_ACTIVE, "done": C_SUCCESS, "idle": C_DIM}
            c = color_map.get(st, C_DIM)
            icon = agent.get("icon", "·")
            bold = curses.A_BOLD if st in ("active", "done") else 0
            label = f"[{icon}{name}]"
            try:
                win.addstr(y + row_offset, col_offset, label,
                           curses.color_pair(c) | bold)
            except curses.error:
                pass
            return col_offset + len(label)

        r = 3  # main row

        try:
            # Phase: only Voice
            if s < 10:
                node("Voice", 4, r)
                win.addstr(y + 1, 4, "等待语音识别...",
                           curses.color_pair(C_DIM))

            # Phase: Voice done, Planner thinking
            elif s < 13:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                node("Planner", end1 + 5, r)
                dots = "." * ((s - 10) % 4)
                win.addstr(y + 1, 4, f"Planner 分析意图，构建感知 DAG{dots}",
                           curses.color_pair(C_PROGRESS_ACTIVE))
                win.addstr(y + 5, 4, "  推理: 目标='火情隐患' → 需要多模态感知",
                           curses.color_pair(C_DIM))

            # Phase: DAG generated — parallel branches appear
            elif s < 46:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                end2 = node("Planner", end1 + 5, r)
                win.addstr(y + r, end2, " ─┤", edge_c)
                # Top branch
                win.addstr(y + r - 1, end2 + 3, "┌→ ", edge_c)
                node("Camera", end2 + 6, r - 1)
                win.addstr(y + r - 1, end2 + 18, "──┐", edge_c)
                # Bottom branch
                win.addstr(y + r + 1, end2 + 3, "└→ ", edge_c)
                node("IR Sensor", end2 + 6, r + 1)
                win.addstr(y + r + 1, end2 + 19, "─┘", edge_c)
                # Merge point → Analyzer (show as ? until sensing done)
                if s < 40:
                    win.addstr(y + r, end2 + 21, "├→ ?", edge_c)
                    win.addstr(y + 1, 4,
                               "✦ DAG: 双光并行采集 → 待融合分析",
                               curses.color_pair(C_TITLE))
                else:
                    win.addstr(y + r, end2 + 21, "├→ ", edge_c)
                    node("Analyzer", end2 + 24, r)
                    win.addstr(y + 1, 4,
                               "✦ DAG: 采集完成 → 融合分析节点激活",
                               curses.color_pair(C_TITLE))
                win.addstr(y + 6, 4,
                           "并行度: 2 (IR∥RGB)  模态: 红外+可见光",
                           curses.color_pair(C_DIM))

            # Phase: Fusion + Report — full DAG visible
            else:
                end1 = node("Voice", 4, r)
                win.addstr(y + r, end1, " ──→ ", edge_c)
                end2 = node("Planner", end1 + 5, r)
                win.addstr(y + r, end2, " ─┤", edge_c)
                win.addstr(y + r - 1, end2 + 3, "┌→ ", edge_c)
                node("Camera", end2 + 6, r - 1)
                win.addstr(y + r - 1, end2 + 18, "──┐", edge_c)
                win.addstr(y + r + 1, end2 + 3, "└→ ", edge_c)
                node("IR Sensor", end2 + 6, r + 1)
                win.addstr(y + r + 1, end2 + 19, "─┘", edge_c)
                win.addstr(y + r, end2 + 21, "├→ ", edge_c)
                end3 = node("Analyzer", end2 + 24, r)
                win.addstr(y + r, end3, " ──→ ", edge_c)
                node("Reporter", end3 + 5, r)
                win.addstr(y + 1, 4,
                           "✓ DAG 完整执行: Voice→Plan→{IR∥RGB}→Fuse→Report",
                           curses.color_pair(C_SUCCESS))
                win.addstr(y + 6, 4,
                           "并行度: 2  临界路径深度: 5  全链路端侧推理",
                           curses.color_pair(C_DIM))
        except curses.error:
            pass

# ─── Main Loop ────────────────────────────────────────────────────────────────

def draw_banner_screen(stdscr):
    """Draw the startup banner with scenario selection."""
    stdscr.clear()
    lines = BANNER.strip("\n").split("\n")
    for i, line in enumerate(lines):
        try:
            stdscr.addstr(i + 1, 2, line, curses.color_pair(C_BANNER) | curses.A_BOLD)
        except curses.error:
            pass
    y = len(lines) + 2
    try:
        stdscr.addstr(y, 2, "─" * 50, curses.color_pair(C_BORDER))
        stdscr.addstr(y+1, 2, f"  Edge AI Agent Framework │ v{VERSION}",
                      curses.color_pair(C_TITLE))
        stdscr.addstr(y+2, 2, "  On-Device NPU │ Multi-Agent │ Formal Verify",
                      curses.color_pair(C_DIM))
        stdscr.addstr(y+3, 2, "─" * 50, curses.color_pair(C_BORDER))
        stdscr.addstr(y+5, 2, "选择演示场景:", curses.color_pair(C_TITLE) | curses.A_BOLD)
        stdscr.addstr(y+7, 4, "[1] 智能座舱 — 多Agent疲劳缓解", curses.color_pair(C_DIM))
        stdscr.addstr(y+8, 4, "[2] 无人机感知 — 山区火情隐患扫描", curses.color_pair(C_DIM))
        stdscr.addstr(y+10, 4, "[q] 退出", curses.color_pair(C_DIM))
    except curses.error:
        pass
    stdscr.refresh()


def run_scenario(stdscr, scenario):
    """Animate a scenario step by step."""
    stdscr.nodelay(True)
    while scenario.step < scenario.max_steps:
        scenario.tick()
        stdscr.clear()
        max_y, max_x = stdscr.getmaxyx()
        scenario.draw(stdscr, max_y, max_x)
        # Footer
        try:
            stdscr.addstr(max_y - 1, 2,
                          "[Space] 暂停/继续  [r] 重置  [q] 返回菜单",
                          curses.color_pair(C_DIM))
        except curses.error:
            pass
        stdscr.refresh()
        time.sleep(0.12)

        # Handle input
        try:
            ch = stdscr.getch()
        except:
            ch = -1
        if ch == ord('q'):
            return
        elif ch == ord('r'):
            scenario.__init__()
        elif ch == ord(' '):
            # Pause
            stdscr.nodelay(False)
            stdscr.getch()
            stdscr.nodelay(True)

    # Hold final frame
    stdscr.nodelay(False)
    try:
        max_y, max_x = stdscr.getmaxyx()
        stdscr.addstr(max_y - 1, 2,
                      "✓ 演示完成 — 按任意键返回菜单",
                      curses.color_pair(C_SUCCESS) | curses.A_BOLD)
    except curses.error:
        pass
    stdscr.refresh()
    stdscr.getch()


def main(stdscr):
    curses.curs_set(0)
    init_colors()

    while True:
        draw_banner_screen(stdscr)
        ch = stdscr.getch()
        if ch == ord('1'):
            run_scenario(stdscr, CockpitScenario())
        elif ch == ord('2'):
            run_scenario(stdscr, DroneScenario())
        elif ch == ord('q'):
            break


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="OpenSparX AgentOS TUI Demo")
    parser.add_argument("--scenario", choices=["cockpit", "drone"],
                        help="Jump directly to a scenario")
    args = parser.parse_args()

    if args.scenario:
        def _main(stdscr):
            curses.curs_set(0)
            init_colors()
            if args.scenario == "cockpit":
                run_scenario(stdscr, CockpitScenario())
            else:
                run_scenario(stdscr, DroneScenario())
        curses.wrapper(_main)
    else:
        curses.wrapper(main)

