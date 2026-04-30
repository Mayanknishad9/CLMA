"""Test agent pipeline with tool execution integration."""
import sys, os
sys.path.insert(0, 'python_interface')
from core import CLMAFramework

for k in ['http_proxy','https_proxy','HTTP_PROXY','HTTPS_PROXY','all_proxy','ALL_PROXY']:
    os.environ.pop(k, None)

print("=" * 60)
print("TEST: Agent Pipeline with Tool Execution")
print("=" * 60)

fw = CLMAFramework(threshold=0.3, mode="closed", max_iterations=3)

result = fw.process_query("write a python script that prints the first 10 Fibonacci numbers")

print(f"\nSuccess:      {result['success']}")
print(f"Score:        {result['score']['overall']:.3f}")
print(f"Threshold:    0.3")
print(f"Tools used:   {result.get('tools_used', False)}")
content = result.get('content', '')
print(f"Final output: {content[:200]}")
print(f"Tool results: {len(result.get('tool_results', []))}")

# Show tool execution details
for i, tr in enumerate(result.get('tool_results', [])):
    print(f"\n  Tool {i+1}: {tr['tool_name']} (exit={tr['exit_code']}, {tr['duration_ms']:.0f}ms)")
    print(f"  Stdout preview: {tr['stdout'][:150]}")

print("\n" + "=" * 60)
status = "PASS" if result['success'] else 'FAIL'
score_ok = result['score']['overall'] >= 0.3
print(f"{status}: Score {result['score']['overall']:.3f} meets threshold 0.3 = {score_ok}")
print("=" * 60)
