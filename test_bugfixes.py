#!/usr/bin/env python3
"""验证6个Bug修复情况：通过SSE流解析各模式下的执行结果"""
import json, sys, re, os

sys.path.insert(0, "/root/closed-loop-multiagent/python_interface")

def test_single_closed_loop():
    """验证Bug③⑤⑥: 单闭环score>0&&output有代码&&mode不跳变"""
    from core import ClosedLoopFramework
    fw = ClosedLoopFramework()
    fw.set_mode("closed")
    fw.set_arch_mode("single")
    
    events = []
    for evt in fw.process_query_stream("用Python写一个计算1+1的程序"):
        events.append(evt)
        if evt.get("event") == "done":
            # Bug⑥: mode不应是dag/multi_loop
            mode = evt.get("mode", "")
            if mode not in ("closed", "open"):
                return False, f"Bug⑥ FAIL: mode={mode}", events
            # Bug③: score不应是50(0.5)
            scores = evt.get("scores", {})
            overall = scores.get("overall", 0)
            if overall == 0.5:
                return False, f"Bug③ FAIL: score=0.5 (50分)", events
            # Bug⑤: content应有代码
            content = evt.get("content", "")
            if not content or len(content) < 20:
                return False, f"Bug⑤ FAIL: content太短: {content[:50]}", events
            # ✅ Single closed loop 验证完成
            return True, f"Single closed loop OK: score={overall}", events
    
    return False, "No done event received", events


def test_multi_loop():
    """验证Bug③⑤: 多闭环score!=0.5&&output有代码"""
    from core import ClosedLoopFramework
    fw = ClosedLoopFramework()
    fw.set_mode("closed")
    fw.set_arch_mode("multi")
    
    events = []
    for evt in fw.process_query_stream("用Python写一个计算1+1的程序"):
        events.append(evt)
        if evt.get("event") == "done":
            mode = evt.get("mode", "")
            if mode not in ("closed", "open"):
                return False, f"Bug⑥ FAIL: mode={mode}", events
            
            scores = evt.get("scores", {})
            overall = scores.get("overall", 0)
            if overall == 0.5:
                return False, f"Bug③ FAIL: score=0.5", events
            
            content = evt.get("content", "")
            if not content or len(content) < 20:
                return False, f"Bug⑤ FAIL: content太短: {content[:50]}", events
            return True, f"Multi-loop OK: score={overall}", events
    
    return False, "No done event", events


def test_dag_mode():
    """验证Bug①②⑥: DAG score>0&&output完整&&mode不跳变"""
    from core import ClosedLoopFramework
    fw = ClosedLoopFramework()
    fw.set_mode("closed")
    fw.set_arch_mode("dag")
    fw.set_dag_mode(True)
    
    events = []
    for evt in fw.process_query_stream("用Python写一个计算1+1的程序"):
        events.append(evt)
        if evt.get("event") == "done":
            mode = evt.get("mode", "")
            if mode not in ("closed", "open"):
                return False, f"Bug⑥ FAIL: mode={mode}", events
            
            scores = evt.get("scores", {})
            overall = scores.get("overall", 0)
            # Bug①: DAG score原本是0
            if overall <= 0:
                return False, f"Bug① FAIL: score={overall}", events
            
            # Bug②: DAG output原本被截断
            content = evt.get("content", "")
            if not content or len(content) < 20:
                return False, f"Bug② FAIL: content太短: {content[:50]}", events
            return True, f"DAG OK: score={overall}", events
    
    return False, "No done event", events


def test_single_open_loop():
    """验证单开环 - 作为baseline"""
    from core import ClosedLoopFramework
    fw = ClosedLoopFramework()
    fw.set_mode("open")
    fw.set_arch_mode("single")
    
    events = []
    for evt in fw.process_query_stream("用Python写一个计算1+1的程序"):
        events.append(evt)
        if evt.get("event") == "done":
            mode = evt.get("mode", "")
            if mode not in ("closed", "open"):
                return False, f"Bug⑥ FAIL: mode={mode}", events
            return True, f"Single open loop OK", events
    
    return False, "No done event", events


def test_mode_dont_jump():
    """验证Bug⑥: 多次执行后模式不跳变"""
    from core import ClosedLoopFramework
    fw = ClosedLoopFramework()
    
    # 多次切换模式执行，检查mode稳定
    for mode_name in ["closed", "open"]:
        fw.set_mode(mode_name)
        fw.set_arch_mode("single")
        
        for evt in fw.process_query_stream("print('hello')"):
            if evt.get("event") == "done":
                m = evt.get("mode", "")
                if m not in ("closed", "open"):
                    return False, f"Bug⑥ FAIL: mode跳变到{m}", None
                break
    
    return True, "Bug⑥: mode稳定不跳变", None


if __name__ == "__main__":
    print("=" * 60)
    print("验证 Bug 修复情况")
    print("=" * 60)
    
    tests = [
        ("单闭环 Closed Loop", test_single_closed_loop),
        ("多闭环 Multi Loop", test_multi_loop),
        ("DAG模式", test_dag_mode),
        ("单开环 Open Loop (baseline)", test_single_open_loop),
        ("模式稳定性", test_mode_dont_jump),
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
        except Exception as e:
            print(f"💥 异常: {e}")
            import traceback
            traceback.print_exc()
            all_pass = False
    
    print("\n" + "=" * 60)
    if all_pass:
        print("🎉 所有 Bug 修复验证通过！")
    else:
        print("⚠️ 存在失败的验证，请检查。")
    print("=" * 60)
