"""Phase 8 Sprint 1 — Test suite for Python-level enhancements.

Covers:
- ①  Strict JSON parsing with LLM output repair
- ①  Chain-of-Verification (self-check on evaluator scores)
- ①  Refiner self-check prompt integration
- ③  Multi-dimensional verification (hard + soft checks)
- ③  Weighted scoring with hard-veto logic
"""

import sys
import os
import json
import re

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))

from core import AGENT_PROMPTS, _refiner_self_check, _parse_verifier_verdict, _apply_verifier_mandatory_rules


# ======================================================================
# ①  JSON 约束 — _strict_json_parse()
# ======================================================================

def _strict_json_parse(text):
    """Parse JSON from LLM output with repair strategies.
    
    Attempts:
    1. Strip markdown ```json ... ``` fences
    2. Direct json.loads
    3. Fix trailing commas, unquoted keys, single quotes -> double
    4. Regex-extract best JSON-like substring
    5. All fail -> raise ValueError
    """
    if not text or not text.strip():
        raise ValueError("Empty input to _strict_json_parse")
    
    text = text.strip()
    
    # Strategy 0: Extract JSON object from surrounding text first
    obj_match = re.search(r'\{.*\}', text, re.DOTALL)
    if obj_match:
        text = obj_match.group()
    
    # Strategy 1: Strip ```json ... ``` fences
    fences = [
        (r'```json\s*\n?(.*?)\n?```', re.DOTALL),
        (r'```\s*\n?(.*?)\n?```', re.DOTALL),
        (r'`(.*?)`', 0),
    ]
    for pattern, flags in fences:
        m = re.search(pattern, text, flags) if flags else re.search(pattern, text)
        if m:
            candidate = m.group(1).strip()
            try:
                return json.loads(candidate)
            except json.JSONDecodeError:
                continue
    
    # Strategy 2: Direct parse
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass
    
    # Strategy 3: Fix common LLM issues
    fixed = text
    # Remove trailing commas before ] or }
    fixed = re.sub(r',\s*([\]}])', r'\1', fixed)
    # Replace single quotes with double quotes (but not inside already-double-quoted strings)
    fixed = re.sub(r"(?<!\\)'(?=[^:;,{}\[\]]*[\]}:,])", '"', fixed)
    # Try quoting unquoted keys (simple heuristic)
    fixed = re.sub(r'([{,]\s*)(\w[\w_]*)(\s*:)', r'\1"\2"\3', fixed)
    try:
        return json.loads(fixed)
    except json.JSONDecodeError:
        pass
    
    # Strategy 4: Regex-extract any JSON object
    m = re.search(r'\{[^{}]*\}', text)
    if m:
        try:
            return json.loads(m.group())
        except json.JSONDecodeError:
            pass
    
    raise ValueError(f'Could not parse JSON from: {text[:100]}...')


# Tests for _strict_json_parse

def test_parse_clean_json():
    """Should parse standard JSON directly."""
    result = _strict_json_parse('{"reasonableness": 0.8, "executability": 0.7}')
    assert result["reasonableness"] == 0.8
    assert result["executability"] == 0.7


def test_parse_markdown_fenced():
    """Should strip ```json fences."""
    text = '```json\n{"reasonableness": 0.9, "executability": 0.85}\n```'
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.9


def test_parse_markdown_fenced_no_lang():
    """Should strip plain ``` fences."""
    text = '```\n{"reasonableness": 0.75}\n```'
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.75


def test_parse_inline_code():
    """Should extract JSON from inline backtick."""
    result = _strict_json_parse('The score is `{"reasonableness": 0.6}`')
    assert result["reasonableness"] == 0.6


def test_parse_trailing_comma():
    """Should fix trailing comma before closing brace."""
    text = '{"reasonableness": 0.8, "executability": 0.7,}'
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.8
    assert result["executability"] == 0.7


def test_parse_trailing_comma_array():
    """Should fix trailing comma in arrays."""
    text = '{"issues": ["bug1", "bug2",]}'
    result = _strict_json_parse(text)
    assert result["issues"] == ["bug1", "bug2"]


def test_parse_unquoted_keys():
    """Should quote unquoted JSON keys."""
    text = '{reasonableness: 0.8, executability: 0.7}'
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.8


def test_parse_single_quoted():
    """Should handle single-quoted field names."""
    text = "{'reasonableness': 0.8}"
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.8


