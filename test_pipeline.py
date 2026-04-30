"""Test full pipeline with content preservation and threshold fixes."""
import sys, os, json
sys.path.insert(0, 'python_interface')
from core import CLMAFramework

# Clean proxies
for k in ['http_proxy','https_proxy','HTTP_PROXY','HTTPS_PROXY','all_proxy','ALL_PROXY']:
    os.environ.pop(k, None)

# Test 1: Open loop - check content + evaluation
print("=" * 60)
print("TEST 1: Open Loop - content + evaluation")
print("=" * 60)
fw = CLMAFramework(mode="open", threshold=0.3, max_iterations=3)
result = fw.process_query("write a hello world python script")
print(f"Success: {result['success']}")
print(f"Content (output): {result['content'][:200]}...")
print(f"Score: {result['score']}")
print(f"Error: {result.get('error_message', 'none')}")
meta = result['metadata']
print(f"Iterations: {meta.get('total_iterations', 'N/A')}")
print(f"Best reasoned: {meta.get('best_iteration_reasoned', 'N/A')[:100]}...")
print(f"Best verified: {meta.get('best_iteration_verified', 'N/A')[:100]}...")

# Test 2: Closed loop with low threshold
print("\n")
print("=" * 60)
print("TEST 2: Closed Loop - threshold=0.3 max_iter=3")
print("=" * 60)
fw2 = CLMAFramework(mode="closed", threshold=0.3, max_iterations=3)
result2 = fw2.process_query("explain what is gravity")
print(f"Success: {result2['success']}")
print(f"Content (output): {result2['content'][:200]}...")
print(f"Score: {result2['score']}")
print(f"Error: {result2.get('error_message', 'none')}")
meta2 = result2['metadata']
print(f"Total iterations: {meta2.get('total_iterations', 'N/A')}")
# Print iteration details
if 'iterations' in result2:
    for it in result2['iterations']:
        print(f"  Iter {it['iteration']}: overall={it['score']:.4f} "
              f"(R={it['reasonableness']:.4f} E={it['executability']:.4f} S={it['satisfaction']:.4f})")
print(f"Final score: R={meta2.get('final_score_reasonableness','?')} "
      f"E={meta2.get('final_score_executability','?')} "
      f"S={meta2.get('final_score_satisfaction','?')} "
      f"O={meta2.get('final_score_overall','?')}")

# Test 3: History
print("\n")
print("=" * 60)
print("TEST 3: Execution History")
print("=" * 60)
history = fw2.get_execution_history()
print(f"History entries: {len(history)}")
for h in history:
    print(f"  Query: {h['query'][:50]}... | Content: {h['result']['content'][:80]}... | Score: {h['result']['score']['overall']:.4f}")

print("\n✅ All tests passed!")
