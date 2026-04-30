#!/usr/bin/env python3
"""验证模拟solver生成代码块 + _auto_execute_code"""
import sys
sys.path.insert(0, "/root/closed-loop-multiagent/python_interface")
from core import CLMAFramework

fw = CLMAFramework()

# 测试模拟solver输出
result = fw._simulated_agent_call("solver", "用Python写一个hello world程序", "")
print(f"模拟solver的输出:")
print(result.content[:200])
print(f"\n包含```python: {'```python' in result.content}")
print(f"包含print(...): {'print(' in result.content}")

# 测试 _auto_execute_code 能否从该输出中找到代码
tr = fw._auto_execute_code(result.content)
if tr:
    print(f"\n✅ _auto_execute_code 成功执行!")
    print(f"  success={tr.success}, exit_code={tr.exit_code}")
    print(f"  stdout={tr.stdout.strip()}")
else:
    print(f"\n❌ _auto_execute_code 返回None")

print()
print("="*50)

# 测试 DAG done 分支的增强逻辑
# 模拟 [Solved] Executing: ... 这种纯文本（无代码块）
plain_content = "[Solved] Executing: 用Python写hello world..."
print(f"对纯文本测试增强代码提取:")
print(f"  输入: {plain_content[:60]}")

# 先测 _auto_execute_code
tr2 = fw._auto_execute_code(plain_content)
print(f"  _auto_execute_code: {'✅ 找到代码' if tr2 else '❌ 无代码块'}")

# 再测增强逻辑
import re as _d
tool_result = None
for _pat in [r'```python[\s\S]*?```', r'```[\s\S]*?```']:
    _m = _d.search(_pat, plain_content)
    if _m:
        print(f"  宽松正则在 {_pat[:30]}... 匹配到的片段: {_m.group()[:30]}")
        tool_result = fw._auto_execute_code(_m.group())
        if tool_result:
            break
if not tool_result and "[Solved]" in plain_content:
    _code_match = _d.search(r'```(\w*)\s*\n([\s\S]*?)```', plain_content)
    if _code_match:
        _lang = _code_match.group(1) or "python"
        _code = _code_match.group(2).strip()
        print(f"  直接从代码块提取: lang={_lang}, code={_code[:50]}")
        if _code:
            tool_result = fw._tool_executor.execute_code_with_tier(
                _code, language="python" if _lang in ("", "python", "py") else _lang
            )

if tool_result:
    print(f"✅ 增强提取成功! stdout={tool_result.stdout.strip()}")
else:
    print(f"  纯文本无代码块, 增强提取返回None (预期行为)")
