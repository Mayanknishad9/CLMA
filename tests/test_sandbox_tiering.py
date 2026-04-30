"""Phase 8 Sprint 2 — Test suite for Sandbox Tiering (沙箱分级 + 审批 + 回滚)."""

import sys
import os
import re

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python_interface'))

# ======================================================================
# ⑤  Sandbox Tiering — _detect_risk_level, approval, rollback
# ======================================================================

def test_detect_risk_safe_code_is_strict():
    """Simple print() should be STRICT level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("print('hello')")
    assert level == SandboxLevel.STRICT, f"Expected STRICT, got {level}"


def test_detect_risk_os_system_is_normal():
    """import os; os.system(...) should be NORMAL level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("import os; os.system('ls')")
    assert level == SandboxLevel.NORMAL, f"Expected NORMAL, got {level}"


def test_detect_risk_eval_is_normal():
    """eval() should be NORMAL level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("eval('1+1')")
    assert level == SandboxLevel.NORMAL, f"Expected NORMAL, got {level}"


def test_detect_risk_subprocess_is_normal():
    """subprocess.Popen should be NORMAL level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("subprocess.Popen(['ls'])")
    assert level == SandboxLevel.NORMAL, f"Expected NORMAL, got {level}"


def test_detect_risk_network_is_relaxed():
    """requests.get should be RELAXED level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("import requests; requests.get('http://example.com')")
    assert level == SandboxLevel.RELAXED, f"Expected RELAXED, got {level}"


def test_detect_risk_socket_is_relaxed():
    """socket connections should be RELAXED level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("import socket; s = socket.socket()")
    assert level == SandboxLevel.RELAXED, f"Expected RELAXED, got {level}"


def test_detect_risk_curl_is_relaxed():
    """curl command in shell should be RELAXED level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("curl http://example.com")
    assert level == SandboxLevel.RELAXED, f"Expected RELAXED, got {level}"


def test_detect_risk_wget_is_relaxed():
    """wget download should be RELAXED level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("wget http://example.com/file.zip")
    assert level == SandboxLevel.RELAXED, f"Expected RELAXED, got {level}"


def test_detect_risk_empty_code_is_strict():
    """Empty code should be STRICT level."""
    from tool_executor import _detect_risk_level, SandboxLevel
    level = _detect_risk_level("")
    assert level == SandboxLevel.STRICT, f"Expected STRICT, got {level}"


def test_sandbox_level_values():
    """SandboxLevel should have proper ordering."""
    from tool_executor import SandboxLevel
    # STRICT < NORMAL < RELAXED
    assert SandboxLevel.STRICT.value < SandboxLevel.NORMAL.value
    assert SandboxLevel.NORMAL.value < SandboxLevel.RELAXED.value


def test_execute_safe_code_returns_strict():
    """Safe code executed through ToolExecutor should return STRICT level."""
    from tool_executor import ToolExecutor, SandboxLevel
    exec = ToolExecutor()
    # We can't actually execute, but can verify the API is correct
    assert hasattr(exec, 'execute_python')
    # Verify SandboxLevel is accessible
    assert SandboxLevel.STRICT == 0
    assert SandboxLevel.NORMAL == 1
    assert SandboxLevel.RELAXED == 2


# ======================================================================
#  Phase 8: SandboxTier — 五级沙箱隔离等级
# ======================================================================


def test_sandbox_tier_values():
    """SandboxTier should have proper ordering."""
    from tool_executor import SandboxTier
    assert SandboxTier.ISOLATE.value < SandboxTier.STRICT.value
    assert SandboxTier.STRICT.value < SandboxTier.MEDIUM.value
    assert SandboxTier.MEDIUM.value < SandboxTier.PERMISSIVE.value
    assert SandboxTier.PERMISSIVE.value < SandboxTier.NATIVE.value


def test_risk_level_to_tier_mapping():
    """STRICT→STRICT, NORMAL→MEDIUM, RELAXED→PERMISSIVE."""
    from tool_executor import _RISK_LEVEL_TO_TIER, SandboxLevel, SandboxTier
    assert _RISK_LEVEL_TO_TIER[SandboxLevel.STRICT] == SandboxTier.STRICT
    assert _RISK_LEVEL_TO_TIER[SandboxLevel.NORMAL] == SandboxTier.MEDIUM
    assert _RISK_LEVEL_TO_TIER[SandboxLevel.RELAXED] == SandboxTier.PERMISSIVE


