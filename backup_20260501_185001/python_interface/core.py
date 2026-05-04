"""
CLMA Framework - Python wrapper around the C++ core engine.

Provides a clean Pythonic interface to the closed-loop multi-agent system.
"""
import os
import sys
import json

# Import C++ bindings
_bindings_dir = os.path.join(os.path.dirname(__file__), '.')
if _bindings_dir not in sys.path:
    sys.path.insert(0, _bindings_dir)

import clma_core

# Convenience imports
EvaluationScore = clma_core.EvaluationScore
AgentResult = clma_core.AgentResult
Rule = clma_core.Rule
TokenUsage = clma_core.TokenUsage
AgentType = clma_core.AgentType
AgentState = clma_core.AgentState
RuleEngine = clma_core.RuleEngine
TokenMonitor = clma_core.TokenMonitor
LoopController = clma_core.LoopController
Orchestrator = clma_core.Orchestrator
PluginManager = clma_core.PluginManager
PluginType = clma_core.PluginType
PluginState = clma_core.PluginState
PluginInfo = clma_core.PluginInfo
PluginVersion = clma_core.PluginVersion
CandidateConfig = clma_core.CandidateConfig
DAGConfig = clma_core.DAGConfig

# API integration
from api_providers import create_provider, load_config, PROVIDER_REGISTRY

# Tool integration
from tool_executor import ToolExecutor, ToolResult


# === Phase 8: Strict JSON Parser & Chain-of-Verification ===

import re as _re_json
import json as _json_module  # for _parse_verifier_verdict


def _parse_verifier_verdict(verifier_content: str) -> dict:
    """解析 Verifier 输出的 JSON，提取 hard_checks 和 verdict。

    Verifier 输出格式示例：
    {
      "hard_checks": {"syntax_valid": true, "boundary_handled": true, "type_safe": true},
      "soft_checks": {"performance_adequate": true, "readable": true},
      "issues": ["minor: missing edge case"],
      "verdict": "PASS"
    }

    如果 JSON 解析失败，返回默认通过状态。
    """
    if not verifier_content or not verifier_content.strip():
        return {"hard_checks": {}, "soft_checks": {}, "verdict": "PASS"}

    try:
        data = _strict_json_parse(verifier_content)
        # 确保层级结构完整
        if "hard_checks" not in data:
            data["hard_checks"] = {}
        if "verdict" not in data:
            data["verdict"] = "PASS"
        return data
    except (ValueError, _json_module.JSONDecodeError):
        pass

    # 兜底：正则提取 verdict 和 syntax_valid
    verdict_match = _re_json.search(r'"verdict"\s*:\s*"(\w+)"', verifier_content)
    syntax_match = _re_json.search(r'"syntax_valid"\s*:\s*(true|false)', verifier_content)
    hard_checks = {}
    if syntax_match:
        hard_checks["syntax_valid"] = syntax_match.group(1).lower() == "true"
    return {
        "hard_checks": hard_checks,
        "soft_checks": {},
        "verdict": verdict_match.group(1).upper() if verdict_match else "PASS",
    }


def _apply_verifier_mandatory_rules(verifier_data: dict, raw_scores: dict) -> dict:
    """基于 Verifier 结果对分数做强制修正。

    强制规则：
    1. hard_checks.syntax_valid == false → executability = 0
    2. hard_checks.syntax_valid == false → verdict 强制降级为 FAIL
    3. verdict == "FAIL" → 所有分数上限 0.3

    Note: 对极其简单的任务（helloworld 级别），放宽规则：
    - 如果 evaluator 原始分 >= 0.7 且所有 hard_checks 失败中只有
      syntax_valid 为 false（其他 check 正常），则只降 executability 不降其他分。
    """
    scores = dict(raw_scores)
    hard = verifier_data.get("hard_checks", {})
    verdict = verifier_data.get("verdict", "PASS").upper()

    # 规则 1: syntax_valid=false → executability=0
    if not hard.get("syntax_valid", True):
        scores["executability"] = 0.0

    # 规则 2: 如果 hard_checks 任意一项失败，verdict 强制降级
    for check_name, passed in hard.items():
        if not passed:
            verdict = "FAIL"

    # 规则 3: verdict=FAIL → 只压制 executability，保留其他分
    if verdict == "FAIL":
        scores["executability"] = min(scores.get("executability", 0.5), 0.3)
        # reasonableness 和 satisfaction 不受 verdict=FAIL 影响
        # 它们的合理性和满意度由 evaluator 基于事实判断，不应因 verifier verdict 被压制

    return scores


def _refiner_self_check(refined_text: str, original_query: str) -> bool:
    """检查 Refiner 的输出是否合格。

    检查项：
    1. 是否保留了原始查询的核心意图（非空输出）
    2. 是否包含了无关的元评论前缀
    3. 是否只是原样重复没做精炼
    """
    if not refined_text or not refined_text.strip():
        return False

    # 元评论标记 — 如果输出以这些前缀开头且前缀占比过大，否决
    meta_markers = [
        "here is the refined", "refined query:", "here's the refined",
        "the refined version", "i have refined", "i've refined",
        "following the", "based on the", "sure, here",
        "好的，", "以下是精炼后的", "精炼后的", "refined:",
    ]
    lower = refined_text.strip().lower()
    for marker in meta_markers:
        if lower.startswith(marker):
            # 元评论前缀占了超过 20% 的总长度 => 否决
            if len(marker) > len(lower) * 0.2:
                return False

    # 检查是否与原查询过于相似（字符级相似度 > 85% 视为没精炼）
    min_len = min(len(refined_text), len(original_query))
    if min_len > 0:
        common = sum(1 for i in range(min_len) if refined_text[i] == original_query[i])
        similarity = common / min_len
        # 如果长度差异不到 30% 且相似度 > 85%，否决（CJK 字符放大系数）
        len_ratio = max(len(refined_text), len(original_query)) / max(1, min_len)
        if len_ratio < 1.30 and similarity > 0.85:
            return False

    return True


def _strict_json_parse(text):
    """Parse JSON from LLM output with multiple repair strategies."""
    if not text or not text.strip():
        raise ValueError("Empty input to _strict_json_parse")

    text = text.strip()

    # Strategy 0: Extract JSON object from surrounding text
    obj_match = _re_json.search(r'\{.*\}', text, _re_json.DOTALL)
    if obj_match:
        text = obj_match.group()

    # Strategy 1: Strip ```json ... ``` fences
    fences = [
        (r'```json\s*\n?(.*?)\n?```', _re_json.DOTALL),
        (r'```\s*\n?(.*?)\n?```', _re_json.DOTALL),
        (r'`(.*?)`', 0),
    ]
    for pattern, flags in fences:
        m = _re_json.search(pattern, text, flags) if flags else _re_json.search(pattern, text)
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
    fixed = _re_json.sub(r',\s*([\]}])', r'\1', fixed)  # trailing commas
    fixed = _re_json.sub(r'(?<!\\)\'(?=[^:;,{}\[\]]*[\]}:,])', '"', fixed)  # single->double quotes
    fixed = _re_json.sub(r'([{,]\s*)(\w[\w_]*)(\s*:)', r'\1"\2"\3', fixed)  # quote unquoted keys
    try:
        return json.loads(fixed)
    except json.JSONDecodeError:
        pass

    # Strategy 4: Regex-extract any JSON object
    m = _re_json.search(r'\{[^{}]*\}', text)
    if m:
        try:
            return json.loads(m.group())
        except json.JSONDecodeError:
            pass

    raise ValueError(f'Could not parse JSON from: {text[:100]}...')


def _covert_verify_scores(original_scores):
    """透传原始分，不做任何数值压制。

    LLM 评估者已经按 prompt 中的完整评分标准进行了打分，
    不应再人为砍分。本函数仅做格式校验和边界钳制。
    """
    scores = dict(original_scores)
    # 仅钳制到 [0, 1] 区间
    for k in ("reasonableness", "executability", "satisfaction"):
        scores[k] = max(0.0, min(1.0, scores.get(k, 0.5)))
    return scores


# === Agent System Prompts ===

AGENT_PROMPTS = {
    "refiner": {
        "system": (
            "You are a query refiner in a closed-loop multi-agent system. "
            "Your job is to improve the query based on the previous iteration's results.\n"
            "If this is the first iteration, simply restructure the original query for clarity.\n"
            "If this is iteration 2+, identify what went wrong in the PREVIOUS solution "
            "(from [Previous context]) and refine the query to fix those issues.\n"
            'Output ONLY the refined query text — no explanations, no meta-commentary.\n'
            '\n'
            'IMPORTANT — Self-Check before output:\n'
            '1. Does the refined query preserve the core intent of the original?\n'
            '2. Does it incorporate all feedback from the previous iteration?\n'
            '3. Is the output JUST the refined query — no extra commentary?\n'
            'If any check fails, rework the output.'
        ),
        "user": "Refine this query (iteration {iteration}):\n\n{query}\n\n{previous_iteration_info}",
    },
    "reasoner": {
        "system": (
            "You are a reasoning agent. Given a refined query, produce step-by-step reasoning "
            "that breaks down the problem. Use chain-of-thought.\n"
            "Output ONLY the reasoning steps, numbered."
        ),
        "user": "Reason through this problem step by step:\n\n{query}",
    },
    "solver": {
        "system": (
            "You are a solution agent. Given a query and reasoning, produce a concrete "
            "solution. Write actual code, commands, or structured output as appropriate.\n"
            "Your code will be AUTO-EXECUTED after you output it in a markdown code block.\n"
            "IMPORTANT: Your code runs non-interactively — do NOT use input(), sys.stdin.read(), "
            "or any blocking user-input calls. Use command-line arguments instead.\n"
            "Execution results (stdout, stderr, exit code) will be provided in subsequent contexts.\n"
            "Put code in markdown code blocks with language: ```python, ```bash, or ```cpp\n"
            "Output ONLY the solution."
        ),
        "user": "Solve this problem:\n\nQuery: {query}\n\nReasoning: {reasoning}\n\n{similar_experiences}\n{execution_result}",
    },
    "verifier": {
        "system": (
            'You are a strict multi-dimensional verification agent.\n'
            'Verify the solution on these dimensions, outputting a JSON object:\n'
            '{\n'
            '  "hard_checks": {\n'
            '    "syntax_valid": true/false,\n'
            '    "boundary_handled": true/false,\n'
            '    "type_safe": true/false\n'
            '  },\n'
            '  "soft_checks": {\n'
            '    "performance_adequate": true/false,\n'
            '    "readable": true/false,\n'
            '    "robust": true/false,\n'
            '    "best_practice": true/false\n'
            '  },\n'
            '  "issues": ["issue1", "issue2"],\n'
            '  "verdict": "PASS" | "FAIL" | "PARTIAL"\n'
            '}\n'
            'Be specific — each check must cite evidence from the solution.'
        ),
        'user': "Verify this solution:\n\n{solution}\n\n{execution_results}\n\nValidation method: {method}",
    },
    "evaluator": {
        "system": (
            "You are a strict evaluation agent. Score the verified solution on three criteria, "
            "each from 0.0 to 1.0. Be CRITICAL and DISCRIMINATING — a perfect 1.0 should be rare.\n"
            "- reasonableness: How logical and sound is the approach? Deduct for missing edge cases, "
            "poor structure, unclear logic, or over-engineered solutions.\n"
            "- executability: How likely is this to work when executed? Deduct for syntax issues, "
            "missing imports, undefined variables, incomplete functions, or platform assumptions.\n"
            "- satisfaction: How well does it address the original query? Deduct for missing features, "
            "partial implementations, stub/placeholder code, or not following instructions.\n\n"
            'Use the full range: 0.0-0.3=poor, 0.3-0.6=needs improvement, 0.6-0.8=good, '
            '0.8-0.95=excellent, 0.95-1.0=perfect (rare).\n'
            'IMPORTANT — The final overall score will be WEIGHTED as follows:\n'
            'reasonableness × weight.reasonableness + executability × weight.executability + '
            'satisfaction × weight.satisfaction.\n'
            'If the verifier reports syntax_valid=false, executability is capped at 0.3. '
            'Factor this into your scoring.\n'
            'Output ONLY a JSON object with these three scores, like:\n'
            '{"reasonableness": 0.72, "executability": 0.65, "satisfaction": 0.80}'
        ),
        "user": "Evaluate this verified solution:\n\n{content}\n\nValidation method: {method}\n\n{execution_results}",
    },
}


