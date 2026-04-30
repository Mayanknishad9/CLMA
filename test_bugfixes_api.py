#!/usr/bin/env python3
"""验证6个Bug修复情况：通过 Web API 接口测试"""
import json, sys, os, urllib.request, urllib.parse

BASE = "http://localhost:5000"

def api_get(path):
    with urllib.request.urlopen(f"{BASE}{path}") as r:
        return json.loads(r.read().decode())

def api_post(path, data):
    body = json.dumps(data).encode()
    r = urllib.request.Request(f"{BASE}{path}", data=body,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r) as resp:
        return json.loads(resp.read().decode())

def sse_execute(query, mode="closed", arch="single"):
    """执行query并通过SSE读取结果"""
    params = urllib.parse.urlencode({"query": query, "mode": mode, "arch": arch})
    url = f"{BASE}/api/stream?{params}"
    
    events = []
    with urllib.request.urlopen(url, timeout=120) as r:
        buf = b""
        while True:
            chunk = r.read(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n\n" in buf:
                block, buf = buf.split(b"\n\n", 1)
                evt = parse_sse_event(block.decode())
                if evt:
                    events.append(evt)
                    if evt.get("event") == "done":
                        return events
    return events

def parse_sse_event(text):
    """解析单条SSE事件"""
    event_type = None
    data = None
    for line in text.strip().split("\n"):
        if line.startswith("event:"):
            event_type = line[6:].strip()
        elif line.startswith("data:"):
            raw = line[5:].strip()
            try:
                data = json.loads(raw)
            except:
                data = raw
    if event_type and data:
        if isinstance(data, str):
            data = {"raw": data}
        data["event"] = event_type
        return data
    return None

def test_single():
    """Bug③⑤⑥"""
    events = sse_execute("用Python写一个计算1+1的程序", mode="closed", arch="single")
    done = [e for e in events if e.get("event") == "done"]
    if not done:
        return False, "无done事件", events
    d = done[0]
    mode = d.get("mode", "")
    if mode not in ("closed", "open"):
        return False, f"Bug⑥ mode跳变: {mode}", events
    scores = d.get("scores", {})
    overall = scores.get("overall", 0)
    if overall == 0.5:
        return False, f"Bug③ score=50分", events
    content = d.get("content", "")
    if len(content) < 20:
        return False, f"Bug⑤ content太短: {content[:50]}", events
    return True, f"✅ Single closed: score={overall}", events

def test_multi():
    events = sse_execute("用Python写一个计算1+1的程序", mode="closed", arch="multi")
    done = [e for e in events if e.get("event") == "done"]
    if not done:
        return False, "无done事件", events
    d = done[0]
    mode = d.get("mode", "")
    if mode not in ("closed", "open"):
        return False, f"Bug⑥ mode跳变: {mode}", events
    scores = d.get("scores", {})
    overall = scores.get("overall", 0)
    if overall == 0.5:
        return False, f"Bug③ score=50分", events
    content = d.get("content", "")
    if len(content) < 20:
        return False, f"Bug⑤ content太短", events
    # 检查内环事件
    inner_its = [e for e in events if e.get("event") == "inner_iteration"]
    return True, f"✅ Multi-loop: score={overall}, inner_iters={len(inner_its)}", events

def test_dag():
    """Bug①②⑥"""
    events = sse_execute("用Python写一个计算1+1的程序", mode="closed", arch="dag")
    done = [e for e in events if e.get("event") == "done"]
    if not done:
        return False, "无done事件", events
    d = done[0]
    mode = d.get("mode", "")
    if mode not in ("closed", "open"):
        return False, f"Bug⑥ mode跳变: {mode}", events
    scores = d.get("scores", {})
    overall = scores.get("overall", 0)
    if overall <= 0:
        return False, f"Bug① DAG score={overall}(应为>0)", events
    content = d.get("content", "")
    if len(content) < 20:
        return False, f"Bug② DAG output截断: {content[:50]}", events
    return True, f"✅ DAG: score={overall}", events

def test_open():
    events = sse_execute("用Python写一个计算1+1的程序", mode="open", arch="single")
    done = [e for e in events if e.get("event") == "done"]
    if not done:
        return False, "无done事件", events
    d = done[0]
    mode = d.get("mode", "")
    if mode not in ("closed", "open"):
        return False, f"Bug⑥ mode跳变: {mode}", events
    return True, "✅ Single open OK", events

def test_mode_stability():
    """Bug⑥"""
    for mode in ["closed", "open"]:
        events = sse_execute("print('hi')", mode=mode, arch="single")
        done = [e for e in events if e.get("event") == "done"]
        if done:
            m = done[0].get("mode", "")
            if m not in ("closed", "open"):
                return False, f"mode跳变到{m}", events
    return True, "✅ mode稳定不跳变", None

if __name__ == "__main__":
    print("=" * 60)
    print("验证 Bug 修复情况 (via Web API)")
    print("=" * 60)
    
    tests = [
        ("单闭环 Closed Loop", test_single),
        ("多闭环 Multi Loop", test_multi),
        ("DAG模式", test_dag),
        ("单开环 Open Loop", test_open),
        ("模式稳定性", test_mode_stability),
    ]
    
    all_pass = True
    for name, fn in tests:
        print(f"\n▶ 测试【{name}】...", end=" ", flush=True)
        try:
            ok, msg, evts = fn()
            if ok:
                print(f"✅ {msg}")
            else:
                print(f"❌ {msg}")
                all_pass = False
                if evts:
                    for e in evts[-3:]:
                        print(f"   last event: {e.get('event')} → {str(e)[:120]}")
        except Exception as e:
            print(f"💥 {e}")
            import traceback
            traceback.print_exc()
            all_pass = False
    
    print("\n" + "=" * 60)
    if all_pass:
        print("🎉 所有 Bug 修复验证通过！")
    else:
        print("⚠️ 存在失败的验证，请检查。")
    print("=" * 60)
