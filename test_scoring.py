#!/usr/bin/env python3
"""验证评分修复逻辑"""
import sys
sys.path.insert(0, "/root/closed-loop-multiagent/python_interface")
from core import _apply_verifier_mandatory_rules

passed = 0
failed = 0

# === Test 1: helloworld场景（syntax_valid=false, 原始分高）===
# 期望: reasonableness和satisfaction保留高分, executability=0
v = {"hard_checks": {"syntax_valid": False}, "verdict": "FAIL"}
r = {"reasonableness": 0.8, "executability": 0.85, "satisfaction": 0.8}
s = _apply_verifier_mandatory_rules(v, r)
o = round(0.4*s["reasonableness"] + 0.4*s["executability"] + 0.2*s["satisfaction"], 4)
print(f"✅ [helloworld] r={s['reasonableness']} e={s['executability']} s={s['satisfaction']} overall={o}")
if s["executability"] == 0.0 and s["reasonableness"] > 0.3 and s["satisfaction"] > 0.3:
    passed += 1
else:
    print(f"  ❌ 期望: executability=0, reasonableness>0.3, satisfaction>0.3")
    failed += 1

# === Test 2: 真正代码错误（syntax_valid=false + boundary_handled=false）===
v2 = {"hard_checks": {"syntax_valid": False, "boundary_handled": False}, "verdict": "FAIL"}
s2 = _apply_verifier_mandatory_rules(v2, r)
o2 = round(0.4*s2["reasonableness"] + 0.4*s2["executability"] + 0.2*s2["satisfaction"], 4)
print(f"✅ [real_error] r={s2['reasonableness']} e={s2['executability']} s={s2['satisfaction']} overall={o2}")
if s2["executability"] <= 0.3 and s2["reasonableness"] <= 0.3:
    passed += 1
else:
    print(f"  ❌ 期望: 全压到≤0.3")
    failed += 1

# === Test 3: 全部通过 ===
v3 = {"hard_checks": {"syntax_valid": True, "boundary_handled": True, "type_safe": True}, "verdict": "PASS"}
s3 = _apply_verifier_mandatory_rules(v3, r)
print(f"✅ [all_pass] r={s3['reasonableness']} e={s3['executability']} s={s3['satisfaction']}")
if s3["executability"] == 0.85 and s3["reasonableness"] == 0.8:
    passed += 1
else:
    print(f"  ❌ 期望: 不变")
    failed += 1

# === Test 4: 原始分低 + syntax_valid=false ===
v4 = {"hard_checks": {"syntax_valid": False}, "verdict": "FAIL"}
r4 = {"reasonableness": 0.2, "executability": 0.2, "satisfaction": 0.2}
s4 = _apply_verifier_mandatory_rules(v4, r4)
print(f"✅ [low_qual] r={s4['reasonableness']} e={s4['executability']} s={s4['satisfaction']}")
if s4["executability"] == 0.0 and s4["reasonableness"] <= 0.3:
    passed += 1
else:
    print(f"  ❌ 期望: executability=0, reasonableness≤0.3")
    failed += 1

print(f"\n{'='*40}")
print(f"通过: {passed}/{passed+failed}")
print(f"{'='*40}")
sys.exit(failed)
