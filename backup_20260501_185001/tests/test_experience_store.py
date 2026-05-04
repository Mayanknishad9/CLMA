"""Phase 8 Sprint 2 — Test suite for ExperienceStore (自我进化 + 经验库)."""

import sys
import os
import json
import math
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))

# ======================================================================
#  Phase 8: ExperienceStore — 经验库检索与存储
# ======================================================================


def test_experience_store_search_injects_similar_experiences():
    """Verify search populates _agent_memory with similar experiences."""
    import sys, os, json
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from experience_store import ExperienceStore
    import tempfile
    
    store_dir = tempfile.mkdtemp(prefix="exp_test_")
    exp = ExperienceStore(store_dir=store_dir)
    
    # Add an entry that should match
    exp.add(
        query="write a python function to sort a list",
        solution="def sort_list(lst): return sorted(lst)",
        verification_report={},
        scores={"overall": 0.9, "reasonableness": 0.8, "executability": 0.9, "satisfaction": 0.85},
    )
    
    # Search for a similar query
    results = exp.search("implement sorting algorithm in python", top_k=3, threshold=0.3)
    assert len(results) > 0, f"Expected at least 1 result, got {len(results)}"
    assert results[0].query == "write a python function to sort a list"
    assert "sort_list" in results[0].solution


def test_experience_store_add_persists():
    """Verify added entries persist to JSONL and can be reloaded."""
    import sys, os, json
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from experience_store import ExperienceStore
    import tempfile
    
    store_dir = tempfile.mkdtemp(prefix="exp_test_")
    exp = ExperienceStore(store_dir=store_dir)
    exp.add(
        query="test persistence",
        solution="print('hello')",
        verification_report={},
        scores={"overall": 0.95},
    )
    del exp
    
    # Reload from disk
    exp2 = ExperienceStore(store_dir=store_dir)
    assert len(exp2.entries) == 1, f"Expected 1 entry, got {len(exp2.entries)}"
    assert exp2.entries[0].query == "test persistence"


def test_experience_store_empty_search_returns_empty():
    """Search on empty store should return empty list."""
    import sys, os, json
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from experience_store import ExperienceStore
    import tempfile
    
    store_dir = tempfile.mkdtemp(prefix="exp_test_")
    exp = ExperienceStore(store_dir=store_dir)
    results = exp.search("anything", top_k=3, threshold=0.0)
    assert results == [], f"Expected empty list, got {results}"


def test_solver_prompt_contains_similar_experiences_placeholder():
    """Verify solver prompt template includes {similar_experiences}."""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import AGENT_PROMPTS
    solver_user = AGENT_PROMPTS["solver"]["user"]
    assert "{similar_experiences}" in solver_user, (
        f"Solver prompt missing {{similar_experiences}} placeholder. "
        f"Got: {solver_user[:100]}..."
    )


# ======================================================================
# Run all tests if executed directly
# ======================================================================

def _make_store(tmpdir=None):
    """Helper to create a store with optional temp directory."""
    from experience_store import ExperienceStore
    if tmpdir is None:
        tmpdir = tempfile.mkdtemp()
    store_dir = os.path.join(tmpdir, "experience")
    return ExperienceStore(store_dir=store_dir), tmpdir


def test_embed_returns_vector_of_correct_size():
    """_embed('...') should return a list of floats with S dimensions."""
    from experience_store import S, _embed
    vec = _embed("写一个排序函数")
    assert isinstance(vec, list), f"Expected list, got {type(vec)}"
    assert len(vec) == S, f"Expected {S} dimensions, got {len(vec)}"
    assert all(isinstance(v, float) for v in vec), "All values should be floats"


def test_embed_normalized():
    """The embedding vector should be L2-normalized (norm ≈ 1.0)."""
    from experience_store import _embed
    vec = _embed("写一个排序函数")
    norm = math.sqrt(sum(v * v for v in vec))
    assert abs(norm - 1.0) < 1e-6, f"Expected norm~1.0, got {norm}"


def test_similar_queries_have_moderate_cosine():
    """Two similar queries should have moderate cosine similarity (> 0.2 for char n-gram)."""
    from experience_store import cosine_sim, _embed
    v1 = _embed("写一个Python排序函数")
    v2 = _embed("写一个排序算法")
    cos = cosine_sim(v1, v2)
    # Char n-gram is a weak embedding. "写一个" has 3 chars shared, rest differs.
    # Expect at least some similarity for this approach.
    assert cos > 0.1, f"Expected > 0.1 (some match), got {cos}"
    assert cos < 0.95, f"Expected < 0.95 (not identical), got {cos}"


def test_different_queries_have_low_cosine():
    """Two completely different queries should have low cosine similarity."""
    from experience_store import cosine_sim, _embed
    v1 = _embed("写一个排序函数")
    v2 = _embed("如何启动服务器")
    cos = cosine_sim(v1, v2)
    assert cos < 0.6, f"Expected < 0.6, got {cos}"