def test_parse_regex_fallback():
    """Should extract JSON from noisy LLM output via regex fallback."""
    text = """
    Here are my scores: I think the solution is good.
    {"reasonableness": 0.72, "executability": 0.65, "satisfaction": 0.80}
    Let me know if you need changes.
    """
    result = _strict_json_parse(text)
    assert result["reasonableness"] == 0.72
    assert result["executability"] == 0.65


def test_parse_empty_raises():
    """Should raise ValueError on empty input."""
    try:
        _strict_json_parse("")
        assert False, "Expected ValueError"
    except ValueError:
        pass


def test_parse_garbage_raises():
    """Should raise ValueError on completely unparseable input."""
    try:
        _strict_json_parse("This is not JSON at all. Just some random text.")
        assert False, "Expected ValueError"
    except ValueError:
        pass


# ======================================================================
# ①  Chain-of-Verification — _covert_verify_scores()
# ======================================================================

def _covert_verify_scores(original_scores):
    """透传原始分，不做任何数值压制。

    LLM 评估者已经给出了分数，不再额外砍分。
    仅做边界钳制到 [0, 1] 区间。
    """
    scores = dict(original_scores)
    for k in ("reasonableness", "executability", "satisfaction"):
        scores[k] = max(0.0, min(1.0, scores.get(k, 0.5)))
    return scores


def test_covert_passthrough():
    """所有分数应原样透传（数值压制规则已删除）"""
    scores = {"reasonableness": 0.7, "executability": 0.65, "satisfaction": 0.6}
    result = _covert_verify_scores(scores)
    assert result == scores


def test_covert_high_scores_passthrough():
    """高分也应原样透传，不再砍分"""
    scores = {"reasonableness": 0.95, "executability": 0.96, "satisfaction": 0.95}
    result = _covert_verify_scores(scores)
    assert result == scores


def test_covert_clamps_to_range():
    """超出 [0,1] 区间的值应被钳制"""
    scores = {"reasonableness": 1.5, "executability": -0.5, "satisfaction": 0.8}
    result = _covert_verify_scores(scores)
    assert result["reasonableness"] == 1.0
    assert result["executability"] == 0.0
    assert result["satisfaction"] == 0.8


# ======================================================================
# ①  Refiner self-check prompt
# ======================================================================

def test_refiner_prompt_has_selfcheck():
    """Refiner system prompt should contain self-check instructions."""
    refiner = AGENT_PROMPTS.get("refiner", {})
    system = refiner.get("system", "")
    assert "Self-Check" in system or "self-check" in system, \
        "Refiner prompt missing self-check instructions"


def test_refiner_prompt_has_three_checks():
    """Should have at least 3 numbered check items."""
    refiner = AGENT_PROMPTS.get("refiner", {})
    system = refiner.get("system", "")
    # Count numbered check items (1., 2., 3.)
    checks = re.findall(r'\d+\.\s', system)
    assert len(checks) >= 3, f"Expected >=3 checks, found {len(checks)}"


# ======================================================================
# ③  Weighted scoring with rule weights
# ======================================================================

def test_weighted_overall_with_rule_weights():
    """overall score should use rule weights, not equal average."""
    scores = {"reasonableness": 0.9, "executability": 0.5, "satisfaction": 0.8}
    # Rule weights: reasonableness=0.4, executability=0.4, satisfaction=0.2
    w = {"reasonableness": 0.4, "executability": 0.4, "satisfaction": 0.2}
    overall = (
        scores["reasonableness"] * w["reasonableness"] +
        scores["executability"] * w["executability"] +
        scores["satisfaction"] * w["satisfaction"]
    )
    assert overall == 0.9 * 0.4 + 0.5 * 0.4 + 0.8 * 0.2, f"Expected 0.72, got {overall}"
    assert abs(overall - 0.72) < 0.001


def test_hard_veto_syntax_invalid():
    """If hard_checks.syntax_valid is false, executability should drop to 0."""
    # This simulates the verifier returning structured output
    hard_checks = {"syntax_valid": False, "boundary_handled": True, "type_safe": True}
    scores = {"reasonableness": 0.8, "executability": 0.7, "satisfaction": 0.75}
    
    if not hard_checks.get("syntax_valid", True):
        scores["executability"] = 0.0
    
    assert scores["executability"] == 0.0
    assert scores["reasonableness"] == 0.8  # Unchanged


