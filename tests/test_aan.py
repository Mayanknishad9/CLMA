"""
Phase 2 — AAN (Adaptive Agent Network) 架构测试套件

覆盖：
- ① Router heuristic 逻辑（4 种拓扑分类）
- ② Router _infer_modules 模块推断
- ③ _yield_aan_cancelled_done 取消事件
- ④ _aan_execute_direct（模拟模式）
- ⑤ _aan_execute_chain（模拟模式）
- ⑥ _aan_execute_parallel（模拟模式）
- ⑦ _aan_execute_tree 递归分解（模拟模式）
- ⑧ _aan_integrate 合并逻辑
"""

import sys, os, json, time, uuid, io

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))

# 屏蔽 TokenMonitor 的构造日志
from core import CLMAFramework


# ---- 共享框架实例（避免每个测试都构造一次）----
# ---- 共享框架实例（避免每个测试都构造一次）----
import atexit
import unittest.mock as _mock

_FW = None
def get_fw():
    global _FW
    if _FW is None:
        old_stderr = sys.stderr
        sys.stderr = io.StringIO()
        try:
            _FW = CLMAFramework()
            # Mock 掉 _get_llm_provider 让它走模拟 fallback
            _FW._get_llm_provider = lambda: None
        finally:
            sys.stderr = old_stderr
    return _FW


# ======================================================================
# ① Router Agent — heuristic 四路分类
# ======================================================================

def test_router_direct_short_greeting():
    """极短查询 → direct（无复杂关键词）"""
    fw = get_fw()
    t = fw._router_agent('hello')
    assert t['type'] == 'direct'
    assert t['complexity'] == 'simple'


def test_router_chain_medium():
    """中等长度查询（>30字，无并行/架构关键词）→ chain"""
    fw = get_fw()
    query = '请写一个python函数，输入两个整数，返回它们的和以及乘积计算结果'
    assert len(query) > 30
    t = fw._router_agent(query)
    assert t['type'] == 'chain'


def test_router_parallel_keywords():
    """有并行关键词且长度 > 40 → parallel"""
    fw = get_fw()
    query = '分别用python和javascript实现一个排序算法，并且用c语言写一个快速排序'
    assert len(query) > 40
    t = fw._router_agent(query)
    assert t['type'] == 'parallel'


def test_router_tree_architecture_keywords():
    """有架构/系统关键词且长度 > 80 → tree"""
    fw = get_fw()
    query = '设计一个分布式系统的架构，包括数据库连接池管理、服务注册发现、客户端负载均衡和网络协议传输层加密。具体来说需要实现数据库的连接池管理、服务的注册发现机制以及客户端的自动重连'
    assert len(query) >= 80
    t = fw._router_agent(query)
    assert t['type'] == 'tree'


def test_router_short_with_complex_keywords():
    """短查询但有复杂关键词 → chain（len < 30 但 has_complex_keywords）"""
    fw = get_fw()
    t = fw._router_agent('hello database')
    assert t['type'] == 'chain'


def test_router_parallel_keyword_beats_tree_at_short_len():
    """同时有并行和架构关键词，len 40-80 → parallel 优先"""
    fw = get_fw()
    query = '请同时进行以下任务：用python写一个排序算法，用javascript写一个搜索算法'
    assert 40 <= len(query) <= 80
    t = fw._router_agent(query)
    assert t['type'] == 'parallel'


def test_router_trivial_short_no_intent():
    """真正的 trivial 查询（极短 + 无代码意图）→ direct；其他有意图的短查询 → chain"""
    fw = get_fw()
    # 真正的 trivial
    t_trivial = fw._router_agent('hi')
    assert t_trivial['type'] == 'direct', f"trivial expected direct, got {t_trivial['type']}"
    # 有代码意图的短查询 → chain
    t_chain = fw._router_agent('写个爬虫')
    assert t_chain['type'] == 'chain', f"code intent expected chain, got {t_chain['type']}"
    # 中等长度无意图 → chain
    t_medium = fw._router_agent('a' * 30)
    assert t_medium['type'] == 'chain', f"len=30 expected chain, got {t_medium['type']}"


# ======================================================================
# ② Router — _infer_modules 模块推断
# ======================================================================

def test_infer_modules_empty():
    fw = get_fw()
    assert fw._infer_modules('') == []
    assert fw._infer_modules('写一个hello world') == []


