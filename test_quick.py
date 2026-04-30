#!/usr/bin/env python3
"""快速验证Bug修复：通过SSE API"""
import json, urllib.request, urllib.parse, sys, time

BASE = "http://localhost:5000"

def sse(url_suffix, timeout=120):
    """读取SSE流，返回所有事件"""
    events = []
    url = BASE + url_suffix
    with urllib.request.urlopen(url, timeout=timeout) as r:
        buf = b""
        while True:
            chunk = r.read(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n\n" in buf:
                block, buf = buf.split(b"\n\n", 1)
                event_type = None
                data_raw = None
                for line in block.decode().strip().split("\n"):
                    if line.startswith("event:"):
                        event_type = line[6:].strip()
                    elif line.startswith("data:"):
                        data_raw = line[5:].strip()
                if event_type and data_raw:
                    try:
                        evt = json.loads(data_raw)
                        evt["event"] = event_type
                        events.append(evt)
                    except:
                        pass
    return events

def check_done(events):
    """检查done事件的mode, score, content"""
    for e in events:
        if e.get("event") == "done":
            mode = e.get("mode", "")
            if mode not in ("closed", "open"):
                return f"Bug⑥: mode={mode}"
            scores = e.get("scores", {})
            overall = scores.get("overall", 0)
            if overall == 0.5:
                return f"Bug③: score=0.5(50分)"
            content = e.get("content", "")
            if len(content) < 20:
                return f"Bug⑤: content太短"
            return f"OK: mode={mode}, score={overall}, content_len={len(content)}"
    return "无done事件"

def check_done_dag(events):
    """DAG的done检查"""
    for e in events:
        if e.get("event") == "done":
            mode = e.get("mode", "")
            if mode not in ("closed", "open"):
                return f"Bug⑥: mode={mode}"
            scores = e.get("scores", {})
            overall = scores.get("overall", 0)
            if overall <= 0:
                return f"Bug①: DAG score={overall}"
            content = e.get("content", "")
            if len(content) < 20:
                return f"Bug②: DAG output截断"
            return f"OK: mode={mode}, score={overall}, content_len={len(content)}"
    return "无done事件"

# ===== 测试 1: 单闭环 =====
print("▶ 单闭环 Closed Loop...", end=" ", flush=True)
try:
    events = sse("/api/process/stream?query=用Python写一个计算1%2B1的程序", timeout=120)
    print(check_done(events))
except Exception as e:
    print(f"💥 {e}")