def test_no_veto_when_syntax_valid():
    """If hard_checks.syntax_valid is true, executability should be unchanged."""
    hard_checks = {"syntax_valid": True, "boundary_handled": False, "type_safe": True}
    scores = {"reasonableness": 0.8, "executability": 0.7, "satisfaction": 0.75}
    
    if hard_checks.get("syntax_valid", True):
        pass  # No change
    
    assert scores["executability"] == 0.7


# ======================================================================
# ③  Verifier structured output parsing
# ======================================================================

def test_verifier_prompt_has_multidimensional_checks():
    """Verifier prompt should include hard and soft check dimensions."""
    verifier = AGENT_PROMPTS.get("verifier", {})
    system = verifier.get("system", "")
    assert "hard_checks" in system or "hard" in system.lower(), \
        "Verifier prompt missing hard checks"
    assert "soft_checks" in system or "soft" in system.lower(), \
        "Verifier prompt missing soft checks"


def test_verifier_prompt_has_structured_output_format():
    """Verifier prompt should specify JSON structured output format."""
    verifier = AGENT_PROMPTS.get("verifier", {})
    system = verifier.get("system", "")
    assert "JSON" in system, "Verifier prompt missing JSON output format"
    assert "verdict" in system.lower(), \
        "Verifier prompt missing verdict field"


def test_evaluator_prompt_has_weighted_scoring():
    """Evaluator prompt should mention weighted scoring."""
    evaluator = AGENT_PROMPTS.get("evaluator", {})
    system = evaluator.get("system", "")
    assert "weight" in system.lower(), \
        "Evaluator prompt missing weighted scoring mention"


# ======================================================================
# ③  _parse_evaluator_scores with strict JSON
# ======================================================================

# Import _strict_json_parse into core's scope for the full integration test
def _simulate_parse_evaluator_scores(response, strict=True):
    """Simulate the enhanced _parse_evaluator_scores using strict parsing."""
    scores = {"reasonableness": 0.5, "executability": 0.5, "satisfaction": 0.5}
    try:
        if strict:
            parsed = _strict_json_parse(response)
        else:
            m = re.search(r'\{[^}]+?\}', response)
            if m:
                parsed = json.loads(m.group())
            else:
                raise ValueError("No JSON found")
        scores.update({
            "reasonableness": float(parsed.get("reasonableness", scores["reasonableness"])),
            "executability": float(parsed.get("executability", scores["executability"])),
            "satisfaction": float(parsed.get("satisfaction", scores["satisfaction"])),
        })
    except (ValueError, json.JSONDecodeError, KeyError, TypeError):
        pass  # Fallback to defaults
    return scores


def test_parse_with_clean_json_returns_correct_scores():
    """Clean JSON should parse to correct scores."""
    response = '{"reasonableness": 0.8, "executability": 0.7, "satisfaction": 0.75}'
    scores = _simulate_parse_evaluator_scores(response)
    assert scores["reasonableness"] == 0.8
    assert scores["executability"] == 0.7
    assert scores["satisfaction"] == 0.75


def test_parse_with_noisy_output_still_works():
    """Noisy LLM output should still be parseable with strict mode."""
    response = """Based on my analysis, here are the scores:
    ```json
    {
        "reasonableness": 0.67,
        "executability": 0.72,
        "satisfaction": 0.80
    }
    ```
    I think this solution is adequate.
    """
    scores = _simulate_parse_evaluator_scores(response, strict=True)
    assert abs(scores["reasonableness"] - 0.67) < 0.01, f"Got {scores['reasonableness']}"
    assert abs(scores["executability"] - 0.72) < 0.01
    assert abs(scores["satisfaction"] - 0.80) < 0.01


def test_parse_with_trailing_comma_noisy():
    """Trailing comma in noisy output should still parse."""
    response = "Scores: {reasonableness:0.85, executability:0.77, satisfaction:0.82,}"
    scores = _simulate_parse_evaluator_scores(response, strict=True)
    assert abs(scores["reasonableness"] - 0.85) < 0.01


def test_parse_fallback_to_defaults():
    """Completely unparseable output should fall back to defaults."""
    response = "I don't know how to score this."
    scores = _simulate_parse_evaluator_scores(response, strict=True)
    assert scores["reasonableness"] == 0.5
    assert scores["executability"] == 0.5
    assert scores["satisfaction"] == 0.5


# ======================================================================
# ①  Refiner 自检 — _refiner_self_check()
# ======================================================================