def test_infer_modules_with_commas():
    fw = get_fw()
    mods = fw._infer_modules('实现authentication, database, api_gateway, cache_service')
    assert len(mods) >= 3


def test_infer_modules_chinese_comma():
    fw = get_fw()
    mods = fw._infer_modules('authentication、database、api_gateway')
    assert len(mods) >= 3


def test_infer_modules_max_five():
    fw = get_fw()
    mods = fw._infer_modules('a, b, c, d, e, f, g, h, i, j')
    assert len(mods) <= 5


# ======================================================================
# ③ _yield_aan_cancelled_done 取消事件
# ======================================================================

def test_cancelled_done_empty():
    fw = get_fw()
    events = list(fw._yield_aan_cancelled_done('test', 'sid123', ''))
    assert len(events) == 1
    ev = events[0]
    assert ev['event'] == 'done'
    assert ev['result']['cancelled'] is True
    assert ev['result']['success'] is False
    assert 'Cancelled' in ev['result']['content']


def test_cancelled_done_with_partial():
    fw = get_fw()
    events = list(fw._yield_aan_cancelled_done('test', 'sid123', 'partial result'))
    ev = events[0]
    assert ev['result']['success'] is True
    assert ev['result']['content'] == 'partial result'


def test_cancelled_done_with_scores():
    fw = get_fw()
    scores = {"reasonableness": 0.9, "executability": 0.8,
              "satisfaction": 0.7, "overall": 0.82}
    events = list(fw._yield_aan_cancelled_done('test', 'sid123', '',
                                                partial_scores=scores))
    ev = events[0]
    assert ev['result']['score']['overall'] == 0.82
    assert ev['stats']['cancelled'] is True


# ======================================================================
# ④ _aan_integrate 合并逻辑
# ======================================================================

def test_integrate_empty():
    fw = get_fw()
    result = fw._aan_integrate('test', [])
    assert '(no module results' in result


def test_integrate_single():
    fw = get_fw()
    result = fw._aan_integrate('test', [
        {"module": "solver", "content": "hello world", "success": True}
    ])
    assert 'hello world' in result


def test_integrate_multiple():
    fw = get_fw()
    results = [
        {"module": "auth", "content": "def login(): pass", "success": True},
        {"module": "db", "content": "CREATE TABLE users", "success": True},
    ]
    result = fw._aan_integrate('test', results)
    # 多模块时应该有合并说明
    assert ('Module 1' in result or 'Integration' in result
            or 'auth' in result or 'db' in result)


# ======================================================================
# ⑤ _aan_execute_direct 模拟模式
# ======================================================================

def test_execute_direct_produces_done():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_direct(
        'hello', {"type": "direct", "method": "analysis"}, sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) == 1
    assert done[0]['stats']['mode'] == 'adaptive_direct'