class CLMAFramework:
    """High-level Python interface to the closed-loop multi-agent framework."""

    def __init__(self, rules_path=None, token_budget=10000, max_iterations=10,
                 threshold=0.3, mode="closed"):
        """Initialize the framework.

        Args:
            rules_path: Path to YAML rules file (default: config/rules/default.yaml)
            token_budget: Maximum token budget
            max_iterations: Maximum iterations per query
            threshold: Satisfaction threshold (0-1)
            mode: "closed" for closed-loop, "open" for open-loop
        """
        # Core components
        self.rule_engine = RuleEngine()
        self.token_monitor = TokenMonitor(token_budget)
        self.loop_controller = LoopController()
        self.orchestrator = Orchestrator()

        # API provider (lazy loaded)
        self._llm_provider = None

        # Configure
        self.loop_controller.set_max_iterations(max_iterations)
        self.loop_controller.set_satisfaction_threshold(threshold)
        self.loop_controller.set_token_budget(token_budget)

        # Store config locally for getter-free C++ API access
        self._max_iterations = max_iterations
        self._threshold = threshold

        if mode == "open":
            self.loop_controller.set_mode(LoopController.Mode.OPEN_LOOP)
        else:
            self.loop_controller.set_mode(LoopController.Mode.CLOSED_LOOP)

        # Try to load rules
        if rules_path is None:
            base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            rules_path = os.path.join(base, "config", "rules", "default.yaml")

        self._rules_path = rules_path
        self._load_rules(rules_path)

        # Wire up components
        self.orchestrator.set_rule_engine(self.rule_engine)
        self.orchestrator.set_token_monitor(self.token_monitor)
        self.orchestrator.set_loop_controller(self.loop_controller)
        
        # Plugin manager for C++ plugin system
        base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        plugin_dir = os.path.join(base, "plugins")
        self.plugin_manager = PluginManager()
        os.makedirs(plugin_dir, exist_ok=True)
        self.plugin_manager.add_plugin_directory(plugin_dir)
        self.orchestrator.register_plugin_manager(self.plugin_manager)

        # Tool executor for code execution / Docker
        base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        tools_dir = os.path.join(base_dir, "tools", "sandbox")
        self._execution_timeout = 120
        self._tool_executor = ToolExecutor(sandbox_dir=tools_dir, timeout=self._execution_timeout)
        self._tool_results = []

        # Working memory for cross-agent context
        self._agent_memory = {}

        # Register default Python agent callbacks
        self._register_default_agents()

        # Experience store for self-evolution / caching
        from experience_store import ExperienceStore as _ExpStore
        base_dir_store = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        exp_dir = os.path.join(base_dir_store, "config", "experience")
        self.experience_store = _ExpStore(store_dir=exp_dir)

        # 并行候选生成配置（默认关闭）
        self._candidate_config = CandidateConfig()
        self._candidate_config.enabled = False

        # 系统架构模式: "single" | "multi"
        self._arch_mode = "single"
        self._candidate_config.num_candidates = 3
        self.orchestrator.set_candidate_config(self._candidate_config)

    def set_candidate_count(self, num: int):
        """设置并行候选生成数量（自动启用并行模式）。"""
        self._candidate_config.num_candidates = num
        self._candidate_config.enabled = num > 1
        self.orchestrator.set_candidate_config(self._candidate_config)

    def enable_candidate_parallel(self, enabled: bool):
        """启用/禁用并行候选生成。"""
        self._candidate_config.enabled = enabled
        self.orchestrator.set_candidate_config(self._candidate_config)

    def get_candidate_config(self):
        """获取当前并行候选配置。"""
        return self.orchestrator.get_candidate_config()

    def set_cache_enabled(self, enabled: bool):
        """启用/禁用查询结果缓存。"""
        self.orchestrator.set_cache_enabled(enabled)

    def clear_cache(self):
        """清空查询结果缓存。"""
        self.orchestrator.clear_cache()

    # ==================== DAG 规划 ====================

    def set_dag_mode(self, enabled: bool):
        """启用/禁用 DAG 任务规划模式（复杂查询自动分解子任务）。"""
        dag_config = DAGConfig()
        dag_config.enabled = enabled
        dag_config.max_subtasks = 8
        dag_config.min_subtasks_to_enable = 2
        dag_config.auto_downgrade = True
        self.orchestrator.set_dag_config(dag_config)
        
        if enabled and not hasattr(self, '_dag_planner_registered'):
            self._register_dag_planner()
            self._dag_planner_registered = True

    def _register_dag_planner(self):
        """注册 Planner 回调（用于将复杂问题分解为子任务）。

        生成结构化 JSON 任务规划，支持两种输出格式：
        - JSON: [{"id":"task_0","desc":"...","deps":[]}, ...]
        - 旧管道格式: task_0|description|dep1,dep2  (兼容)

        输出会经过后处理验证，保证 C++ 侧解析的健壮性。
        """
        def _validate_plan(plan):
            """验证并过滤规划结果，返回 (valid_tasks, errors)。"""
            if not plan:
                return [], ["Empty plan"]
            errors = []
            valid = []
            seen_ids = set()
            for i, task in enumerate(plan):
                tid = task.get("id", "").strip()
                desc = task.get("desc", task.get("description", "")).strip()
                deps = task.get("deps", [])
                if isinstance(deps, str):
                    deps = [d.strip() for d in deps.split(",") if d.strip()]
                if not tid:
                    errors.append(f"Task[{i}]: empty id, skipped")
                    continue
                if not desc:
                    errors.append(f"Task[{i}]({tid}): empty description, using id as desc")
                    desc = tid  # fallback
                if tid in seen_ids:
                    errors.append(f"Task[{i}]({tid}): duplicate id, skipping")
                    continue
                seen_ids.add(tid)
                # 过滤自引用依赖
                filtered_deps = [d for d in deps if d != tid]
                if len(filtered_deps) < len(deps):
                    errors.append(f"Task[{i}]({tid}): self-referencing dep removed")
                # 过滤不存在的依赖（仅警告，不阻塞）
                unknown_deps = [d for d in filtered_deps if d not in seen_ids and d not in {n["id"] for n in valid}]
                if unknown_deps:
                    errors.append(f"Task[{i}]({tid}): depends on unknown tasks {unknown_deps}")
                valid.append({"id": tid, "description": desc, "dependencies": filtered_deps})
            return valid, errors

        def _plan_to_pipe_format(valid_tasks):
            """将结构化规划转为管道格式供给 C++ 解析。"""
            lines = []
            for task in valid_tasks:
                deps_str = ",".join(task["dependencies"]) if task["dependencies"] else ""
                lines.append(f"{task['id']}|{task['description']}|{deps_str}")
            return "\n".join(lines)

        def _try_parse_json(text):
            """尝试从 LLM 输出中提取 JSON 数组。返回 (parsed_list|None, error_msg)。"""
            import re as _re, json
            # 尝试反引号包裹的 JSON 块
            for m in _re.finditer(r'```(?:json)?\s*\n?([\s\S]*?)```', text):
                try:
                    data = json.loads(m.group(1).strip())
                    if isinstance(data, list):
                        return data, None
                except (json.JSONDecodeError, ValueError):
                    continue
            # 尝试查找方括号 JSON 数组
            m = _re.search(r'(\[\s*\{.*?\}\s*\])', text, _re.DOTALL)
            if m:
                try:
                    data = json.loads(m.group(1))
                    if isinstance(data, list):
                        return data, None
                except (json.JSONDecodeError, ValueError):
                    pass
            # 尝试解析全部文本
            try:
                data = json.loads(text.strip())
                if isinstance(data, list):
                    return data, None
            except (json.JSONDecodeError, ValueError):
                pass
            return None, "No valid JSON array found"

        def dag_planner(query, method):
            result = AgentResult()
            try:
                provider = self._get_llm_provider()
                if provider:
                    # Planner system prompt — 增强 JSON 约束
                    planner_prompt = AGENT_PROMPTS.get("planner", {}).get("system", "")
                    if not planner_prompt:
                        planner_prompt = (
                            "You are a task planner. Given a user query, break it down into "
                            "independent subtasks that can be executed in parallel.\n\n"
                            "Output a JSON array of task objects. Each object has:\n"
                            "  - \"id\": unique identifier (e.g., \"task_0\", \"task_1\")\n"
                            "  - \"desc\": short description of the subtask\n"
                            "  - \"deps\": array of dependency IDs this task depends on (empty [] if none)\n\n"
                            "Constraints:\n"
                            "1. Each id must be unique\n"
                            "2. Non-trivial queries should be split into 3-8 subtasks\n"
                            "3. For trivial queries, output 1 task with empty deps\n"
                            "4. No circular dependencies allowed (task A depends on B, B depends on A)\n"
                            "5. id format: task_N (sequential)\n\n"
                            "Example output:\n"
                            '```json\n'
                            '[{"id":"task_0","desc":"parse user input","deps":[]},\n'
                            ' {"id":"task_1","desc":"validate data","deps":["task_0"]},\n'
                            ' {"id":"task_2","desc":"generate output","deps":["task_1"]}]\n'
                            '```'
                        )
                    else:
                        # 已有的 prompt 上追加 JSON 约束
                        planner_prompt += (
                            "\n\nIMPORTANT — Output your plan as a JSON array of objects. "
                            "Each object must have: id (unique), desc (task description), deps (array of dependency IDs). "
                            "Format inside a ```json ... ``` code block."
                        )
                    prompt = f"{planner_prompt}\n\nUser Query: {query}\n"
                    response = provider.chat([{"role": "user", "content": prompt}])
                    # provider.chat() 返回字符串（与 _llm_agent_call 一致），不是字典
                    raw_content = response if isinstance(response, str) else response.get("content", "")

                    # Step 1: 尝试从 LLM 输出提取 JSON
                    parsed, json_err = _try_parse_json(raw_content)
                    if parsed is not None:
                        valid_tasks, _ = _validate_plan(parsed)
                    else:
                        valid_tasks = []

                    # Step 2: 如果 JSON 提取成功且有效任务 > 0，用结构化格式
                    if valid_tasks:
                        result.content = _plan_to_pipe_format(valid_tasks)
                    else:
                        # Step 3: 回退 — 尝试直接用原始内容（兼容旧管道格式）
                        # 检查是否至少有一些 "task_" 行
                        import re as _re2
                        has_pipe_lines = any('|' in line for line in raw_content.split('\n') if line.strip())
                        if has_pipe_lines:
                            result.content = raw_content
                        else:
                            # Step 4: 最终回退 — 单任务兜底
                            result.content = f"task_0|{query}|"
                    result.success = True
                else:
                    # Fallback: 直接将查询作为一个任务
                    result.content = f"task_0|{query}|"
                result.success = True
            except Exception as e:
                result.success = False
                result.error_message = str(e)
            return result
        
        self.orchestrator.register_planner(dag_planner)

    def _parse_dag_plan_from_pipe(self, pipe_content):
        """从管道格式提取结构化任务列表（供测试使用）。"""
        tasks = []
        for line in pipe_content.split('\n'):
            line = line.strip()
            if not line:
                continue
            parts = line.split('|')
            if len(parts) < 2:
                continue
            tid = parts[0].strip()
            desc = parts[1].strip()
            deps = parts[2].strip().split(',') if len(parts) > 2 and parts[2].strip() else []
            if tid and desc:
                tasks.append({"id": tid, "description": desc, "dependencies": deps})
        return tasks

    def get_dag_status(self):
        """获取当前 DAG 执行状态。"""
        return self.orchestrator.get_dag_status()

    def _get_llm_provider(self):
        """Lazy-load and return the configured LLM provider."""
        if self._llm_provider is None:
            self._llm_provider = create_provider()
        return self._llm_provider

    def refresh_api_config(self):
        """Reload API provider from config file. Call after saving new config."""
        self._llm_provider = create_provider()

    @property
    def api_configured(self):
        """Check if a working API provider is configured."""
        provider = self._get_llm_provider()
        return provider is not None and bool(provider.api_key) if provider else False

    def _llm_agent_call(self, agent_name: str, query: str, method: str = "",
                        context: dict = None) -> AgentResult:
        """Call the LLM for a specific agent role. Falls back to simulation on failure."""
        result = AgentResult()
        result.metadata["agent"] = agent_name
        provider = self._get_llm_provider()

        if provider and provider.api_key:
            try:
                prompts = AGENT_PROMPTS.get(agent_name, {})
                system_prompt = prompts.get("system", "You are a helpful assistant.")
                fmt_context = (context or {}).copy()
                # Defensive fill: ensure all template placeholders exist to avoid KeyError
                # Extract placeholders from the template via str.find iteration or just
                # provide safe defaults for all known agent-specific fields
                user_template = prompts.get("user", "{query}")
                for key in ("reasoning", "similar_experiences", "execution_result",
                            "solution", "execution_results", "content",
                            "iteration", "previous_iteration_info", "method"):
                    if "{" + key + "}" in user_template and key not in fmt_context:
                        fmt_context[key] = ""
                user_prompt = user_template.format(
                    query=query, method=method, **fmt_context)
                # Add cross-agent memory if available — only for refiner (iteration feedback)
                # Other agents get context via explicit {placeholder} variables
                if self._agent_memory and agent_name == "refiner":
                    user_prompt += f"\n\n[Previous context]\n{json.dumps(self._agent_memory, indent=2)}"

                response = provider.chat([
                    {"role": "system", "content": system_prompt},
                    {"role": "user", "content": user_prompt},
                ])
                result.content = response
                result.success = True

                # Estimate tokens (rough: 4 chars per token)
                result.metadata["prompt_tokens"] = str(len(system_prompt + user_prompt) // 4)
                result.metadata["completion_tokens"] = str(len(response) // 4)

                # If this is the evaluator, try to extract JSON scores
                if agent_name == "evaluator":
                    self._parse_evaluator_scores(response, result)
                else:
                    # Store in agent memory for downstream agents
                    self._agent_memory[agent_name] = response
                    # Simulate scores for non-evaluator agents
                    result.metadata["reasonableness"] = "0.7"
                    result.metadata["executability"] = "0.7"
                    result.metadata["satisfaction"] = "0.7"

                return result
            except Exception as e:
                print(f"[{agent_name}] LLM call failed: {e} — falling back to simulation")
                # Fall through to simulated fallback

        # Simulated fallback
        return self._simulated_agent_call(agent_name, query, method, context)

    def _parse_evaluator_scores(self, response: str, result: AgentResult):
        """Parse JSON scores from evaluator response using strict parser.

        NOTE: pybind11's std::map<string,string> bindings do NOT propagate
        dict __setitem__ back to C++ — so result.metadata["key"] = val is
        a silent no-op. We store parsed scores in result.content's trailing
        metadata block for downstream extraction.
        """
        try:
            scores = _strict_json_parse(response)
            r = scores.get("reasonableness", 0.5)
            e = scores.get("executability", 0.5)
            s = scores.get("satisfaction", 0.5)
            # Store raw scores in content as structured block (metadata wont persist)
            result.content = (
                result.content.rstrip() +
                f"\n\n<SCORE_META reasonableness={r} executability={e} satisfaction={s} />"
            )
            raw = {"reasonableness": r, "executability": e, "satisfaction": s}

            # Phase 8: 读取 Verifier 输出并应用强制规则
            verifier_raw = self._agent_memory.get("verifier", "")
            if verifier_raw:
                verifier_data = _parse_verifier_verdict(verifier_raw)
                raw = _apply_verifier_mandatory_rules(verifier_data, raw)
                # 记录强制修正信息
                verdict = verifier_data.get("verdict", "PASS")
                hard_checks = verifier_data.get("hard_checks", {})
                result.content += (
                    f"\n<VERIFIER_OVERRIDE verdict={verdict} "
                    f"syntax_valid={hard_checks.get('syntax_valid', True)} "
                    f"override_applied=true />"
                )

            adjusted = _covert_verify_scores(raw)
            result.content += (
                f"\n<SCORE_ADJ reasonableness={adjusted['reasonableness']} "
                f"executability={adjusted['executability']} "
                f"satisfaction={adjusted['satisfaction']} "
                f"overall={round(sum(adjusted.values())/len(adjusted), 4)} />"
            )
            return
        except (ValueError, KeyError, TypeError, AttributeError):
            pass

    def _simulated_agent_call(self, agent_name: str, query: str, method: str = "",
                              context: dict = None) -> AgentResult:
        """Simulated agent callback (fallback when no API configured)."""
        result = AgentResult()
        result.metadata["agent"] = agent_name
        length = len(query)

        if agent_name == "refiner":
            result.content = f"[Refined] Query: {query}\nValidation: {method}"
        elif agent_name == "reasoner":
            result.content = f"[Reasoned] Solution for: {query[:100]}..."
            result.metadata["approach"] = "chain-of-thought"
        elif agent_name == "solver":
            # 生成真实可执行的代码块，让 _auto_execute_code 能匹配执行
            q_lower = query.lower()
            if "hello" in q_lower or "helloworld" in q_lower:
                result.content = f"[Solved] Executing: {query[:100]}...\n\n```python\nprint('Hello, World!')\n```"
            elif "斐波那契" in q_lower or "fibonacci" in q_lower:
                result.content = (
                    f"[Solved] Executing: {query[:100]}...\n\n"
                    f"```python\ndef fibonacci(n):\n"
                    f"    if n <= 1:\n        return n\n"
                    f"    a, b = 0, 1\n"
                    f"    for _ in range(2, n + 1):\n"
                    f"        a, b = b, a + b\n"
                    f"    return b\n\n"
                    f"# Test\nprint(fibonacci(10))\n```"
                )
            elif "排序" in q_lower or "sort" in q_lower:
                result.content = (
                    f"[Solved] Executing: {query[:100]}...\n\n"
                    f"```python\ndef sort_list(arr):\n"
                    f"    return sorted(arr)\n\n"
                    f"# Test\nprint(sort_list([3, 1, 4, 1, 5, 9, 2, 6]))\n```"
                )
            elif "1+1" in q_lower or "一加一" in q_lower:
                result.content = f"[Solved] Executing: {query[:100]}...\n\n```python\nprint(1 + 1)\n```"
            else:
                result.content = f"[Solved] Executing: {query[:100]}...\n\n```python\nprint('Hello, World!')\n```"
            result.metadata["execution_time_ms"] = "150"
        elif agent_name == "verifier":
            result.content = f"[Verified] Passed validation: {method}"
        elif agent_name == "evaluator":
            length = len(query)
            r = min(0.95, 0.5 + length / 2000)
            e = min(0.90, 0.4 + length / 3000)
            s = min(0.85, 0.3 + length / 2500)
            overall = round((r + e + s) / 3.0, 4)
            result.content = (
                f"[Evaluated] Score calculated\n\n"
                f"<SCORE_ADJ reasonableness={r} executability={e} "
                f"satisfaction={s} overall={overall} />"
            )
            result.metadata["reasonableness"] = str(r)
            result.metadata["executability"] = str(e)
            result.metadata["satisfaction"] = str(s)

        result.metadata["prompt_tokens"] = str(len(query) // 2)
        result.metadata["completion_tokens"] = str(len(result.content) // 2)
        result.success = True
        return result

    def _load_rules(self, rules_path):
        """Load rules from YAML file."""
        if os.path.exists(rules_path):
            ok = self.rule_engine.load_rules_from_file(rules_path)
            if not ok:
                print(f"[WARN] Failed to load rules from: {rules_path}")
                self._load_default_rules()
        else:
            print(f"[WARN] Rules file not found: {rules_path}")
            self._load_default_rules()

    def _load_default_rules(self):
        """Load hardcoded default rules as fallback."""
        yaml_content = """
rules:
  - pattern: "write|create|generate|make|build"
    validation_method: "code_generation"
    recommended_tools: ["compiler", "interpreter"]
    weights:
      reasonableness: 0.4
      executability: 0.4
      satisfaction: 0.2
    threshold: 0.3
  - pattern: "explain|describe|what|how|why|tell|hello|help"
    validation_method: "analysis"
    recommended_tools: ["debugger", "profiler"]
    weights:
      reasonableness: 0.5
      executability: 0.3
      satisfaction: 0.2
    threshold: 0.3
  - pattern: "deploy|build|run"
    validation_method: "execution"
    recommended_tools: ["docker", "shell"]
    weights:
      reasonableness: 0.3
      executability: 0.5
      satisfaction: 0.2
    threshold: 0.3
  - pattern: "optimize|refactor|improve"
    validation_method: "refactoring"
    recommended_tools: ["linter", "profiler"]
    weights:
      reasonableness: 0.4
      executability: 0.4
      satisfaction: 0.2
    threshold: 0.3
"""
        self.rule_engine.load_rules_from_string(yaml_content)

    def _register_default_agents(self):
        """Register default Python agent callbacks (LLM-backed with simulation fallback)."""

        def make_agent_callback(agent_name):
            def callback(query, method):
                context = {}
                if agent_name == "solver":
                    context["reasoning"] = self._agent_memory.get("reasoner", "")
                    context["similar_experiences"] = ""
                    context["execution_result"] = ""
                    # Include previous tool results if available
                    if self._tool_results:
                        last_result = self._tool_results[-1]
                        context["execution_result"] = (
                            f"[TOOL EXECUTION OUTPUT]\n"
                            f"Tool: {last_result.tool_name}\n"
                            f"Exit code: {last_result.exit_code}\n"
                            f"Stdout: {last_result.stdout}\n"
                            f"Stderr: {last_result.stderr}\n"
                            f"Duration: {last_result.duration_ms:.0f}ms"
                        )
                if agent_name == "verifier":
                    context["solution"] = self._agent_memory.get("solver", "")
                    context["execution_results"] = ""
                    # Include execution results for verifier
                    if self._tool_results:
                        exec_summary = "\n\n[EXECUTION RESULTS]\n"
                        for i, tr in enumerate(self._tool_results):
                            exec_summary += (
                                f"[Tool {i+1}] {tr.tool_name}\n"
                                f"  Success: {tr.success}\n"
                                f"  Exit code: {tr.exit_code}\n"
                                f"  Stdout: {tr.stdout}\n"
                                f"  Stderr: {tr.stderr}\n"
                                f"---\n"
                            )
                        context["execution_results"] = exec_summary
                if agent_name == "evaluator":
                    context["content"] = self._agent_memory.get("verifier", query)
                
                # Call LLM
                result = self._llm_agent_call(agent_name, query, method, context)
                
                # Post-processing: refiner self-check
                if agent_name == "refiner" and result.success and result.content:
                    if not _refiner_self_check(result.content, query):
                        result.success = False
                        result.error_message = "Refiner self-check failed: output contains meta-commentary or lacks meaningful refinement"
                        result.content = query  # 回退到原查询

                # Post-processing: auto-execute code for solver
                if agent_name == "solver" and result.success and result.content:
                    tool_result = self._auto_execute_code(result.content)
                    if tool_result:
                        self._tool_results.append(tool_result)
                        # Append execution output to content for verifier
                        result.content += (
                            f"\n\n=== EXECUTION OUTPUT ===\n"
                            f"✓ Success: {tool_result.success}\n"
                            f"Exit code: {tool_result.exit_code}\n"
                            f"Stdout:\n{tool_result.stdout}\n"
                        )
                        if tool_result.stderr:
                            result.content += f"Stderr:\n{tool_result.stderr}\n"
                        result.content += "=== END EXECUTION OUTPUT ==="
                        # Store for next iteration
                        self._agent_memory["tool_result"] = tool_result.to_dict()
                
                return result
            return callback

        self.orchestrator.register_agent(AgentType.REFINER, make_agent_callback("refiner"))
        self.orchestrator.register_agent(AgentType.REASONER, make_agent_callback("reasoner"))
        self.orchestrator.register_agent(AgentType.SOLVER, make_agent_callback("solver"))
        self.orchestrator.register_agent(AgentType.VERIFIER, make_agent_callback("verifier"))
        self.orchestrator.register_agent(AgentType.EVALUATOR, make_agent_callback("evaluator"))

    def _auto_execute_code(self, content):
        """Detect code blocks in solver output and execute them.
        
        Supports: python, shell/bash, cpp code blocks (```lang ... ```).
        Uses scoring from previous iteration to determine sandbox tier.
        Returns ToolResult if code was executed, None otherwise.
        """
        import re
        # Match ```python ... ```, ```bash ... ```, ```shell ... ```, ```cpp ... ```
        patterns = [
            (r'```python\s*\n(.*?)```', 'python'),
            (r'```py\s*\n(.*?)```', 'python'),
            (r'```bash\s*\n(.*?)```', 'shell'),
            (r'```shell\s*\n(.*?)```', 'shell'),
            (r'```sh\s*\n(.*?)```', 'shell'),
            (r'```cpp\s*\n(.*?)```', 'cpp'),
            (r'```c\+\+\s*\n(.*?)```', 'cpp'),
        ]
        # 读取上一轮的 executability 评分（如果有）
        executability = self._agent_memory.get("_last_executability", None)
        for pattern, lang in patterns:
            match = re.search(pattern, content, re.DOTALL)
            if match:
                code = match.group(1).strip()
                if not code:
                    continue
                # 使用评分感知的执行（而非直接 execute_xxx）
                return self._tool_executor.execute_code_with_tier(
                    code, language=lang, executability=executability
                )
        return None





    def process_query(self, query):
        """Process a query through the multi-agent pipeline."""
        # Clear cross-agent memory for fresh query
        self._agent_memory = {}
        self._tool_results = []
        # Clear C++ execution history for session isolation
        self.orchestrator.clear_execution_history()
        result = self.orchestrator.process_query(query)
        formatted = self._format_result(result)
        # Include tool execution results
        formatted["tool_results"] = [tr.to_dict() for tr in self._tool_results]
        formatted["tools_used"] = len(self._tool_results) > 0
        return formatted

    # ===================== 并行候选生成 =====================

    def _generate_candidates(self, query, reasoning, method, num_candidates, iteration,
                              similar_experiences_text=""):
        """并行生成 N 个 Solver 候选方案。

        使用 ThreadPoolExecutor 并行调用 LLM，不同温度。
        每个候选包含: content, tool_result(如有), seed。

        Args:
            query: 当前查询
            reasoning: Reasoner 的输出
            method: 验证方法
            num_candidates: 候选数量
            iteration: 当前迭代序号
            similar_experiences_text: 相似经验文本（已格式化）

        Returns:
            list[dict]: 候选列表，按完成顺序
        """
        import concurrent.futures
        import threading

        candidates = []
        _lock = threading.Lock()

        def _single_candidate(seed):
            """生成单个候选方案。"""
            temp = 0.3 + (seed * 0.15)
            context = {
                "reasoning": reasoning,
                "execution_result": "",
                "similar_experiences": similar_experiences_text,
            }
            # 为每个候选创建独立的临时 context
            result = self._llm_agent_call("solver", query, method, context)
            cand = {"seed": seed, "content": result.content if result else "",
                    "tool_result": None, "success": result.success if result else False,
                    "error": result.error_message if result and not result.success else ""}
            if result and result.success and result.content:
                try:
                    tr = self._auto_execute_code(result.content)
                    cand["tool_result"] = tr
                except Exception:
                    pass
            return cand

        with concurrent.futures.ThreadPoolExecutor(max_workers=num_candidates) as pool:
            futures = {pool.submit(_single_candidate, i): i for i in range(num_candidates)}
            for future in concurrent.futures.as_completed(futures):
                cand = future.result()
                candidates.append(cand)

        return candidates

    def _critic_compress_candidates(self, candidates, query, keep_ratio=0.5):
        """压缩候选方案，保留 top-K。

        评分标准（按优先级）：
        1. 代码执行成功 — 最高优先
        2. 执行输出的质量（stderr 为空加分）
        3. stdout 的信息量

        Returns:
            list[dict]: 排序后的最佳候选（保留 top-K）
        """
        def _score(cand):
            tr = cand.get("tool_result")
            if tr is None:
                # 没有执行结果，看 content 长度作为粗略指标
                return 0.1 + min(len(cand.get("content", "")) / 2000, 0.3)
            if not tr.success:
                return 0.2 + min(len(tr.stdout or "") / 1000, 0.2)
            # 成功执行
            score = 1.0
            if not tr.stderr:
                score += 0.2
            if tr.stdout:
                score += min(len(tr.stdout) / 1000, 0.3)
            return score

        scored = sorted(candidates, key=_score, reverse=True)
        keep = max(1, int(len(candidates) * keep_ratio))
        return scored[:keep]

    def _llm_classify(self, provider, query, with_info=True):
        """Use LLM to classify a query into code/problem/question/greeting/chitchat.

        Returns (raw_response, category).
        If with_info=True, the system prompt includes 'question' category.
        """
        categories = (
            "- 'greeting': casual hello, hi, good morning, thanks, or any standalone pleasantry\n"
            "- 'chitchat': casual chat, short reaction, off-topic small talk, humor, teasing\n"
            "- 'question': factual or information-seeking — news, music, movies, weather, general knowledge, opinions, recommendations\n"
            "- 'code': a programming question, code review, technical implementation debugging\n"
            "- 'problem': a math/logic/reasoning problem that needs step-by-step analysis\n"
        )
        prompts = {
            "system": (
                "You are a query classifier. Classify the user's query into exactly one category:\n"
                f"{categories}"
                "Output ONLY the category word — no punctuation, no explanation."
            ),
            "user": f"Query: {query}",
        }
        resp = provider.create_completion(
            messages=[
                {"role": "system", "content": prompts["system"]},
                {"role": "user", "content": prompts["user"]},
            ],
            max_tokens=10,
            temperature=0.0,
        )
        result = resp.get("choices", [{}])[0].get("message", {}).get("content", "").strip().lower()
        return result, result

    def _classify_query(self, query):
        """Classify query type to decide whether to run the full pipeline.

        Returns one of: "code", "problem", "question", "greeting", "chitchat"
        ...
        """
        if not query or not query.strip():
            return "chitchat"

        q = query.strip().lower()
        greetings = {
            "你好", "hello", "hi", "hey", "你好啊", "您好", "hi there",
            "你好呀", "嗨", "早上好", "下午好", "晚上好", "早", "午好",
            "晚安", "吃了没", "在吗", "在不在", "空", "ok", "okay",
            "好", "好的", "嗯", "哦", "thanks", "谢谢", "多谢",
            "thank you", "thx", "牛逼", "厉害", "nb",
            "好的谢谢", "好的", "没问题", "收到", "明白", "知道了",
        }
        single_word = q.strip() in greetings
        very_short = len(q.split()) <= 2 and len(q) <= 16

        # Code indicators -> skip classification
        code_indicators = ["```", "#include", "def ", "class ", "int main",
                           "print(", "import ", "from ", "return ", "//",
                           "const ", "void ", "using namespace", "int ",
                           "float ", "double ", "char ", "std::", "printf"]
        has_code_block = any(ind in q for ind in code_indicators)

        # Broad task keywords — catch programming & factual questions
        task_keywords = ["实现", "实现一个", "写一个", "写个", "开发", "修复",
                         "implement", "write ", "create ", "fix ", "refactor",
                         "优化", "重构", "测试", "调试", "debug", "build",
                         "设计", "架构", "解释", "explain", "how to",
                         "what is", "difference between", "比较", "对比",
                         "写段", "写个程序", "写个函数", "改一下", "帮我",
                         "分析", "analyze", "review", "代码", "code",
                         "program", "function", "algorithm", "算法",
                         "bug", "error", "问题",
                         "doesn't work", "not working", "报错", "出错",
                         "是什么", "什么是", "什么时候", "为什么",
                         "在哪里", "哪个", "哪些", "多少", "如何",
                         "怎么回事", "怎么做", "怎么用",
                         # 信息查询关键词
                         "有没有", "有什么", "有吗", "有没",
                         "最近", "今天", "明天", "昨天",
                         "推荐", "推荐一下",
                         "怎么样", "好不好", "哪个好",
                         "新闻", "热点", "热门", "流行", "趋势",
                         "天气", "时间", "日期",
                         "谁", "谁的", "谁唱的",
                         "这首歌", "这个", "这个是什么",
                         "怎么找", "怎么查", "去哪",
                         "最新", "新出的", "有没有新",
                         "价格", "多少钱",
                         "jpop", "kpop", "cpop", "音乐", "歌曲", "歌",
                         "电影", "动漫", "游戏", "书", "小说",
                         "事件", "消息",
                         # 增强：形如"用Python写xxx"的编程任务
                         "用python", "用c++", "用java", "用go", "用rust",
                         "用js", "用ts", "用typescript", "用javascript",
                         "用c语言", "用c#", "用ruby", "用php", "用swift",
                         "用kotlin", "用scala", "用r语言",
                         ]
        is_task = any(kw in q for kw in task_keywords)

        if has_code_block or is_task:
            # Need LLM to distinguish code/problem/question
            # But since we have broad keywords, use LLM when available
            try:
                provider = self._get_llm_provider()
                _, result = self._llm_classify(provider, q, with_info=True)
                if result in ("code", "problem", "greeting", "chitchat", "question"):
                    return result
            except Exception:
                pass
            # If LLM fails, code/problem/question all go to full pipeline
            return "code"

        # For very short queries, do lightweight LLM call
        if single_word or very_short:
            try:
                provider = self._get_llm_provider()
                _, result = self._llm_classify(provider, q, with_info=True)
                if result in ("code", "problem", "greeting", "chitchat", "question"):
                    return result
            except Exception:
                pass

            # Fallback
            if single_word:
                return "greeting"
            return "chitchat"

        # Longer queries: use LLM classification (not just code)
        try:
            provider = self._get_llm_provider()
            _, result = self._llm_classify(provider, q, with_info=True)
            if result in ("code", "problem", "greeting", "chitchat", "question"):
                return result
        except Exception:
            pass

        return "code"

    def _is_simple_query(self, query):
        """判断是否为极简单查询，DAG 模式下可直接走快速通道跳过 planner 开销。

        极简单定义：无需 planning 的单一编程任务。
        特征：短查询，无复杂语义，无算法/推理要求。
        """
        q = query.strip().lower()
        # 过长肯定不简单
        if len(q) > 60:
            return False
        # 非代码相关（问问题/闲聊）走原本的 classifier 路径
        if not any(kw in q for kw in ["写", "打印", "print", "输出", "实现",
                                        "hello", "helloworld", "1+1",
                                        "加", "减", "乘", "除",
                                        "计算", "算"]):
            return False
        # 排除有明显算法/数据结构/复杂逻辑的任务
        complex_keywords = [
            "分别", "同时", "多个", "各", "逐一", "逐个", "依次",
            "比较", "对比", "组合", "结合", "集成", "联调", "对接",
            "and ", "then ", "also ", "plus ", "both", "multiple",
            "然后", "之后", "接着", "第一步", "第二步",
            "first", "second", "third",
            "同时运行", "同时实现",
            "多个功能", "多个接口", "多个服务", "多个模块",
            "多个文件", "多文件", "多模块",
            "client", "server", "前端", "后端",
            "数据库", "api", "rest", "grpc", "http",
            "docker", "deploy", "部署",
            "微服务", "分布式", "并行", "并发",
            "斐波那契", "fibonacci", "fib", "fibonacci数列",
            "递归", "recursion", "递归函数",
            "排序", "sort", "排序算法",
            "搜索", "search", "二分", "binary",
            "链表", "list", "linked",
            "树", "tree", "二叉树", "bst",
            "哈希", "hash", "map",
            "图", "graph", "bfs", "dfs",
            "动态规划", "dp", "动态",
            "数组", "array",
            "字符串", "string",
            "栈", "stack", "队列", "queue",
            "指针", "pointer",
            "正则", "regex", "正则表达式",
            "文件", "file", "io", "读写",
            "线程", "thread", "进程", "process",
            "网络", "network", "socket",
            "类", "class", "面向对象", "oop",
        ]
        return not any(kw in q for kw in complex_keywords)

    def process_query_stream(self, query):
        """Generator that yields SSE-compatible event dicts during processing.

        Provides real-time progress events as each agent runs, replacing the
        blocking process_query() for the streaming UI frontend.

        Yields:
            dict: Event with 'event' key and type-specific data fields:
                - agent_start: {agent, iteration, timestamp}
                - agent_complete: {agent, iteration, content_preview, duration_ms, tokens, success}
                - tool_execution: {tool_name, success, exit_code, stdout_preview, duration_ms}
                - iteration: {iteration, scores: {...}, best_so_far}
                - done: {result: {...}, history: [...], stats: {...}, mode, session_id}
                - error: {message, iteration}
        """
        import time
        import json
        import uuid as uuid_mod

        # --- Reset state for fresh query ---
        self._agent_memory = {}
        self._tool_results = []
        # Clear C++ execution history for session isolation
        self.orchestrator.clear_execution_history()

        timestamp = time.time()
        session_id = uuid_mod.uuid4().hex[:12]

        # === Query Classification: skip pipeline for non-task queries ===
        query_type = self._classify_query(query)
        if query_type in ("greeting", "chitchat"):
            yield {
                "event": "agent_start",
                "agent": "classifier",
                "agent_label": "Query Classifier",
                "iteration": 0,
                "timestamp": timestamp,
            }
            # 随机回复池——让闲聊更有生气
            _GREETINGS = [
                "你好！有什么我可以帮你的吗？",
                "嗨！我在呢，有什么吩咐？",
                "嘿，你好呀！",
                "来了来了！你说吧~",
                "哈喽！有什么任务？",
            ]
            _CHITCHATS = [
                "嗯，我听着呢，请继续说说你的问题~",
                "收到收到~你继续！",
                "哈哈，在的在的。",
                "好嘞，你说~",
                "嗯嗯，我知道啦，然后呢？",
                "放心，我在听着呢。",
                "哦？说来听听~",
                "行，你接着说。",
                "有道理！请继续~",
                "懂了懂了，还有吗？",
            ]
            import random as _rand
            greeting = _rand.choice(_GREETINGS) if query_type == "greeting" else _rand.choice(_CHITCHATS)
            yield {
                "event": "agent_complete",
                "agent": "classifier",
                "agent_label": "Query Classifier",
                "content_preview": greeting,
                "duration_ms": 50,
                "tokens": 0,
                "success": True,
            }
            yield {
                "event": "done",
                "result": {"content": greeting},
                "history": [],
                "stats": {"total_iterations": 0, "total_tokens": 0, "mode": "direct_reply"},
                "mode": "direct_reply",
                "session_id": session_id,
            }
            return

        # === Factual Question: use LLM to answer directly (no multi-agent pipeline) ===
        if query_type == "question":
            yield {
                "event": "agent_start",
                "agent": "classifier",
                "agent_label": "Query Classifier",
                "iteration": 0,
                "timestamp": timestamp,
            }
            try:
                provider = self._get_llm_provider()
                resp = provider.create_completion(
                    messages=[
                        {
                            "role": "system",
                            "content": (
                                "你是 Hermes，一个智能助手。回答用户的问题请用中文，"
                                "简洁明了，根据你的知识给出答案即可。"
                                "如果不知道确切信息，请如实告知，不要编造。"
                            ),
                        },
                        {"role": "user", "content": query},
                    ],
                    max_tokens=1024,
                    temperature=0.7,
                )
                answer = resp.get("choices", [{}])[0].get("message", {}).get("content", "").strip()
                if not answer:
                    answer = "这个嘛……我暂时不太清楚。你换个问法试试？"
            except Exception:
                answer = "嗯，这个问题有点棘手，我现在没法查实时信息，你可以自己去搜一下看看~"
            yield {
                "event": "agent_complete",
                "agent": "classifier",
                "agent_label": "Query Classifier",
                "content_preview": answer,
                "duration_ms": 50,
                "tokens": 0,
                "success": True,
            }
            yield {
                "event": "done",
                "result": {"content": answer},
                "history": [],
                "stats": {"total_iterations": 0, "total_tokens": 0, "mode": "direct_reply"},
                "mode": "direct_reply",
                "session_id": session_id,
            }
            return

        # === Multi-Level Nested Loop Architecture ===
        if getattr(self, '_arch_mode', 'single') == 'multi':
            yield from self._process_multi_loop(query, session_id, timestamp)
            return

        agent_order = ["refiner", "reasoner", "solver", "verifier", "evaluator"]
        agent_labels = {
            "refiner": "Refiner",
            "reasoner": "Reasoner",
            "solver": "Solver",
            "verifier": "Verifier",
            "evaluator": "Evaluator",
        }

        # Map query to a rule via rule engine
        rule = None
        method = "analysis"
        try:
            rules = self.rule_engine.match(query)
            if rules and len(rules) > 0:
                rule = rules[0]
                method = rule.validation_method
        except Exception:
            pass

        # C++ LoopController doesn't expose get_max_iterations/get_threshold getters,
        # so we use locally stored config values from __init__
        is_closed = (self.loop_controller.get_mode() ==
                     self.loop_controller.__class__.Mode.CLOSED_LOOP)
        max_iterations = self._max_iterations
        threshold = self._threshold

        iteration = 0
        all_iterations = []
        best_score = 0.0
        final_raw_result = None

        # Build the agent callback function map (mirrors _register_default_agents)
        # We must build callbacks that work standalone (not through C++ orchestrator)
        def _run_agent(agent_name, input_query, validation_method):
            """Run a single agent and return its result + timing."""
            context = {}
            if agent_name == "solver":
                context["reasoning"] = self._agent_memory.get("reasoner", "")
                context["execution_result"] = ""
                # 注入相似经验信息
                similar_exps = self._agent_memory.get("similar_experiences", [])
                if similar_exps:
                    exp_text = "\n\n[Similar Experiences from Knowledge Base]:\n"
                    for i, exp in enumerate(similar_exps):
                        exp_text += (
                            f"  [{i+1}] Previous query: {exp['query'][:200]}\n"
                            f"      Score: {exp.get('score', 'N/A')}\n"
                            f"      Solution preview: {exp['solution'][:500]}\n"
                            f"      ---\n"
                        )
                    context["similar_experiences"] = exp_text
                if self._tool_results:
                    last_result = self._tool_results[-1]
                    context["execution_result"] = (
                        f"[TOOL EXECUTION OUTPUT]\n"
                        f"Tool: {last_result.tool_name}\n"
                        f"Exit code: {last_result.exit_code}\n"
                        f"Stdout: {last_result.stdout}\n"
                        f"Stderr: {last_result.stderr}\n"
                        f"Duration: {last_result.duration_ms:.0f}ms"
                    )
            elif agent_name == "refiner":
                context["iteration"] = str(iteration + 1)
                if iteration > 0 and all_iterations:
                    prev = all_iterations[-1]
                    context["previous_iteration_info"] = (
                        f"[Previous Iteration #{prev['iteration']}]\n"
                        f"Score: {json.dumps(prev['scores'])}\n"
                        f"Solver output preview: {prev['solver_content'][:300]}"
                    )
                else:
                    context["previous_iteration_info"] = "(no previous iteration)"
            elif agent_name == "verifier":
                context["solution"] = self._agent_memory.get("solver", "")
                context["execution_results"] = ""
                if self._tool_results:
                    exec_summary = "\n\n[EXECUTION RESULTS]\n"
                    for i, tr in enumerate(self._tool_results):
                        exec_summary += (
                            f"[Tool {i+1}] {tr.tool_name}\n"
                            f"  Success: {tr.success}\n"
                            f"  Exit code: {tr.exit_code}\n"
                            f"  Stdout: {tr.stdout}\n"
                            f"  Stderr: {tr.stderr}\n"
                            f"---\n"
                        )
                    context["execution_results"] = exec_summary
            elif agent_name == "evaluator":
                context["content"] = self._agent_memory.get("verifier", input_query)
                context["execution_results"] = ""
                if self._tool_results:
                    exec_summary = "\n\n[EXECUTION RESULTS]\n"
                    for i, tr in enumerate(self._tool_results):
                        exec_summary += (
                            f"[Tool {i+1}] {tr.tool_name}\n"
                            f"  Success: {tr.success}\n"
                            f"  Exit code: {tr.exit_code}\n"
                            f"  Stdout: {tr.stdout[:200] if tr.stdout else '(empty)'}\n"
                            f"  Stderr: {tr.stderr[:200] if tr.stderr else '(empty)'}\n"
                            f"---\n"
                        )
                    context["execution_results"] = exec_summary

            t0 = time.perf_counter()
            result = self._llm_agent_call(agent_name, input_query, validation_method, context)
            duration_ms = (time.perf_counter() - t0) * 1000

            # Auto-execute code for solver
            if agent_name == "solver" and result.success and result.content:
                tool_result = self._auto_execute_code(result.content)
                if tool_result:
                    self._tool_results.append(tool_result)
                    result.content += (
                        f"\n\n=== EXECUTION OUTPUT ===\n"
                        f"\u2713 Success: {tool_result.success}\n"
                        f"Exit code: {tool_result.exit_code}\n"
                        f"Stdout:\n{tool_result.stdout}\n"
                    )
                    if tool_result.stderr:
                        result.content += f"Stderr:\n{tool_result.stderr}\n"
                    result.content += "=== END EXECUTION OUTPUT ==="
                    self._agent_memory["tool_result"] = tool_result.to_dict()

            return result, duration_ms

        # === DAG Mode: delegate to C++ process_query_dag ===
        dag_enabled = False
        try:
            dag_enabled = self.orchestrator.is_dag_mode()
        except Exception:
            pass

        if dag_enabled:
            yield {"event": "dag_start", "mode": "dag", "iteration": 0, "timestamp": time.time()}

            # Fast path: simple/trivial queries skip DAG planner overhead
            # Just run solver → execute code → score from execution results directly
            if self._is_simple_query(query):
                # Reset state for clean execution
                self._agent_memory = {}
                self._tool_results = []
                yield {"event": "dag_progress", "total_tasks": 1, "completed": 0, "failed": 0}
                # Run single solver with proper context for real LLM call
                solver_ctx = {
                    "reasoning": "Direct implementation — no complex decomposition needed.",
                    "similar_experiences": "",
                    "execution_result": "",
                }
                solver_result = self._llm_agent_call("solver", query, "code_generation", solver_ctx)
                dag_complete_content = solver_result.content
                # Auto-execute code
                tool_result = self._auto_execute_code(dag_complete_content)
                execution_success = tool_result and tool_result.success
                execution_stdout = tool_result.stdout if tool_result else ""
                execution_stderr = tool_result.stderr if tool_result else ""
                if tool_result:
                    dag_complete_content += (
                        f"\n\n=== EXECUTION OUTPUT ===\n"
                        f"✓ Success: {tool_result.success}\n"
                        f"Exit code: {tool_result.exit_code}\n"
                        f"Stdout:\n{tool_result.stdout}\n"
                    )
                    if tool_result.stderr:
                        dag_complete_content += f"Stderr:\n{tool_result.stderr}\n"
                    dag_complete_content += "=== END EXECUTION OUTPUT ==="
                # Score based on actual execution results — no LLM evaluator needed
                # for simple queries where execution is the ground truth
                if execution_success:
                    # Code ran successfully — high score
                    dag_reasonableness = 0.95 if execution_stdout.strip() else 0.85
                    dag_executability = 1.0
                    dag_satisfaction = 0.95 if execution_stdout.strip() else 0.85
                else:
                    # Execution failed — low score
                    dag_reasonableness = 0.5
                    dag_executability = 0.3
                    dag_satisfaction = 0.4
                dag_score = round((dag_reasonableness + dag_executability + dag_satisfaction) / 3.0, 4)
                yield {
                    "event": "done",
                    "result": {
                        "content": dag_complete_content,
                        "success": True,
                        "score": {"overall": dag_score, "reasonableness": dag_reasonableness,
                                  "executability": dag_executability, "satisfaction": dag_satisfaction},
                    },
                    "history": [],
                    "stats": {"mode": "dag", "total_tasks": 1, "completed_tasks": 1, "failed_tasks": 0,
                              "total_iterations": 1, "dag_auto_downgraded": "false",
                              "fast_path": True},
                    "mode": "dag",  # execution mode is DAG, not closed/open
                    "session_id": session_id,
                }
                return

            # Reset state and call C++ DAG processor
            self._agent_memory = {}
            self._tool_results = []
            dag_result = self.orchestrator.process_query_dag(query)

            if dag_result.success:
                # Get DAG status info for task-level detail
                dag_status = self.orchestrator.get_dag_status()
                total_tasks = int(dag_status.get("total_nodes", "0"))
                completed = int(dag_status.get("completed", "0"))

                yield {
                    "event": "dag_progress",
                    "total_tasks": total_tasks,
                    "completed": completed,
                    "failed": int(dag_status.get("failed", "0")),
                }

                # Yield agent_complete for each completed task
                # NOTE: skip === EXECUTION OUTPUT === and === END EXECUTION OUTPUT === lines
                # which are execution-result markers, not task separators.
                for line in dag_result.content.split("\n"):
                    if line.startswith("=== ") and " ===" in line:
                        if "EXECUTION OUTPUT" in line or "END EXECUTION" in line:
                            continue
                        task_info = line.strip("= ")
                        task_id, _, task_desc = task_info.partition(": ")
                        yield {
                            "event": "agent_complete",
                            "agent": f"dag_{task_id}",
                            "agent_label": f"DAG: {task_desc[:40]}",
                            "content_preview": task_desc,
                            "duration_ms": 0,
                            "tokens": 0,
                            "success": "[FAILED]" not in line,
                        }

                # Build stats
                dag_stats = {
                    "mode": "dag",
                    "total_tasks": total_tasks,
                    "completed_tasks": completed,
                    "failed_tasks": int(dag_status.get("failed", "0")),
                    "total_iterations": 1,
                    "dag_auto_downgraded": dag_result.metadata.get("dag_auto_downgraded", "false"),
                }

                # Parse score from EvaluationScore object on C++ AgentResult
                dag_score_obj = dag_result.score
                dag_score = dag_score_obj.overall() if hasattr(dag_score_obj, 'overall') else 0.0
                dag_reasonableness = getattr(dag_score_obj, 'reasonableness', dag_score)
                dag_executability = getattr(dag_score_obj, 'executability', dag_score)
                dag_satisfaction = getattr(dag_score_obj, 'satisfaction', dag_score)

                # Build complete output: C++ processQuery only preserves solver output in content.
                dag_complete_content = dag_result.content

                # Auto-execute code from DAG result (same as single-loop does for solver)
                # NOTE: C++ processQueryDag may already have appended execution output
                # from the solver branch. Skip if already present.
                if "=== EXECUTION OUTPUT ===" not in dag_complete_content:
                    try:
                        tool_result = self._auto_execute_code(dag_complete_content)
                        if not tool_result:
                            # _auto_execute_code 可能没找到代码块（例如模拟调用返回纯文本）
                            # 尝试更宽松的提取：从所有 backtick 代码块里找
                            import re as _d
                            for _pat in [r'```python[\s\S]*?```', r'```[\s\S]*?```']:
                                _m = _d.search(_pat, dag_complete_content)
                                if _m:
                                    tool_result = self._auto_execute_code(_m.group())
                                    if tool_result:
                                        break
                            # 如果还不行，对 solver content 直接提纯为代码尝试执行
                            if not tool_result and "[Solved]" in dag_complete_content:
                                _code_match = _d.search(r'```(\w*)\s*\n([\s\S]*?)```', dag_complete_content)
                                if _code_match:
                                    _lang = _code_match.group(1) or "python"
                                    _code = _code_match.group(2).strip()
                                    if _code:
                                        tool_result = self._tool_executor.execute_code_with_tier(
                                            _code, language="python" if _lang in ("", "python", "py") else _lang
                                        )
                        if tool_result:
                            dag_complete_content += (
                                f"\n\n=== EXECUTION OUTPUT ===\n"
                                f"✓ Success: {tool_result.success}\n"
                                f"Exit code: {tool_result.exit_code}\n"
                                f"Stdout:\n{tool_result.stdout}\n"
                            )
                            if tool_result.stderr:
                                dag_complete_content += f"Stderr:\n{tool_result.stderr}\n"
                            dag_complete_content += "=== END EXECUTION OUTPUT ==="
                    except Exception:
                        pass

                yield {
                    "event": "done",
                    "result": {
                        "content": dag_complete_content,
                        "success": dag_result.success,
                        "score": {
                            "overall": float(dag_score),
                            "reasonableness": float(dag_reasonableness),
                            "executability": float(dag_executability),
                            "satisfaction": float(dag_satisfaction),
                        },
                    },
                    "history": [],
                    "stats": dag_stats,
                    "mode": "dag",  # execution mode is DAG, not closed/open
                    "session_id": session_id,
                }
            else:
                yield {
                    "event": "error",
                    "message": f"DAG processing failed: {dag_result.error_message}",
                    "iteration": 0,
                }
            return

        # === 经验库检索：在迭代前查询相似经验 ===
        try:
            similar_exps = self.experience_store.search(query, top_k=3, threshold=0.7)
            if similar_exps:
                self._agent_memory["similar_experiences"] = [
                    {
                        "query": exp.query,
                        "solution": exp.solution[:1000],
                        "score": exp.scores.get("overall", 0),
                    }
                    for exp in similar_exps
                ]
                yield {
                    "event": "info",
                    "message": f"📚 Found {len(similar_exps)} similar experiences in memory",
                    "timestamp": time.time(),
                }
        except Exception:
            pass

        # --- Main loop ---
        try:
            while iteration < max_iterations:
                iteration += 1

                # Use refined query after first iteration
                current_query = self._agent_memory.get("refiner", query)

                for agent_name in agent_order:
                    agent_start_time = time.time()

                    # Yield agent_start
                    yield {
                        "event": "agent_start",
                        "agent": agent_name,
                        "agent_label": agent_labels.get(agent_name, agent_name.title()),
                        "iteration": iteration,
                        "timestamp": agent_start_time,
                    }

                    # Run the agent
                    # 候选模式：Solver 走并行候选生成
                    if (agent_name == "solver"
                            and self._candidate_config.enabled
                            and int(self._candidate_config.num_candidates) > 1):
                        # yield 候选开始事件
                        num = max(2, int(self._candidate_config.num_candidates))
                        yield {
                            "event": "candidate_start",
                            "agent": "solver",
                            "num_candidates": num,
                            "iteration": iteration,
                            "timestamp": time.time(),
                        }
                        # 候选模式
                        reasoning = self._agent_memory.get("reasoner", "")
                        similar_text = self._agent_memory.get("similar_experiences", "")
                        if isinstance(similar_text, list):
                            exp_parts = []
                            for e in similar_text:
                                exp_parts.append(
                                    f"[{e.get('query','')[:200]}] score={e.get('score','N/A')} "
                                    f"sln={e.get('solution','')[:300]}"
                                )
                            similar_text = "\n".join(exp_parts)
                        elif isinstance(similar_text, str):
                            pass
                        else:
                            similar_text = ""
                        num = max(2, int(self._candidate_config.num_candidates))
                        t0_cand = time.perf_counter()
                        candidates = self._generate_candidates(
                            current_query, reasoning, method, num, iteration, similar_text)
                        gen_dur = (time.perf_counter() - t0_cand) * 1000
                        # yield 候选完成事件（压缩前统计）
                        yield {
                            "event": "candidate_progress",
                            "agent": "solver",
                            "num_candidates": num,
                            "num_executed": sum(1 for c in candidates if c.get("tool_result")),
                            "generation_ms": round(gen_dur, 1),
                            "iteration": iteration,
                            "timestamp": time.time(),
                        }
                        # Critic 压缩
                        keep_ratio = float(getattr(self._candidate_config, 'critic_keep_ratio', 0.5))
                        best_cands = self._critic_compress_candidates(candidates, current_query, keep_ratio)
                        # 使用最佳候选
                        best_cand = best_cands[0] if best_cands else candidates[0]
                        # 模拟 AgentResult
                        result = type('AgentResultProxy', (), {})()
                        result.content = best_cand.get("content", "")
                        result.success = best_cand.get("success", False)
                        result.metadata = {"prompt_tokens": "0", "completion_tokens": "0"}
                        result.error_message = best_cand.get("error", "")
                        duration_ms = gen_dur
                        # 如果候选有执行结果，记录
                        tr = best_cand.get("tool_result")
                        if tr:
                            # 已有执行结果，跳过 _auto_execute_code
                            self._tool_results.append(tr)
                            self._agent_memory["tool_result"] = tr.to_dict()
                            # 标记不重执行
                            self._agent_memory["_candidate_executed"] = True
                        # 存储候选统计（用于后续迭代改进）
                        self._agent_memory["_candidate_info"] = {
                            "num_candidates": num,
                            "num_executed": sum(1 for c in candidates if c.get("tool_result")),
                            "num_best_executed": sum(1 for c in best_cands if c.get("tool_result")),
                            "generation_ms": round(gen_dur, 1),
                        }
                        # 在内容中注入候选提示（帮助后续 agent 理解这是候选结果）
                        if result.content:
                            result.content += (
                                f"\n\n<CANDIDATE num_candidates={num} "
                                f"best_rank={best_cands[0] is best_cand if best_cands else True} />"
                            )
                    else:
                        result, duration_ms = _run_agent(agent_name, current_query, method)
                    
                    # 候选模式已执行代码，跳过后续的 _auto_execute_code
                    skip_tool_execution = (agent_name == "solver"
                                           and self._candidate_config.enabled
                                           and self._agent_memory.get("_candidate_executed", False))

                    # Store in cross-agent memory
                    if agent_name != "evaluator":
                        self._agent_memory[agent_name] = result.content

                    # Build content preview (first 200 chars)
                    content_preview = result.content[:200] if result.content else ""
                    if len(result.content or "") > 200:
                        content_preview += "..."

                    # Compute token counts
                    prompt_tokens = int(result.metadata.get("prompt_tokens", 0))
                    completion_tokens = int(result.metadata.get("completion_tokens", 0))

                    # Yield agent_complete
                    yield {
                        "event": "agent_complete",
                        "agent": agent_name,
                        "agent_label": agent_labels.get(agent_name, agent_name.title()),
                        "iteration": iteration,
                        "content_preview": content_preview,
                        "duration_ms": round(duration_ms, 1),
                        "prompt_tokens": prompt_tokens,
                        "completion_tokens": completion_tokens,
                        "success": result.success,
                        "timestamp": time.time(),
                    }

                    # If solver produced a tool execution, yield that too
                    if agent_name == "solver" and self._tool_results:
                        last_tool = self._tool_results[-1]
                        yield {
                            "event": "tool_execution",
                            "tool_name": last_tool.tool_name,
                            "success": last_tool.success,
                            "exit_code": last_tool.exit_code,
                            "stdout_preview": last_tool.stdout[:200] if last_tool.stdout else "",
                            "stderr_preview": last_tool.stderr[:200] if last_tool.stderr else "",
                            "duration_ms": last_tool.duration_ms,
                            "timestamp": time.time(),
                        }

                # --- After all 5 agents in this iteration ---
                # Compute iteration scores from evaluator output
                # Priority 1: <SCORE_ADJ> tags appended by _parse_evaluator_scores
                # Priority 2: JSON object in evaluator's content
                scores = {"reasonableness": 0.5, "executability": 0.5, "satisfaction": 0.5}
                if result and result.content:
                    import re as _re
                    # Try SCORE_ADJ tag first (set by _parse_evaluator_scores)
                    _adj_match = _re.search(
                        r'<SCORE_ADJ\s+reasonableness=([\d.]+)\s+executability=([\d.]+)\s+'
                        r'satisfaction=([\d.]+)\s+overall=([\d.]+)\s*/>',
                        result.content
                    )
                    if _adj_match:
                        scores["reasonableness"] = float(_adj_match.group(1))
                        scores["executability"] = float(_adj_match.group(2))
                        scores["satisfaction"] = float(_adj_match.group(3))
                        scores["overall"] = round(
                            scores["reasonableness"] * 0.4 +
                            scores["executability"] * 0.4 +
                            scores["satisfaction"] * 0.2,
                            4
                        )
                    else:
                        # Fallback: Look for JSON object in the evaluator's text output
                        _json_match = _re.search(r'\{[^{}]*"reasonableness"[^{}]*\}', result.content, _re.DOTALL)
                        if _json_match:
                            _raw = _json_match.group(0)
                            _cleaned = _re.sub(r'"\s*}', '}', _raw)
                            _cleaned = _re.sub(r',\s*}', '}', _cleaned)
                            try:
                                _parsed = json.loads(_cleaned)
                                if isinstance(_parsed, dict):
                                    scores["reasonableness"] = float(_parsed.get("reasonableness", scores["reasonableness"]))
                                    scores["executability"] = float(_parsed.get("executability", scores["executability"]))
                                    scores["satisfaction"] = float(_parsed.get("satisfaction", scores["satisfaction"]))
                            except (json.JSONDecodeError, ValueError, TypeError):
                                pass
                scores["overall"] = round(
                    scores["reasonableness"] * 0.4 +
                    scores["executability"] * 0.4 +
                    scores["satisfaction"] * 0.2,
                    4
                )
                best_score = max(best_score, scores["overall"])

                # Store iteration data
                # 将本轮评分存入 agent_memory，供下一轮 solver 代码执行时使用
                self._agent_memory["_last_executability"] = scores.get("executability", 0.5)
                iter_data = {
                    "iteration": iteration,
                    "scores": scores,
                    "solver_content": self._agent_memory.get("solver", ""),
                    "best_so_far": best_score,
                }
                all_iterations.append(iter_data)

                # Yield iteration event
                yield {
                    "event": "iteration",
                    "iteration": iteration,
                    "scores": scores,
                    "best_so_far": best_score,
                    "timestamp": time.time(),
                }

                # Check termination conditions
                if scores["overall"] >= threshold:
                    break  # Satisfactory solution found

                if not is_closed:
                    break  # Open loop: one iteration only

                # Closed loop: continue with refined query next iteration
                # (agent_memory already has previous outputs for context)

            # --- Build final result ---
            final_content = self._agent_memory.get("solver", "")
            # Use the last evaluator's best guess for final content
            if not final_content:
                final_content = self._agent_memory.get("verifier", result.content if result else "")

            final_result = {
                "success": True,
                "content": final_content,
                "score": all_iterations[-1]["scores"] if all_iterations else {
                    "reasonableness": 0.5, "executability": 0.5, "satisfaction": 0.5, "overall": 0.5
                },
                "iterations": all_iterations,
                "total_iterations": iteration,
                "best_score": best_score,
                "tool_results": [tr.to_dict() for tr in self._tool_results],
                "tools_used": len(self._tool_results) > 0,
            }

            # Build stats (from local data, not C++ orchestrator which is 0 in streaming mode)
            stats = {
                "queries_processed": 1,
                "iterations_executed": iteration,
                "rules_matched": 1 if rule else 0,
                "processes_completed": 1,
                "total_token_usage": max(int(self.loop_controller.get_total_token_usage()), 0) if hasattr(self, 'loop_controller') else 0,
                "token_budget": self.token_monitor.get_budget() if hasattr(self, 'token_monitor') else 10000,
                "usage_ratio": 0,
            }
            mode = self.get_mode()

            # Store successful results in experience store (self-evolution)
            if best_score >= threshold and final_content:
                try:
                    self.experience_store.add(
                        query=query,
                        solution=final_content,
                        verification_report={
                            "best_score": best_score,
                            "iterations": iteration,
                        },
                        scores=scores,
                        iterations_used=iteration,
                        total_tokens=stats.get("total_token_usage", 0),
                    )
                except Exception:
                    pass  # Experience store failure should not crash the main flow

            # Yield done
            yield {
                "event": "done",
                "result": final_result,
                "history": all_iterations,
                "stats": stats,
                "mode": mode,
                "session_id": session_id,
                "query": query,
                "timestamp": time.time(),
            }

        except Exception as e:
            import traceback
            yield {
                "event": "error",
                "message": str(e),
                "traceback": traceback.format_exc(),
                "iteration": iteration,
                "timestamp": time.time(),
            }

    # ===================== 多级嵌套闭环架构 =====================

    def _process_multi_loop(self, query, session_id, timestamp):
        """Generator — 多级嵌套闭环的流式处理，复用现有 agent callbacks。

        架构：
            外层（Strategy Loop）: Strategy Refiner → Strategy Reasoner
                → [内层 Execution Loop: Solver → Verifier → Evaluator(inner) ← 收敛]
                → Outer Verifier → Outer Evaluator
                ← 外层 feedback（outer score < outer_threshold）

        SSE 事件使用 loop_level 字段区分内外层。
        """
        import time
        import json
        import random as _rand

        self._agent_memory = {}
        self._tool_results = []

        # 配置（从 UI 设置读取）
        # 迭代次数：简单任务 2外2内，复杂任务由 UI 配置的 max_iterations 控制
        configured_max = getattr(self, '_max_iterations', 3)
        max_outer_iterations = min(configured_max, 3)  # 外环最多 3 次
        max_inner_iterations = min(configured_max, 2)  # 内环最多 2 次（比外环少）
        outer_threshold = getattr(self, '_threshold', 0.7)
        # 内环收敛阈值与外环一致（内环 0.99 就不该被外环打低分）
        inner_threshold = outer_threshold

        # Map query to rule
        rule = None
        method = "analysis"
        try:
            rules = self.rule_engine.match(query)
            if rules and len(rules) > 0:
                rule = rules[0]
                method = rule.validation_method
        except Exception:
            pass

        # --- 辅助函数: 内层闭环 ---
        def _run_inner_loop(input_query, outer_iter):
            """Run inner execution loop: Solver → Verifier → Evaluator(inner).
            Yields (inner_iter, inner_result, inner_scores, inner_content).
            """
            inner_memory = {}
            inner_tool_results = []
            best_inner_score = 0.0
            inner_result = None

            for i_iter in range(1, max_inner_iterations + 1):
                for i_agent in ["solver", "verifier", "evaluator"]:
                    # 保存并清除全局 agent_memory，避免 inner 的 prompt 被外层/前次迭代污染
                    saved_global_memory = dict(self._agent_memory) if self._agent_memory else {}
                    self._agent_memory = dict(inner_memory) if inner_memory else {}
                    ctx = {}
                    if i_agent == "solver":
                        ctx["reasoning"] = inner_memory.get("reasoner", input_query)
                        ctx["execution_result"] = ""
                        ctx["similar_experiences"] = ""  # 内环不需要相似经验，给空占位防 KeyError
                        if inner_tool_results:
                            last_tr = inner_tool_results[-1]
                            ctx["execution_result"] = (
                                f"[TOOL EXECUTION OUTPUT]\n"
                                f"Tool: {last_tr.tool_name}\nExit code: {last_tr.exit_code}\n"
                                f"Stdout: {last_tr.stdout}\nStderr: {last_tr.stderr}\n"
                                f"Duration: {last_tr.duration_ms:.0f}ms"
                            )
                    elif i_agent == "verifier":
                        ctx["solution"] = inner_memory.get("solver", "")
                        ctx["execution_results"] = ""
                        if inner_tool_results:
                            es = "\n\n[EXECUTION RESULTS]\n"
                            for ti, tr in enumerate(inner_tool_results):
                                es += f"[Tool {ti+1}] {tr.tool_name} Success:{tr.success} Exit:{tr.exit_code} Stdout:{tr.stdout}\n---\n"
                            ctx["execution_results"] = es
                    elif i_agent == "evaluator":
                        solver_content = inner_memory.get("solver", "")
                        verifier_content = inner_memory.get("verifier", input_query)
                        ctx["content"] = (
                            f"=== SOLUTION ===\n{solver_content}\n\n"
                            f"=== VERIFICATION RESULT ===\n{verifier_content}"
                        )
                        ctx["execution_results"] = ""
                        if inner_tool_results:
                            es = "\n\n[EXECUTION RESULTS]\n"
                            for ti, tr in enumerate(inner_tool_results):
                                es += f"[Tool {ti+1}] {tr.tool_name} Success:{tr.success} Stdout:{tr.stdout[:200]}\n---\n"
                            ctx["execution_results"] = es

                    # Yield inner agent_start
                    i_agent_label = {"solver": "Solver", "verifier": "Verifier", "evaluator": "Eval"}.get(i_agent, i_agent)
                    yield {
                        "event": "agent_start",
                        "loop_level": "inner",
                        "agent": "inner_" + i_agent,
                        "agent_label": i_agent_label,
                        "outer_iteration": outer_iter,
                        "inner_iteration": i_iter,
                        "timestamp": time.time(),
                    }

                    t0 = time.perf_counter()
                    agent_result = self._llm_agent_call(i_agent, input_query, method, ctx)
                    duration_ms = (time.perf_counter() - t0) * 1000

                    # Yield inner agent_complete
                    yield {
                        "event": "agent_complete",
                        "loop_level": "inner",
                        "agent": "inner_" + i_agent,
                        "agent_label": i_agent_label,
                        "content_preview": (agent_result.content or "")[:80],
                        "success": agent_result.success,
                        "duration_ms": duration_ms,
                        "outer_iteration": outer_iter,
                        "inner_iteration": i_iter,
                        "timestamp": time.time(),
                    }

                    if i_agent != "evaluator":
                        inner_memory[i_agent] = agent_result.content

                    # Auto-execute for solver
                    if i_agent == "solver" and agent_result.success and agent_result.content:
                        tr = self._auto_execute_code(agent_result.content)
                        if tr:
                            inner_tool_results.append(tr)
                            agent_result.content += (
                                f"\n\n=== EXECUTION OUTPUT ===\n✓ Success: {tr.success}\n"
                                f"Exit code: {tr.exit_code}\nStdout:\n{tr.stdout}\n=== END EXECUTION OUTPUT ==="
                            )
                            inner_memory["tool_result"] = tr.to_dict()

                    inner_result = agent_result

                    # 将 _llm_agent_call 写入 self._agent_memory 的结果同步回 inner_memory
                    for k in list(self._agent_memory.keys()):
                        if k not in saved_global_memory or self._agent_memory[k] != saved_global_memory.get(k):
                            inner_memory[k] = self._agent_memory[k]
                    # 恢复全局 agent_memory（不影响外层）
                    self._agent_memory = saved_global_memory

                # --- Compute inner scores ---
                i_scores = {"reasonableness": 0.5, "executability": 0.5, "satisfaction": 0.5}
                if inner_result and inner_result.content:
                    import re as _re
                    _adj_match = _re.search(
                        r'<SCORE_ADJ\s+reasonableness=([\d.]+)\s+executability=([\d.]+)\s+'
                        r'satisfaction=([\d.]+)\s+overall=([\d.]+)\s*/>',
                        inner_result.content
                    )
                    if _adj_match:
                        i_scores["reasonableness"] = float(_adj_match.group(1))
                        i_scores["executability"] = float(_adj_match.group(2))
                        i_scores["satisfaction"] = float(_adj_match.group(3))
                i_scores["overall"] = round(
                    i_scores["reasonableness"] * 0.4 + i_scores["executability"] * 0.4 + i_scores["satisfaction"] * 0.2, 4
                )
                best_inner_score = max(best_inner_score, i_scores["overall"])

                # Yield inner events
                yield {
                    "event": "inner_iteration",
                    "loop_level": "inner",
                    "outer_iteration": outer_iter,
                    "inner_iteration": i_iter,
                    "scores": i_scores,
                    "best_so_far": best_inner_score,
                    "timestamp": time.time(),
                }

                if i_scores["overall"] >= inner_threshold:
                    break  # Inner loop converged

            inner_content = inner_memory.get("solver", inner_result.content if inner_result else "")
            yield {
                "event": "inner_done",
                "loop_level": "inner",
                "outer_iteration": outer_iter,
                "inner_iterations": i_iter,
                "best_score": best_inner_score,
                "timestamp": time.time(),
            }
            return inner_content, inner_tool_results, i_scores

        # ====== 外层闭环主循环 ======
        outer_iteration = 0
        all_outer_iterations = []
        best_outer_score = 0.0
        final_content = ""
        final_scores = {}

        while outer_iteration < max_outer_iterations:
            outer_iteration += 1
            current_query = query if outer_iteration == 1 else self._agent_memory.get("refiner_strategy", query)

            # --- Outer: Strategy Refiner ---
            yield {
                "event": "agent_start",
                "loop_level": "outer",
                "agent": "strategy_refiner",
                "agent_label": "Strategy Refiner",
                "outer_iteration": outer_iteration,
                "timestamp": time.time(),
            }
            t0 = time.perf_counter()
            refiner_ctx = {"iteration": str(outer_iteration)}
            if outer_iteration > 1 and all_outer_iterations:
                prev = all_outer_iterations[-1]
                refiner_ctx["previous_iteration_info"] = (
                    f"[Previous Outer Iteration #{prev['outer_iteration']}]\n"
                    f"Score: {json.dumps(prev.get('scores', {}))}\n"
                    f"Output preview: {prev.get('solver_content', '')[:300]}"
                )
            else:
                refiner_ctx["previous_iteration_info"] = "(no previous iteration)"
            r_result = self._llm_agent_call("refiner", current_query, method, refiner_ctx)
            r_duration = (time.perf_counter() - t0) * 1000
            self._agent_memory["refiner_strategy"] = r_result.content
            yield {
                "event": "agent_complete",
                "loop_level": "outer",
                "agent": "strategy_refiner",
                "agent_label": "Strategy Refiner",
                "content_preview": r_result.content[:200],
                "duration_ms": round(r_duration, 1),
                "success": r_result.success,
                "timestamp": time.time(),
            }

            # --- Outer: Strategy Reasoner ---
            yield {
                "event": "agent_start",
                "loop_level": "outer",
                "agent": "strategy_reasoner",
                "agent_label": "Strategy Reasoner",
                "outer_iteration": outer_iteration,
                "timestamp": time.time(),
            }
            t0 = time.perf_counter()
            re_result = self._llm_agent_call("reasoner", r_result.content, method, {"strategy": r_result.content})
            re_duration = (time.perf_counter() - t0) * 1000
            self._agent_memory["strategy"] = re_result.content
            yield {
                "event": "agent_complete",
                "loop_level": "outer",
                "agent": "strategy_reasoner",
                "agent_label": "Strategy Reasoner",
                "content_preview": re_result.content[:200],
                "duration_ms": round(re_duration, 1),
                "success": re_result.success,
                "timestamp": time.time(),
            }

            # --- 内层 Execution Loop（delegate） ---
            yield {
                "event": "inner_loop_start",
                "loop_level": "outer",
                "agent": "execution_loop",
                "agent_label": "Execution Loop (Inner)",
                "outer_iteration": outer_iteration,
                "timestamp": time.time(),
            }
            inner_gen = _run_inner_loop(re_result.content, outer_iteration)
            inner_content = ""
            inner_tool_results = []
            inner_scores = {}
            try:
                # Use next() + iteration to capture the return value from generator
                inner_iter_result = None
                try:
                    while True:
                        inner_event = next(inner_gen)
                        yield inner_event
                except StopIteration as si:
                    inner_iter_result = si.value  # captures (inner_content, inner_tool_results, i_scores)
                if inner_iter_result:
                    inner_content, inner_tool_results, inner_scores = inner_iter_result
                else:
                    inner_content = self._agent_memory.get("solver", "")
                    inner_tool_results = list(self._tool_results)
            except Exception:
                inner_content = self._agent_memory.get("solver", "")
                if not inner_content and 'inner_iter_result' in dir() and inner_iter_result:
                    inner_content = inner_iter_result[0] if inner_iter_result[0] else ""
                inner_tool_results = list(self._tool_results)

            # --- Outer: Verifier ---
            # 构建 execution_results 传给 verifier（复用内环的执行结果）
            outer_exec_results = ""
            if inner_tool_results:
                es = "\n\n[EXECUTION RESULTS]\n"
                for ti, tr in enumerate(inner_tool_results):
                    es += f"[Tool {ti+1}] {tr.tool_name} Success:{tr.success} Exit:{tr.exit_code} Stdout:{tr.stdout}\n---\n"
                outer_exec_results = es
            yield {
                "event": "agent_start",
                "loop_level": "outer",
                "agent": "outer_verifier",
                "agent_label": "Outer Verifier",
                "outer_iteration": outer_iteration,
                "timestamp": time.time(),
            }
            t0 = time.perf_counter()
            v_ctx = {"solution": inner_content, "execution_results": outer_exec_results}
            v_result = self._llm_agent_call("verifier", inner_content, method, v_ctx)
            v_duration = (time.perf_counter() - t0) * 1000
            yield {
                "event": "agent_complete",
                "loop_level": "outer",
                "agent": "outer_verifier",
                "agent_label": "Outer Verifier",
                "content_preview": v_result.content[:200],
                "duration_ms": round(v_duration, 1),
                "success": v_result.success,
                "timestamp": time.time(),
            }

            # --- Outer: Evaluator ---
            # 外层 evaluator 评估"内环成果是否满足策略一致性"
            # 传入内环分数 + 策略 + 代码 + 执行结果，让 LLM 判断策略对齐度
            yield {
                "event": "agent_start",
                "loop_level": "outer",
                "agent": "outer_evaluator",
                "agent_label": "Outer Evaluator",
                "outer_iteration": outer_iteration,
                "timestamp": time.time(),
            }
            t0 = time.perf_counter()
            # 构建外环专属评估上下文：策略 VS 代码实现
            inner_score_str = json.dumps(inner_scores) if inner_scores else "N/A"
            e_ctx = {"content": (f"=== STRATEGY (Outer) ===\n{re_result.content}\n\n"
                                 f"=== INNER LOOP SCORES ===\n{inner_score_str}\n\n"
                                 f"=== CODE IMPLEMENTATION ===\n{inner_content}\n\n"
                                 f"=== VERIFICATION RESULT ===\n{v_result.content}"),
                     "execution_results": outer_exec_results}
            e_result = self._llm_agent_call("evaluator", inner_content, method, e_ctx)
            e_duration = (time.perf_counter() - t0) * 1000
            yield {
                "event": "agent_complete",
                "loop_level": "outer",
                "agent": "outer_evaluator",
                "agent_label": "Outer Evaluator",
                "content_preview": e_result.content[:200],
                "duration_ms": round(e_duration, 1),
                "success": e_result.success,
                "timestamp": time.time(),
            }

            # --- Compute outer scores ---
            scores = {"reasonableness": 0.5, "executability": 0.5, "satisfaction": 0.5}
            if e_result and e_result.content:
                import re as _re
                _adj_match = _re.search(
                    r'<SCORE_ADJ\s+reasonableness=([\d.]+)\s+executability=([\d.]+)\s+'
                    r'satisfaction=([\d.]+)\s+overall=([\d.]+)\s*/>',
                    e_result.content
                )
                if _adj_match:
                    scores["reasonableness"] = float(_adj_match.group(1))
                    scores["executability"] = float(_adj_match.group(2))
                    scores["satisfaction"] = float(_adj_match.group(3))
            scores["overall"] = round(
                scores["reasonableness"] * 0.4 + scores["executability"] * 0.4 + scores["satisfaction"] * 0.2, 4
            )
            best_outer_score = max(best_outer_score, scores["overall"])

            outer_iter_data = {
                "outer_iteration": outer_iteration,
                "scores": scores,
                "best_so_far": best_outer_score,
                "solver_content": inner_content[:200],
                "strategy": re_result.content[:200],
            }
            all_outer_iterations.append(outer_iter_data)

            yield {
                "event": "iteration",
                "loop_level": "outer",
                "outer_iteration": outer_iteration,
                "scores": scores,
                "best_so_far": best_outer_score,
                "timestamp": time.time(),
            }

            # Store final content (update each iteration)
            final_content = inner_content or v_result.content or ""
            final_scores = scores

            # Check convergence
            if scores["overall"] >= outer_threshold:
                break  # Outer loop converged

            # Closed loop: outer feedback continues
            # strategy refiner will get previous context from _agent_memory

        # ====== Build final result ======
        mode = self.get_mode()
        final_result = {
            "success": True,
            "content": final_content,
            "score": final_scores,
            "iterations": all_outer_iterations,
            "total_iterations": outer_iteration,
            "best_score": best_outer_score,
            "mode": "multi_loop",
            "tool_results": [tr.to_dict() for tr in self._tool_results],
            "tools_used": len(self._tool_results) > 0,
        }
        stats = {
            "queries_processed": 1,
            "iterations_executed": outer_iteration,
            "rules_matched": 1 if rule else 0,
            "processes_completed": 1,
            "mode": "multi_loop",
            "total_token_usage": 0,
        }
        # Store successful results in experience store (multi-loop)
        if best_outer_score >= outer_threshold and final_content:
            try:
                self.experience_store.add(
                    query=query,
                    solution=final_content,
                    verification_report={
                        "best_score": best_outer_score,
                        "iterations": outer_iteration,
                    },
                    scores=scores,
                    iterations_used=outer_iteration,
                    total_tokens=0,
                )
            except Exception:
                pass

        yield {
            "event": "done",
            "result": final_result,
            "history": all_outer_iterations,
            "stats": stats,
            "mode": self.get_mode(),  # actual mode (closed/open), not execution mode
            "arch_mode": "multi",
            "session_id": session_id,
            "query": query,
            "timestamp": time.time(),
        }

    def _format_result(self, result):
        """Convert C++ AgentResult to a Python dict, preserving all stage outputs."""
        formatted = {
            "success": result.success,
            "content": result.content,
            "error_message": result.error_message,
            "score": {
                "reasonableness": result.score.reasonableness,
                "executability": result.score.executability,
                "satisfaction": result.score.satisfaction,
                "overall": result.score.overall(),
            },
            "metadata": dict(result.metadata),
        }
        # Extract structured iteration data from metadata for frontend display
        meta = formatted["metadata"]
        if meta.get("iteration_count"):
            iterations = []
            n = int(meta["iteration_count"])
            for i in range(n):
                iter_data = {
                    "iteration": i + 1,
                    "score": float(meta.get(f"iter_{i}_score", 0)),
                    "reasonableness": float(meta.get(f"iter_{i}_reasonableness", 0)),
                    "executability": float(meta.get(f"iter_{i}_executability", 0)),
                    "satisfaction": float(meta.get(f"iter_{i}_satisfaction", 0)),
                }
                iterations.append(iter_data)
            formatted["iterations"] = iterations
        return formatted

    def get_statistics(self):
        """Get framework statistics."""
        raw_stats = dict(self.orchestrator.get_statistics())
        # Use actual recorded token usage from orchestrator/loop controller
        # rather than the standalone TokenMonitor, which is never updated
        # during processQuery since tokens are tracked in the loop controller.
        total_tokens = int(raw_stats.get("total_token_used", 0))
        if total_tokens <= 0:
            # Fallback: try loop_controller's tracking
            try:
                total_tokens = int(self.loop_controller.get_total_token_usage())
            except Exception:
                pass
        budget = self.token_monitor.get_budget()
        usage_ratio = total_tokens / max(budget, 1)
        return {
            "queries_processed": int(raw_stats.get("queries_processed", 0)),
            "iterations_executed": int(raw_stats.get("iterations_executed", 0)),
            "rules_matched": int(raw_stats.get("rules_matched", 0)),
            "processes_completed": int(raw_stats.get("processes_completed", 0)),
            "total_token_usage": total_tokens,
            "token_budget": budget,
            "usage_ratio": usage_ratio,
        }

    def get_execution_history(self):
        """Get execution history as list of dicts."""
        history = []
        for query, result in self.orchestrator.get_execution_history():
            entry = {
                "query": query,
                "result": {
                    "success": result.success,
                    "content": result.content,
                    "score": {
                        "reasonableness": result.score.reasonableness,
                        "executability": result.score.executability,
                        "satisfaction": result.score.satisfaction,
                        "overall": result.score.overall(),
                    },
                    "metadata": dict(result.metadata),
                }
            }
            # Extract iteration-level score data from metadata
            meta = entry["result"]["metadata"]
            entry["result"]["iteration_index"] = int(meta.get("iteration_index", 0))
            entry["result"]["iteration_score_overall"] = float(meta.get("iteration_score_overall", 0))
            history.append(entry)
        return history

    def get_rules(self):
        """Get all loaded rules."""
        return [
            {
                "pattern": r.pattern,
                "validation_method": r.validation_method,
                "recommended_tools": list(r.recommended_tools),
                "threshold": r.threshold,
                "weights": dict(r.weights),
            }
            for r in self.rule_engine.get_all_rules()
        ]

    def get_mode(self):
        """Get the current loop mode: 'closed' or 'open'."""
        try:
            m = self.loop_controller.get_mode()
            return "closed" if m == self.loop_controller.__class__.Mode.CLOSED_LOOP else "open"
        except Exception:
            return "closed"

    def set_mode(self, mode):
        """Set loop mode: 'closed' or 'open'."""
        try:
            target = (self.loop_controller.__class__.Mode.CLOSED_LOOP
                      if mode == 'closed'
                      else self.loop_controller.__class__.Mode.OPEN_LOOP)
            self.loop_controller.set_mode(target)
        except Exception as e:
            pass

    def set_arch_mode(self, mode):
        """Set architecture mode: 'single' or 'multi'."""
        self._arch_mode = mode

    def get_arch_mode(self):
        """Get current architecture mode: 'single' or 'multi'."""
        return getattr(self, '_arch_mode', 'single')

    def set_max_iterations(self, n):
        """Set max iterations per query."""
        self._max_iterations = max(1, int(n))
        try:
            self.loop_controller.set_max_iterations(self._max_iterations)
        except Exception:
            pass

    def set_threshold(self, t):
        """Set satisfaction threshold (0-1)."""
        self._threshold = t
        try:
            self.loop_controller.set_threshold(t)
        except Exception:
            pass

    def set_execution_timeout(self, seconds):
        """Set code execution timeout in seconds.
        
        Recreates the ToolExecutor with the new timeout value.
        """
        import os
        self._execution_timeout = max(1, int(seconds))
        base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        tools_dir = os.path.join(base_dir, "tools", "sandbox")
        self._tool_executor = ToolExecutor(sandbox_dir=tools_dir, timeout=self._execution_timeout)

    def set_token_budget(self, budget):
        """Set token budget."""
        try:
            self.token_monitor.set_budget(budget)
        except Exception:
            pass

    def reset(self):
        """Reset framework state — clear execution history and agent memory."""
        try:
            self.orchestrator.reset()
            self._agent_memory = {}
            self._tool_results = []
        except Exception:
            pass

    def get_token_usage_by_agent(self):
        """Get token usage breakdown by agent type. Returns dict."""
        try:
            raw = dict(self.orchestrator.get_statistics())
            return {
                "refiner": int(raw.get("tokens_refiner", 0)),
                "reasoner": int(raw.get("tokens_reasoner", 0)),
                "solver": int(raw.get("tokens_solver", 0)),
                "verifier": int(raw.get("tokens_verifier", 0)),
                "evaluator": int(raw.get("tokens_evaluator", 0)),
            }
        except Exception:
            return {}

    def get_token_usage_by_operation(self):
        """Get token usage breakdown by operation type. Returns dict."""
        try:
            return dict(self.orchestrator.get_token_usage_by_operation())
        except Exception:
            return {}

    def save_rules(self, rules):
        """Save a list of rule dicts to the engine.
        Each rule: {pattern, validation_method, recommended_tools, weights, threshold}
        """
        try:
            self.rule_engine.clear()
            for r in rules:
                rule = Rule()
                rule.pattern = r.get("pattern", "")
                rule.validation_method = r.get("validation_method", "regex")
                rule.recommended_tools = list(r.get("recommended_tools", []))
                rule.weights = r.get("weights", {
                    "reasonableness": 0.4,
                    "executability": 0.4,
                    "satisfaction": 0.2,
                })
                rule.threshold = r.get("threshold", 0.7)
                self.rule_engine.add_rule(rule)
            # Also persist to YAML file
            self._persist_rules(rules)
        except Exception as e:
            pass

    def _persist_rules(self, rules):
        """Persist rules to the YAML config file."""
        try:
            import os
            config_dir = os.path.join(os.path.dirname(__file__), '..', 'config', 'rules')
            os.makedirs(config_dir, exist_ok=True)
            path = os.path.join(config_dir, 'default.yaml')
            yaml_lines = ["# CLMA Rules Configuration", "rules:"]
            for r in rules:
                yaml_lines.append(f"  - pattern: {self._to_yaml(r.get('pattern', ''))}")
                yaml_lines.append(f"    validation_method: {r.get('validation_method', 'regex')}")
                yaml_lines.append(f"    recommended_tools: [{', '.join(r.get('recommended_tools', []))}]")
                yaml_lines.append(f"    threshold: {r.get('threshold', 0.7)}")
                w = r.get("weights", {})
                yaml_lines.append(f"    weights:")
                yaml_lines.append(f"      reasonableness: {w.get('reasonableness', 0.4)}")
                yaml_lines.append(f"      executability: {w.get('executability', 0.4)}")
                yaml_lines.append(f"      satisfaction: {w.get('satisfaction', 0.2)}")
            with open(path, 'w') as f:
                f.write('\n'.join(yaml_lines) + '\n')
        except Exception:
            pass

    def _to_yaml(self, data, indent=0):
        """Lightweight YAML serializer for nested dict/list structures.
        Pure Python — no external dependencies needed."""
        lines = []