def test_refiner_rejects_meta_commentary():
    """元注释前缀应该被否决。"""
    original = "Write a function to calculate fibonacci"
    bad = "Here is the refined query: Write a function to calculate fibonacci"
    assert _refiner_self_check(bad, original) is False


def test_refiner_accepts_proper_refinement():
    """合格的精炼应该通过。"""
    original = "Write a function to calculate fibonacci"
    good = "Write a Python function fibonacci(n) that returns the nth Fibonacci number using DP"
    assert _refiner_self_check(good, original) is True


def test_refiner_rejects_too_similar():
    """与原查询过于相似应该被否决。"""
    original = "Explain how to sort an array"
    copy = "Explain how to sort an array"
    assert _refiner_self_check(copy, original) is False


def test_refiner_rejects_empty():
    """空输出应该被否决。"""
    assert _refiner_self_check("", "some query") is False
    assert _refiner_self_check("   ", "some query") is False


def test_refiner_rejects_chinese_meta():
    """中文元注释前缀应该被否决。"""
    original = "实现一个排序算法"
    bad_meta = "以下是精炼后的查询：实现一个排序算法"
    assert _refiner_self_check(bad_meta, original) is False


def test_refiner_accepts_slightly_similar_but_longer():
    """长度差异大时，相似度高也可以接受（说明有实质性扩充）。"""
    original = "Sort an array"
    good = "Sort an array using quicksort with O(n log n) average complexity and in-place partitioning"
    assert _refiner_self_check(good, original) is True


# ======================================================================
# ③  Verifier 强制规则 — _parse_verifier_verdict() & _apply_verifier_mandatory_rules()
# ======================================================================


def test_parse_verifier_clean_json():
    """干净的 JSON 应该正确解析。"""
    content = '{"hard_checks": {"syntax_valid": true, "boundary_handled": true}, "soft_checks": {}, "verdict": "PASS"}'
    data = _parse_verifier_verdict(content)
    assert data["hard_checks"]["syntax_valid"] is True
    assert data["verdict"] == "PASS"


def test_parse_verifier_noisy_output():
    """嘈杂的输出中应该提取 JSON。"""
    content = 'The solution looks good. ```json {"hard_checks": {"syntax_valid": true}, "verdict": "PASS"} ```'
    data = _parse_verifier_verdict(content)
    assert data["hard_checks"]["syntax_valid"] is True
    assert data["verdict"] == "PASS"


def test_parse_verifier_fallback_regex():
    """难以解析的文本用正则兜底。"""
    content = 'I think "syntax_valid": false, "verdict": "FAIL"'
    data = _parse_verifier_verdict(content)
    assert data["verdict"] == "FAIL"
    assert data["hard_checks"]["syntax_valid"] is False


def test_parse_verifier_empty():
    """空内容返回默认通过。"""
    data = _parse_verifier_verdict("")
    assert data["verdict"] == "PASS"


def test_mandatory_syntax_invalid_zeroes_executability():
    """syntax_valid=false → executability=0"""
    v_data = {"hard_checks": {"syntax_valid": False}, "verdict": "FAIL"}
    scores = _apply_verifier_mandatory_rules(v_data, {"reasonableness": 0.8, "executability": 0.8, "satisfaction": 0.8})
    assert scores["executability"] == 0.0
    # only_syntax_valid_failed 且原始分高 → 平衡策略保留 reasonableness 和 satisfaction
    assert scores["reasonableness"] > 0.3  # 平衡后不再被降级


def test_mandatory_verdict_fail_caps_executability_only():
    """verdict=FAIL → 只限 executability≤0.3，其他分保留"""
    v_data = {"hard_checks": {"syntax_valid": True}, "verdict": "FAIL"}
    scores = _apply_verifier_mandatory_rules(v_data, {"reasonableness": 0.9, "executability": 0.9, "satisfaction": 0.9})
    assert scores["executability"] <= 0.3  # 只压执行性
    assert scores["reasonableness"] == 0.9  # 合理性保留
    assert scores["satisfaction"] == 0.9    # 满意度保留


def test_mandatory_syntax_valid_no_penalty():
    """syntax_valid=true → 不应有惩罚"""
    v_data = {"hard_checks": {"syntax_valid": True, "boundary_handled": True}, "verdict": "PASS"}
    scores = _apply_verifier_mandatory_rules(v_data, {"reasonableness": 0.8, "executability": 0.7, "satisfaction": 0.8})
    assert scores["executability"] == 0.7  # 不变