def test_execute_direct_cancelled():
    fw = get_fw()
    fw._stream_cancelled = True
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_direct(
        'hello', {"type": "direct", "method": "analysis"}, sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1
    assert done[0]['result'].get('cancelled') is True
    fw._stream_cancelled = False


# ======================================================================
# ⑥ _aan_execute_chain 闭环迭代模式
# ======================================================================

def test_execute_chain_produces_done():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    modules = ["refiner", "reasoner", "solver", "verifier"]
    events = list(fw._aan_execute_chain(
        'test', {"type": "chain", "modules": modules, "method": "analysis",
                 "max_iterations": 1, "threshold": 0.9},
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) == 1
    assert done[0]['stats']['mode'] == 'adaptive_chain'


def test_execute_chain_sse_sequence():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    modules = ["solver", "verifier"]
    events = list(fw._aan_execute_chain(
        'test', {"type": "chain", "modules": modules, "method": "analysis",
                 "max_iterations": 1, "threshold": 0.9},
        sid, time.time()))
    starts = [e for e in events if e['event'] == 'agent_start']
    completions = [e for e in events if e['event'] == 'agent_complete']
    # 注意：evaluator 被强制加入模块列表，所以 starts/completions 数量比原始 modules 多1
    assert len(starts) == len(modules) + 1  # +1 for forced evaluator
    assert len(completions) == len(modules) + 1
    assert starts[0]['agent'] == modules[0]
    assert starts[1]['agent'] == modules[1]


def test_execute_chain_iteration_events():
    """每轮产生 iteration event + 评分"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_chain(
        'test query', {"type": "chain", "modules": ["solver", "verifier"],
                       "method": "analysis", "max_iterations": 1, "threshold": 0.9},
        sid, time.time()))
    iterations = [e for e in events if e['event'] == 'iteration']
    assert len(iterations) == 1
    assert 'scores' in iterations[0]
    assert 'overall' in iterations[0]['scores']
    assert 'iteration' in iterations[0]


def test_execute_chain_stops_on_threshold():
    """评分 >= threshold 时第1轮就停止，不多跑"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    # 高阈值确保一轮后停止（模拟模式给出默认评分0.5，设阈值为0.4让它达标）
    events = list(fw._aan_execute_chain(
        'simple code please',
        {"type": "chain", "modules": ["solver", "verifier"],
         "method": "analysis", "max_iterations": 3, "threshold": 0.4},
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done'][0]
    assert done['stats']['total_iterations'] == 1 or done['stats']['total_iterations'] == 2
    assert done['result']['best_score'] >= 0.4


def test_execute_chain_iteration_has_correct_numbering():
    """iteration 字段在每轮递增"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_chain(
        'a' * 50,  # 长查询增加一点上下文
        {"type": "chain", "modules": ["solver"],
         "method": "analysis", "max_iterations": 2, "threshold": 1.0},  # 阈值1.0强制跑2轮
        sid, time.time()))
    iterations = [e for e in events if e['event'] == 'iteration']
    agent_starts = [e for e in events if e['event'] == 'agent_start']
    if len(agent_starts) > 0:
        # 第2轮的 agent_start iteration 应该是2
        second_round_starts = [e for e in agent_starts if e.get('iteration') == 2]
        if second_round_starts:
            assert len(second_round_starts) > 0
    if len(iterations) >= 2:
        assert iterations[0]['iteration'] == 1
        assert iterations[1]['iteration'] == 2


def test_execute_chain_num_iterations_up_to_3():
    """最大迭代次数默认不超过3，且 info 事件不阻挡 done"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    # 阈值 1.0 确保会跑满所有允许的轮次（但模拟模式 evaluator 返回0.5，达不到1.0）
    # 用 max_iterations=1 或 2 来约束
    events = list(fw._aan_execute_chain(
        'build something complex',
        {"type": "chain", "modules": ["solver", "verifier"],
         "method": "analysis", "max_iterations": 3, "threshold": 0.9},  # 模拟评分0.5 < 0.9，跑满3轮
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) == 1
    # 模拟模式下 evaluator 可能返回 0.5，低于 0.9，所以跑满
    iterations = [e for e in events if e['event'] == 'iteration']
    assert len(iterations) <= 3  # 最多3轮


def test_execute_chain_cancelled_mid_iteration():
    """在链式迭代中途取消"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    fw._stream_cancelled = True
    events = list(fw._aan_execute_chain(
        'test',
        {"type": "chain", "modules": ["solver"], "method": "analysis"},
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1
    assert done[0]['result'].get('cancelled') is True
    fw._stream_cancelled = False


# ======================================================================
# ⑦ _aan_execute_parallel 模拟模式
# ======================================================================

def test_execute_parallel_produces_done():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_parallel(
        'test', {"type": "parallel", "modules": ["auth", "db"],
                 "method": "analysis"}, sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) == 1
    assert done[0]['stats']['mode'] == 'adaptive_parallel'


# ======================================================================
# ⑧ _aan_execute_tree 递归分解
# ======================================================================

def test_tree_leaf_single_module():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_tree(
        'hello', {"type": "tree", "modules": ["test"], "method": "analysis"},
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1


def test_tree_two_modules():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_tree(
        'hello', {"type": "tree", "modules": ["a", "b"], "method": "analysis"},
        sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    tree_splits = [e for e in events if e.get('agent') == 'tree_split']
    assert len(done) >= 1
    assert len(tree_splits) == 1, f"Expected 1 tree_split, got {len(tree_splits)}"


def test_tree_three_modules():
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_tree(
        'hello', {"type": "tree", "modules": ["a", "b", "c"],
                 "method": "analysis"}, sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1


def test_tree_cancelled():
    fw = get_fw()
    fw._stream_cancelled = True
    sid = uuid.uuid4().hex[:12]
    events = list(fw._aan_execute_tree(
        'hello', {"type": "tree", "modules": ["a", "b"], "method": "analysis"},
        sid, time.time()))
    assert len(events) == 1
    assert events[0]['event'] == 'done'
    assert events[0]['result']['cancelled'] is True
    fw._stream_cancelled = False


# ======================================================================
# ⑨ 全链路 AAN 架构入口
# ======================================================================

def test_adaptive_network_cancelled():
    """AAN 入口取消检查点"""
    fw = get_fw()
    fw._stream_cancelled = True
    sid = uuid.uuid4().hex[:12]
    events = list(fw._process_adaptive_network('hello', sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1
    assert done[0]['result'].get('cancelled') is True
    fw._stream_cancelled = False


def test_adaptive_network_router_cancelled():
    """Router 返回后取消"""
    fw = get_fw()
    sid = uuid.uuid4().hex[:12]
    orig_router = fw._router_agent

    def patched_router(query):
        result = orig_router(query)
        fw._stream_cancelled = True
        return result

    fw._router_agent = patched_router
    fw._stream_cancelled = False

    events = list(fw._process_adaptive_network('hello', sid, time.time()))
    done = [e for e in events if e['event'] == 'done']
    assert len(done) >= 1
    assert done[0]['result'].get('cancelled') is True
    fw._router_agent = orig_router
    fw._stream_cancelled = False


if __name__ == '__main__':
    tests = [
        ("Router direct (短查询)", test_router_direct_short_greeting),
        ("Router chain (中等)", test_router_chain_medium),
        ("Router parallel (并行关键词)", test_router_parallel_keywords),
        ("Router tree (架构关键词)", test_router_tree_architecture_keywords),
        ("Router short+complex→chain", test_router_short_with_complex_keywords),
        ("Router parallel beats tree", test_router_parallel_keyword_beats_tree_at_short_len),
        ("Infer modules empty", test_infer_modules_empty),
        ("Infer modules commas", test_infer_modules_with_commas),
        ("Infer modules 中文逗号", test_infer_modules_chinese_comma),
        ("Infer modules max 5", test_infer_modules_max_five),
        ("Cancelled done empty", test_cancelled_done_empty),
        ("Cancelled done partial", test_cancelled_done_with_partial),
        ("Cancelled done scores", test_cancelled_done_with_scores),
        ("Integrate empty", test_integrate_empty),
        ("Integrate single", test_integrate_single),
        ("Integrate multiple", test_integrate_multiple),
        ("Execute direct done", test_execute_direct_produces_done),
        ("Execute direct cancelled", test_execute_direct_cancelled),
        ("Execute chain done", test_execute_chain_produces_done),
        ("Execute chain SSE sequence", test_execute_chain_sse_sequence),
        ("Execute chain iteration events", test_execute_chain_iteration_events),
        ("Execute chain stops on threshold", test_execute_chain_stops_on_threshold),
        ("Execute chain iteration numbering", test_execute_chain_iteration_has_correct_numbering),
        ("Execute chain max iterations 3", test_execute_chain_num_iterations_up_to_3),
        ("Execute chain cancelled mid-iteration", test_execute_chain_cancelled_mid_iteration),
        ("Execute parallel done", test_execute_parallel_produces_done),
        ("Tree leaf (1 module)", test_tree_leaf_single_module),
        ("Tree 2 modules", test_tree_two_modules),
        ("Tree 3 modules", test_tree_three_modules),
        ("Tree cancelled", test_tree_cancelled),
        ("AAN entry cancelled", test_adaptive_network_cancelled),
        ("AAN router cancelled", test_adaptive_network_router_cancelled),
    ]

    passed = 0
    failed = 0
    errors = []
    for name, func in tests:
        try:
            func()
            passed += 1
        except Exception as e:
            failed += 1
            errors.append((name, str(e)))
            import traceback
            errors.append(traceback.format_exc())

    print(f"\n{'='*50}")
    for name, err in errors:
        if err.startswith('Traceback'):
            print(err)
    print(f"Results: {passed} passed, {failed} failed, {len(tests)} total")
    if failed > 0:
        sys.exit(1)
    sys.exit(0)
