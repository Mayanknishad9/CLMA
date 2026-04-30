"""Complete pipeline test: content preservation + threshold + history."""
import sys, os, json
sys.path.insert(0, 'python_interface')
from core import CLMAFramework

for k in ['http_proxy','https_proxy','HTTP_PROXY','HTTPS_PROXY','all_proxy','ALL_PROXY']:
    os.environ.pop(k, None)

errors = []

# ──────────── TEST 1: Open Loop ────────────
print("=" * 60)
print("TEST 1: Open Loop — 输出 + 评估")
print("=" * 60)
fw = CLMAFramework(mode="open", threshold=0.3, max_iterations=3)
result = fw.process_query("write a hello world python script")
c = result.get('content', '')
print(f"  Success: {result['success']}")
print(f"  Output: {c[:100]}")
print(f"  Score: {result['score']['overall']:.4f}")
print(f"  Error: {result.get('error_message', '<none>')}")
meta = result.get('metadata', {})
print(f"  Iterations: {meta.get('total_iterations', '?')}")
print(f"  Best reasoned: {meta.get('best_iteration_reasoned', 'N/A')[:80]}...")
print(f"  Best verified: {meta.get('best_iteration_verified', 'N/A')[:80]}...")
if not result['success']:
    errors.append("TEST1: success=False")
if not c.strip():
    errors.append("TEST1: empty content")
print(f"  → {'✅ PASS' if not result['success'] == False and c.strip() else '❌ FAIL'}")

# ──────────── TEST 2: Closed Loop ────────────
print("\n" + "=" * 60)
print("TEST 2: Closed Loop — 多轮迭代 + 收敛")
print("=" * 60)
fw2 = CLMAFramework(mode="closed", threshold=0.3, max_iterations=3)
result2 = fw2.process_query("write a hello world python script")
c2 = result2.get('content', '')
print(f"  Success: {result2['success']}")
print(f"  Score: {result2['score']['overall']:.4f}")
print(f"  Error: {result2.get('error_message', '<none>')}")
meta2 = result2.get('metadata', {})
iters = meta2.get('total_iterations', '?')
print(f"  Total iterations: {iters}")
if not result2['success']:
    errors.append("TEST2: success=False")
if iters == '?' or int(iters) > 3:
    errors.append(f"TEST2: too many iterations ({iters})")
print(f"  → {'✅ PASS' if result2['success'] else '❌ FAIL'}")

# ──────────── TEST 3: Threshold低+快速收敛 ────────────
print("\n" + "=" * 60)
print("TEST 3: 低阈值快速收敛")
print("=" * 60)
fw3 = CLMAFramework(mode="closed", threshold=0.2, max_iterations=2)
result3 = fw3.process_query("explain what is gravity")
c3 = result3.get('content', '')
print(f"  Success: {result3['success']}")
print(f"  Content: {c3[:120]}")
print(f"  Score: {result3['score']['overall']:.4f}")
meta3 = result3.get('metadata', {})
print(f"  Iterations: {meta3.get('total_iterations', '?')}")
if not result3['success']:
    errors.append("TEST3: success=False")
print(f"  → {'✅ PASS' if result3['success'] else '❌ FAIL'}")

# ──────────── TEST 4: 执行历史 ────────────
print("\n" + "=" * 60)
print("TEST 4: Execution History")
print("=" * 60)
history = fw2.get_execution_history()
print(f"  Entries: {len(history)}")
for h in history:
    print(f"    [{h['query'][:40]}...] → score={h['result']['score']['overall']:.3f}, content={h['result']['content'][:60]}...")
if not history:
    errors.append("TEST4: empty history")
print(f"  → {'✅ PASS' if history else '❌ FAIL'}")

# ──────────── TEST 5: API端点 ────────────
print("\n" + "=" * 60)
print("TEST 5: API 端点 (Flask)")
print("=" * 60)
import urllib.request
try:
    r = urllib.request.urlopen("http://localhost:5000/api/status", timeout=3)
    status = json.loads(r.read())
    print(f"  /api/status: ready={status.get('status','?')}, api={status.get('api_configured','?')}")
except Exception as e:
    errors.append(f"TEST5: {e}")
    print(f"  /api/status: ❌ {e}")
print(f"  → {'✅ PASS' if not errors or not any('TEST5' in e for e in errors) else '❌ FAIL'}")

# ──────────── RESULTS ────────────
print("\n" + "=" * 60)
if errors:
    print(f"❌ FAILED: {len(errors)} error(s)")
    for e in errors:
        print(f"  - {e}")
    sys.exit(1)
else:
    print("✅ ALL TESTS PASSED!")
