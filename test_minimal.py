"""Minimal pipeline test - single framework, simple query."""
import sys, os
sys.path.insert(0, 'python_interface')
from core import CLMAFramework

for k in ['http_proxy','https_proxy','HTTP_PROXY','HTTPS_PROXY','all_proxy','ALL_PROXY']:
    os.environ.pop(k, None)

print("Creating framework (open loop, max_iter=1)...")
fw = CLMAFramework(mode="open", threshold=0.3, max_iterations=1)
print("Processing query...")
result = fw.process_query("write a hello world python script")
print(f"Success: {result['success']}")
print(f"Content length: {len(result.get('content',''))}")
print(f"Content preview: {result.get('content','')[:200]}")
print(f"Score: {result['score']}")
print(f"Error: {result.get('error_message','none')}")
meta = result.get('metadata', {})
print(f"Iterations: {meta.get('total_iterations','N/A')}")
print(f"Best reasoned available: {'best_iteration_reasoned' in meta}")
print(f"Best verified available: {'best_iteration_verified' in meta}")
print("DONE")