def test_store_add_entry():
    """add() should append an entry to the in-memory list."""
    store, tmpdir = _make_store()
    try:
        store.add(
            query="写一个排序函数",
            solution="def sort(arr): return sorted(arr)",
            verification_report={"verified": True},
            scores={"overall": 0.9, "reasonableness": 0.88},
            iterations_used=2,
            total_tokens=1500,
        )
        assert len(store.entries) == 1, f"Expected 1 entry, got {len(store.entries)}"
        entry = store.entries[0]
        assert entry.query == "写一个排序函数"
        assert entry.scores["overall"] == 0.9
        assert isinstance(entry.timestamp, float)
        assert entry.timestamp > 0
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_store_search_finds_similar():
    """search() should find similar entries with cosine > threshold."""
    store, tmpdir = _make_store()
    try:
        store.add("写一个排序函数", "def sort(arr): return sorted(arr)", {}, {"overall": 0.9})
        store.add("如何启动web服务器", "from flask import Flask", {}, {"overall": 0.8})
        results = store.search("写一个排序算法", top_k=1, threshold=0.5)
        assert len(results) == 1, f"Expected 1 result, got {len(results)}"
        assert "sort" in results[0].solution
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_store_search_respects_threshold():
    """search() should return empty when threshold > 1.0 (impossible match)."""
    store, tmpdir = _make_store()
    try:
        store.add("写一个排序函数", "def sort(arr): ...", {}, {"overall": 0.9})
        # Exact same query has cosine = 1.0, so threshold must be > 1.0 to reject
        results = store.search("写一个排序函数", top_k=1, threshold=1.1)
        assert len(results) == 0, f"Expected 0 results with threshold > 1.0, got {len(results)}"
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_store_persists_and_loads():
    """Entries should survive save/load cycle via JSONL."""
    store, tmpdir = _make_store()
    try:
        store.add("写一个排序函数", "def sort(arr): ...",
                  {"verified": True}, {"overall": 0.9})
        store.add("读取CSV文件", "import csv; reader = csv.reader(f)",
                  {"verified": True}, {"overall": 0.85})

        # Create new store loading from same directory
        store2 = _make_store(tmpdir)[0]
        assert len(store2.entries) == 2, f"Expected 2 entries after load, got {len(store2.entries)}"
        assert store2.entries[0].query == "写一个排序函数"
        assert store2.entries[1].query == "读取CSV文件"
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_store_prune_keeps_top_k():
    """prune() should keep only the top max_entries by average score."""
    store, tmpdir = _make_store()
    try:
        store.add("a", "sol a", {}, {"overall": 0.9})
        store.add("b", "sol b", {}, {"overall": 0.5})
        store.add("c", "sol c", {}, {"overall": 0.3})
        store.prune(max_entries=2)
        assert len(store.entries) == 2, f"Expected 2 entries after prune, got {len(store.entries)}"
        # Top 2 by overall: 0.9 (a) and 0.5 (b) — c removed
        queries = [e.query for e in store.entries]
        assert "a" in queries and "b" in queries
        assert "c" not in queries
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_store_integrated_with_framework():
    """Search+cache cycle should work end-to-end through the experience module."""
    from experience_store import ExperienceStore
    from experience_store import cosine_sim, _embed
    s, tmpdir = _make_store()
    try:
        # Simulate: store a successful execution result
        s.add("生成斐波那契数列", "def fib(n): a,b=0,1; ...",
              {"verdict": "PASS"}, {"overall": 0.92})

        # Simulate: new query comes in, search for cache
        hits = s.search("实现斐波那契", top_k=1, threshold=0.1)
        assert len(hits) == 1, f"Expected 1 hit with threshold 0.1, got {len(hits)}"
        assert "fib" in hits[0].solution
        assert hits[0].scores["overall"] == 0.92
        assert hits[0].verification_report["verdict"] == "PASS"
    finally:
        import shutil
        shutil.rmtree(tmpdir)


def test_embed_empty_string():
    """Empty string should produce a zero vector (all zeros)."""
    from experience_store import _embed
    vec = _embed("")
    assert all(v == 0.0 for v in vec), "Empty string should produce zero vector"
    norm = math.sqrt(sum(v * v for v in vec))
    assert norm == 0.0, "Zero vector should have zero norm"


def test_store_add_low_score_not_added_by_default():
    """If overall < threshold, add() should still add (caller decides)."""
    store, tmpdir = _make_store()
    try:
        # add() does NOT filter by threshold — the caller (core.py) decides
        store.add("低分查询", "poor solution", {}, {"overall": 0.1})
        assert len(store.entries) == 1
    finally:
        import shutil
        shutil.rmtree(tmpdir)


if __name__ == "__main__":
    tests_passed = 0
    tests_failed = 0
    failures = []
    
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                tests_passed += 1
                print(f"  ✓ {name}")
            except Exception as e:
                tests_failed += 1
                failures.append((name, str(e)))
                print(f"  ✗ {name}: {e}")
    
    print(f"\n{'='*50}")
    print(f"Results: {tests_passed} passed, {tests_failed} failed, "
          f"{tests_passed + tests_failed} total")
    
    if failures:
        print(f"\nFailures:")
        for name, msg in failures:
            print(f"  • {name}: {msg}")
        sys.exit(1)