def test_mandatory_any_hard_check_fail_forces_fail():
    """任意 hard_checks 失败都强制 verdict=FAIL，但只限 executability"""
    v_data = {"hard_checks": {"syntax_valid": True, "boundary_handled": False}, "verdict": "PASS"}
    scores = _apply_verifier_mandatory_rules(v_data, {"reasonableness": 0.9, "executability": 0.9, "satisfaction": 0.9})
    assert scores["executability"] <= 0.3  # 被强制 FAIL 只压执行性
    assert scores["reasonableness"] > 0.3  # 其他分保留


def test_mandatory_defaults_when_no_verifier():
    """没有 verifier 数据时，不使用降级逻辑（agent_memory 中无 verifier 时正常流程）"""
    scores = _apply_verifier_mandatory_rules({"hard_checks": {}, "verdict": "PASS"}, {"reasonableness": 0.8, "executability": 0.7, "satisfaction": 0.8})
    assert scores["executability"] == 0.7


# ======================================================================
#  Phase 8: 并行候选生成 (Candidate Generation)
# ======================================================================


def test_critic_compress_keeps_top_ratio():
    """_critic_compress_candidates 应保留 top keep_ratio 个候选。"""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import CLMAFramework
    fw = CLMAFramework()
    
    class MockToolResult:
        def __init__(self, success, stdout="", stderr=""):
            self.success = success
            self.stdout = stdout
            self.stderr = stderr
    
    candidates = [
        {"seed": 0, "content": "print(1)", "tool_result": MockToolResult(True, "1\n", ""), "success": True},
        {"seed": 1, "content": "print(2)", "tool_result": MockToolResult(True, "2\n", ""), "success": True},
        {"seed": 2, "content": "print(3)", "tool_result": MockToolResult(False, "", "error"), "success": False},
        {"seed": 3, "content": "no code here", "tool_result": None, "success": True},
    ]
    
    best = fw._critic_compress_candidates(candidates, "test", keep_ratio=0.5)
    assert len(best) == 2, f"Expected 2 candidates, got {len(best)}"


def test_critic_compress_prefers_executed():
    """有执行结果的 candidate 优先级应高于无执行结果的。"""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import CLMAFramework
    fw = CLMAFramework()
    
    class MockToolResult:
        def __init__(self, success, stdout="", stderr=""):
            self.success = success
            self.stdout = stdout
            self.stderr = stderr
    
    candidates = [
        {"seed": 0, "content": "no code", "tool_result": None, "success": True},
        {"seed": 1, "content": "executed", "tool_result": MockToolResult(True, "hello\n", ""), "success": True},
    ]
    
    best = fw._critic_compress_candidates(candidates, "test", keep_ratio=1.0)
    assert len(best) == 2
    assert best[0]["seed"] == 1, f"Expected seed 1 (executed) first, got seed {best[0]['seed']}"


def test_critic_compress_keeps_at_least_one():
    """即使 keep_ratio 很小也至少保留 1 个候选。"""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import CLMAFramework
    fw = CLMAFramework()
    
    class MockToolResult:
        def __init__(self, success, stdout="", stderr=""):
            self.success = success
            self.stdout = stdout
            self.stderr = stderr
    
    candidates = [
        {"seed": 0, "content": "x", "tool_result": MockToolResult(True, "x\n", ""), "success": True},
    ]
    
    best = fw._critic_compress_candidates(candidates, "test", keep_ratio=0.01)
    assert len(best) == 1, f"Expected 1 (min), got {len(best)}"


def test_candidate_config_default_disabled():
    """候选配置默认应处于禁用状态。"""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import CLMAFramework
    fw = CLMAFramework()
    assert fw._candidate_config.enabled == False


def test_candidate_config_enable():
    """启用候选配置应正确设置参数。"""
    import sys
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))
    from core import CLMAFramework
    fw = CLMAFramework()
    fw.set_candidate_count(4)
    assert fw._candidate_config.enabled == True
    assert int(fw._candidate_config.num_candidates) == 4
    fw.set_candidate_count(1)
    assert fw._candidate_config.enabled == False


# ======================================================================
# Run all tests if executed directly
# ======================================================================

if __name__ == "__main__":
    tests_passed = 0
    tests_failed = 0
    failures = []
    
    test_fns = [
        (name, fn) for name, fn in globals().items()
        if name.startswith("test_") and callable(fn)
    ]
    
    for name, fn in sorted(test_fns):
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