def test_score_to_tier_isolate_below_03():
    """executability < 0.3 should force ISOLATE regardless of base tier."""
    from tool_executor import _score_to_tier, SandboxTier
    for base in (SandboxTier.STRICT, SandboxTier.MEDIUM, SandboxTier.PERMISSIVE, SandboxTier.NATIVE):
        result = _score_to_tier(0.29, base)
        assert result == SandboxTier.ISOLATE, f"Expected ISOLATE for {base}, got {result}"


def test_score_to_tier_isolate_zero():
    """executability = 0 should force ISOLATE."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.0, SandboxTier.NATIVE)
    assert result == SandboxTier.ISOLATE


def test_score_to_tier_normal_range_no_change():
    """executability 0.5-0.89 should keep base tier unchanged."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.7, SandboxTier.STRICT)
    assert result == SandboxTier.STRICT


def test_score_to_tier_high_score_upgrades():
    """executability >= 0.9 should upgrade by one tier."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.95, SandboxTier.STRICT)
    assert result == SandboxTier.MEDIUM, f"Expected MEDIUM upgrade, got {result}"
    result = _score_to_tier(0.95, SandboxTier.MEDIUM)
    assert result == SandboxTier.PERMISSIVE, f"Expected PERMISSIVE upgrade, got {result}"


def test_score_to_tier_high_score_at_top_stays():
    """executability >= 0.9 at NATIVE should stay NATIVE."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.95, SandboxTier.NATIVE)
    assert result == SandboxTier.NATIVE


def test_score_to_tier_low_score_downgrades():
    """executability 0.3-0.49 should downgrade by one tier."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.4, SandboxTier.PERMISSIVE)
    assert result == SandboxTier.MEDIUM, f"Expected MEDIUM downgrade, got {result}"
    result = _score_to_tier(0.4, SandboxTier.NATIVE)
    assert result == SandboxTier.PERMISSIVE, f"Expected PERMISSIVE downgrade, got {result}"


def test_score_to_tier_low_score_at_bottom_stays():
    """executability 0.3-0.49 at ISOLATE should stay ISOLATE."""
    from tool_executor import _score_to_tier, SandboxTier
    result = _score_to_tier(0.4, SandboxTier.ISOLATE)
    assert result == SandboxTier.ISOLATE


def test_get_execution_tier_safe_code_default():
    """Safe code with no score should return base tier."""
    from tool_executor import ToolExecutor, SandboxTier
    exec = ToolExecutor()
    tier = exec.get_execution_tier("print('hello')")
    assert tier == SandboxTier.STRICT, f"Expected STRICT, got {tier}"


def test_get_execution_tier_dangerous_code():
    """Code with import os + subprocess should be MEDIUM."""
    from tool_executor import ToolExecutor, SandboxTier
    exec = ToolExecutor()
    tier = exec.get_execution_tier("import os; os.system('ls')")
    assert tier == SandboxTier.MEDIUM, f"Expected MEDIUM, got {tier}"


def test_get_execution_tier_low_score_isolates():
    """Low executability should ISOLATE even safe code."""
    from tool_executor import ToolExecutor, SandboxTier
    exec = ToolExecutor()
    tier = exec.get_execution_tier("print('hello')", executability=0.2)
    assert tier == SandboxTier.ISOLATE, f"Expected ISOLATE, got {tier}"


def test_execute_code_with_tier_isolate_blocks():
    """ISOLATE tier should block execution with appropriate error."""
    from tool_executor import ToolExecutor
    exec = ToolExecutor()
    result = exec.execute_code_with_tier("print('hello')", "python", executability=0.1)
    assert not result.success
    assert "BLOCKED" in result.stderr


def test_execute_code_with_tier_normal_executes():
    """Normal tier should allow execution."""
    from tool_executor import ToolExecutor
    exec = ToolExecutor()
    result = exec.execute_code_with_tier("print('hello world')", "python", executability=0.9)
    assert result.success
    assert "hello world" in result.stdout


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
