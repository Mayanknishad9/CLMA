# CLMA — Closed-Loop Multi-Agent Framework

> A self-verifying code generation system that combines C++17 orchestration with Python-based multi-agent pipelines and a real-time Web UI. Automates the validation loop — no human-in-the-middle required.

[![Python 3.10+](https://img.shields.io/badge/Python-3.10%2B-blue)](https://python.org)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-green)](https://isocpp.org)
[![Build](https://img.shields.io/badge/build-CMake%203.15%2B-orange)](https://cmake.org)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## Table of Contents

- [Motivation](#motivation)
- [Architecture Overview](#architecture-overview)
- [Execution Modes](#execution-modes)
- [Benchmarks](#benchmarks)
- [Quick Start](#quick-start)
- [LLM Provider Configuration](#llm-provider-configuration)
- [Project Structure](#project-structure)
- [Web UI Reference](#web-ui-reference)
- [Python API Reference](#python-api-reference)
- [Development Guide](#development-guide)
- [FAQ](#faq)
- [License](#license)

---

## Motivation

Large language models excel at generating plausible code, but generated code frequently fails at execution time. The traditional workflow — the user copies error messages back to the model, the model produces a revised version, and the cycle repeats — is fundamentally **a manual feedback loop**.

CLMA **embeds this verification loop into the framework itself**. Given a natural language requirement, the system:

1. **Refines** ambiguous requirements into structured specifications
2. **Reasons** about algorithmic choices, edge cases, and constraints
3. **Generates** executable code
4. **Verifies** correctness through execution and rule-based checks
5. **Evaluates** output quality across three dimensions
6. **Iterates** automatically when quality falls below threshold — no manual intervention required

The result: LLM-generated code that converges to a verifiably correct solution without human oversight.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Web UI (Flask + SSE)                          │
│    Dark theme · Real-time SVG flow graph · Score dashboard      │
│    Pan/zoom · Mode selector · Session history · LLM catalog     │
└──────────────────────────┬──────────────────────────────────────┘
                           │ HTTP / Server-Sent Events
┌──────────────────────────▼──────────────────────────────────────┐
│               Python Interface Layer (pybind11)                   │
│    Config management · API adapters · Tool executors             │
│    Scoring engine · Iteration controller · Experience store      │
└──────────────────────────┬──────────────────────────────────────┘
                           │ pybind11 bindings
┌──────────────────────────▼──────────────────────────────────────┐
│                   C++17 Core Engine                              │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐             │
│  │ Orchestrator │ │ Rule Engine  │ │Token Monitor │ LoopCtrl    │
│  └──────────────┘ └──────────────┘ └──────────────┘             │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐             │
│  │PluginManager │ │DAG Processor │ │   Sandbox    │             │
│  └──────────────┘ └──────────────┘ └──────────────┘             │
└─────────────────────────────────────────────────────────────────┘
```

### Core Modules

| Module | Language | Responsibility |
|--------|----------|----------------|
| **Orchestrator** | C++17 | Central scheduler; coordinates agent execution and iteration flow |
| **DAG Processor** | C++17 | Directed acyclic graph task decomposition with parallel dispatch |
| **RuleEngine** | C++17 + YAML | Regex/keyword rule matching; configurable validation methods |
| **TokenMonitor** | C++17 | Token consumption tracking with budget-aware preemption |
| **LoopController** | C++17 | Iteration limit enforcement and convergence detection |
| **PluginManager** | C++17 | Dynamic `.so` hot-loading for extensible agent plugins |
| **Sandbox** | Python + subprocess | Isolated code execution environment with timeout enforcement |
| **Framework** | Python | Agent prompt management, scoring pipeline, context maintenance |

### Agent Roles

| Agent | Input | Output | Pluggable |
|-------|-------|--------|-----------|
| **Refiner** | Raw user query | Structured task specification with extracted constraints | ✅ |
| **Reasoner** | Refined query | Solution steps, algorithm selection, edge case analysis | ✅ |
| **Solver** | Reasoning + execution feedback | Executable code (Python/Bash/C++/JS/Go...) | ✅ |
| **Verifier** | Code + execution results | JSON verdict: hard checks, soft checks, pass/fail | ✅ |
| **Evaluator** | Verification results + execution output | JSON scores: reasonableness, executability, satisfaction | ✅ |

---

## Execution Modes

CLMA selects the optimal execution strategy based on task complexity through automatic classification:

```
query entry
  │
  ├── Simple ("print Hello World" / "compute 1+1")
  │     └── 🚀 Fast Path ~2s
  │               Direct solver → auto-execute → score from results
  │
  ├── Moderate ("implement binary search" / "write fibonacci")
  │     └── 🔄 Single Closed-Loop ~5s
  │              Refiner → Reasoner → Solver → Verifier → Evaluator ← score feedback
  │
  └── Complex ("design microservice architecture with API gateway, service discovery, circuit breaker")
        └── 🔁 Nested Multi-Loop ~40s
               ┌─ Outer Loop: Strategy Refiner → Strategy Reasoner
               │           ↓
               │      Inner Loop: [Solver → Verifier → Evaluator] (convergence)
               │           ↓
               │      Outer Verifier → Outer Evaluator (strategy alignment)
               └── Outer score below threshold → strategy refinement → re-execute inner loop
```

### Fast Path (DAG Fast Track)

Extremely simple tasks bypass the full pipeline planner overhead:

```python
# Trigger: query length ≤ 60 characters + code keyword match
# Suppressed by: algorithm keywords (sort, search, recursion, etc.)
"print 1 to 100"              → Fast Path ✓
"implement fibonacci in Python" → Fast Path ✗ (contains "fibonacci")
```

### DAG Mode

Complex multi-component tasks are decomposed by the C++ DAG Processor into independent sub-tasks. Each sub-task executes through its own closed-loop verification pipeline; results are aggregated upon completion.

### Nested Multi-Loop

- **Outer Loop (Strategy Loop)**: Defines the architectural strategy and validates strategy alignment
- **Inner Loop (Execution Loop)**: Generates code, verifies correctness, evaluates code quality
- **Convergence Criteria**: Iteration terminates when score ≥ threshold — does not exhaust iteration budget unnecessarily

---

## Benchmarks

Measured against **DeepSeek API** (single LLM call latency ~5-8s):

| Task | Fast Path | Single Loop | DAG | Nested Loop |
|------|-----------|-------------|-----|-------------|
| Hello World | **2.3s** / 0.97 | 4.7s / 0.99 | 5.3s / 0.99 | — |
| Fibonacci | — | **4.7s** / 0.99 | 5.3s / 0.99 | — |
| Quicksort | — | **5.1s** / 0.98 | — | — |
| Batch file rename | — | **8.1s** / 0.98 | 12.0s / 0.97 | — |
| Microservice architecture | — | — | — | **39s** / 1.0 ✅ |
| Multi-component project | — | — | **~25s** / 0.97 | — |

### Mode Selection Guidelines

| Scenario | Recommended Mode | Rationale |
|----------|-----------------|-----------|
| One-liner, simple command, basic calculation | **Fast Path** | Minimal overhead, ~2s response |
| Single function, single file, well-defined requirements | **Single Loop** | Best latency/quality trade-off, ~5s |
| Multiple independent modules, parallelizable work | **DAG** | Modules verified independently, aggregated results |
| Complex architecture, strategic constraints | **Nested Loop** | Only viable approach when top-level planning is required |

> **Note on Nested Loop Performance**: At 39s total (7 real LLM calls), the nested loop is the slowest mode by wall-clock time, but it is the **only** mode that reliably produces correct output for strategical tasks. Pre-nested-loop benchmarks showed 0.49 scores with non-convergent behavior — the current iteration converges at 1.0 in a single outer pass.

---

## Quick Start

### Prerequisites

```bash
# Required
gcc/g++ 9+ or clang 12+
cmake 3.15+
python 3.10+
yaml-cpp      # Ubuntu: apt install libyaml-cpp-dev
sqlite3       # Usually pre-installed
pybind11      # pip install pybind11

# Optional
docker        # Containerized code execution
```

### One-Step Launch (Recommended)

```bash
git clone https://github.com/yourname/clma.git
cd clma

# 1. Create virtual environment
python3 -m venv venv
source venv/bin/activate
pip install flask flask-cors pybind11

# 2. Build C++ core
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# 3. Configure API (edit config/api_config.json with your key)
#    See "LLM Provider Configuration" below

# 4. Launch Web UI
./run_webui.sh
# → Open http://localhost:5000 in your browser
```

### Step-by-Step Build

**Build C++ Core Engine:**

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run C++ unit tests (62 test cases)
ctest -j$(nproc)
```

**Build Python Bindings + Web UI:**

```bash
pip install flask flask-cors pybind11

cd build
cmake .. -DBUILD_PYTHON_BINDINGS=ON
make -j$(nproc) clma_core
# The generated .so file is automatically copied to python_interface/

cd ..
./run_webui.sh
```

**Run Python Tests (verify all logic):**

```bash
source venv/bin/activate
cd tests && python3 -m pytest test_core_python.py -v

# Expected: 46 passed
```

### China-Specific Setup

Users in mainland China may experience API connectivity issues. We recommend:

1. **DeepSeek API** (recommended for China users): Set up `config/api_config.json` with `provider: "deepseek"` and your DeepSeek API key
2. **Local models**: Configure `provider: "local"` with a Ollama/vLLM endpoint running on the same machine

See [LLM Provider Configuration](#llm-provider-configuration) for details.

---

## LLM Provider Configuration

CLMA supports five LLM providers through a unified adapter pattern. All provider-specific logic is encapsulated behind the `BaseProvider` interface; the framework is provider-agnostic.

### Method 1: Direct Configuration

Edit `config/api_config.json`:

```json
{
  "provider": "deepseek",
  "api_key": "sk-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
  "base_url": "https://api.deepseek.com/v1",
  "model": "deepseek-chat",
  "max_tokens": 8192,
  "temperature": 0.7,
  "enabled": true
}
```

### Method 2: Web UI Configuration

Navigate to `http://localhost:5000` → API configuration button (top-right) → Select provider → Enter API key → Test connection.

### Supported Providers

| Provider | Config Value | Default Model | Notes |
|----------|-------------|---------------|-------|
| **OpenAI** | `openai` | gpt-4o | General-purpose, best overall quality |
| **Anthropic** | `anthropic` | claude-sonnet-4 | Strong code comprehension |
| **DeepSeek** | `deepseek` | deepseek-chat | Cost-effective, China-friendly |
| **Gemini** | `gemini` | gemini-2.0-flash | Free tier available |
| **Local** | `local` | Custom | Ollama/vLLM compatible, fully offline |

**Configuration file:** `config/api_config.json`
**LLM catalog (auto-updated):** `config/llm_catalog.json`

> CLMA includes an automated LLM catalog updater (`scripts/auto_update_providers.py`) that syncs the latest model lists from each provider every 72 hours.

### API Key Security

- API keys are stored **locally** in `config/api_config.json`
- The framework makes **no outbound telemetry calls** — all LLM traffic goes directly to the configured provider
- `config/api_config.json` is excluded from version control via `.gitignore`

---

## Project Structure

```
clma/
├── src/                    # C++ core engine
│   ├── core/               # Core module implementations
│   │   ├── Orchestrator.cpp    # Central scheduler
│   │   ├── RuleEngine.cpp      # Rule matching engine
│   │   ├── TokenMonitor.cpp    # Token consumption tracking
│   │   ├── LoopController.cpp  # Iteration control
│   │   ├── PluginManager.cpp   # Plugin lifecycle management
│   │   ├── PluginWatcher.cpp   # File-system hot-reload watcher
│   │   ├── Sandbox.cpp         # Sandbox execution
│   │   └── Types.cpp           # Type definitions
│   ├── agents/             # Agent plugin interface (C++ plugins)
│   └── main.cpp            # CLI entry point (optional)
├── include/core/           # C++ headers
├── plugins/                # Agent plugins (.so)
│   ├── agent_refiner/
│   ├── agent_reasoner/
│   ├── agent_solver/
│   ├── agent_verifier/
│   └── agent_evaluator/
├── python_interface/       # Python interface layer
│   ├── core.py             # Framework logic + agent prompts (~2400 LOC)
│   ├── web_app.py          # Flask web application
│   ├── api_providers.py    # 5 LLM provider adapters
│   ├── tool_executor.py    # Sandbox code execution
│   ├── experience_store.py # Experience storage/retrieval
│   ├── session_store.py    # Session persistence
│   └── templates/          # HTML templates
├── tests/                  # Test suites (Google Test + pytest)
│   ├── test_core_python.py # 46 Python unit tests
│   ├── test_*.cpp          # 62 C++ unit tests
│   └── CMakeLists.txt
├── config/                 # Configuration files
│   ├── api_config.json     # LLM provider configuration
│   ├── llm_catalog.json    # LLM model catalog
│   ├── rules/default.yaml  # Rule definitions
│   └── sessions/           # Historical sessions (JSON)
├── docs/                   # Design documents
├── run_webui.sh            # One-click launch script
└── CMakeLists.txt          # Top-level build configuration
```

---

## Web UI Reference

### Interface Components

| Component | Location | Description |
|-----------|----------|-------------|
| **Query Input** | Central text area | Enter requirements; submit via Enter |
| **Architecture Selector** | Top button group | Single / DAG / Multi-Loop mode toggle |
| **Mode Toggle** | Settings panel | Closed (iterative) / Open (single-pass) |
| **Settings Panel** | Gear icon (top-right) | Iteration count, threshold, timeout, token budget |
| **Real-time Flow Graph** | Left panel | SVG flow diagram; nodes transition green/red dynamically |
| **Score Dashboard** | Right panel | Three scoring dimensions updated in real-time + gauge visualization |
| **Execution Timeline** | Bottom panel | Waterfall chart showing per-agent latency |
| **Session History** | Left menu | Historical queries with search and replay |
| **API Configuration** | Top-right API button | Provider switch and connection test |
| **Plugin Management** | Plugin page | View, load, and unload agent plugins |

### Settings Reference

| Parameter | Default | Description |
|-----------|---------|-------------|
| `max_iterations` | 10 | Maximum iterations before forced termination |
| `threshold` | 0.3 | Convergence threshold (iteration stops when score ≥ this value) |
| `execution_timeout` | 120 | Code execution timeout in seconds |
| `mode` | `closed` | `closed` (iterative) / `open` (single-pass) |
| `arch_mode` | `single` | `single` / `dag` / `multi` |

---

## Python API Reference

```python
from core import CLMAFramework

fw = CLMAFramework(
    mode="closed",        # "closed" | "open"
    max_iterations=5,
    threshold=0.7,
    token_budget=10000,
)

# Synchronous query (blocking)
result = fw.process_query("implement quicksort in Python")
print(f"Score: {result['score']['overall']}")
print(f"Code:\n{result['content']}")

# Streaming query (SSE events)
for event in fw.process_query_stream("design a REST API route"):
    if event["event"] == "agent_start":
        print(f"[{event['agent']}] processing...")
    elif event["event"] == "agent_complete":
        print(f"[{event['agent']}] completed ({event['duration_ms']:.0f}ms)")
    elif event["event"] == "iteration":
        print(f"[Iteration {event['iteration']}] score: {event['scores']['overall']:.3f}")
    elif event["event"] == "done":
        print(f"Final result (score: {event['result']['score']['overall']:.3f})")
```

---

## Development Guide

### Adding Rules

Edit `config/rules/default.yaml`:

```yaml
rules:
  - pattern: "deploy|build|run"
    validation_method: "execution"
    recommended_tools: ["docker", "shell"]
    weights:
      reasonableness: 0.3
      executability: 0.5
      satisfaction: 0.2
    threshold: 0.3
```

### Adding a New LLM Provider

Register in `python_interface/api_providers.py`:

```python
@register_provider("my_provider")
class MyProvider(BaseProvider):
    """Custom LLM provider implementation."""
    
    def create_completion(self, messages, **kwargs):
        # Implement your API call logic
        ...
```

### Adding a New Agent Plugin

```cpp
// plugins/my_agent/plugin.cpp
#include <core/PluginInterface.hpp>

class MyAgent : public AgentPlugin {
public:
    AgentResult execute(const Context& ctx) override {
        // Your agent logic
        return result;
    }
};

extern "C" AgentPlugin* create_plugin() {
    return new MyAgent();
}
```

### Running All Tests

```bash
# C++ tests
cd build && ctest -j$(nproc)

# Python tests
source venv/bin/activate
python3 -m pytest tests/test_core_python.py -v
python3 -m pytest tests/test_dag_planner.py -v
python3 -m pytest tests/test_sandbox_tiering.py -v

# Total: 108+ test cases
```

---

## FAQ

**Q: How does CLMA differ from LangChain or CrewAI?**

A: LangChain provides chainable LLM calls; CrewAI focuses on role-based agent orchestration. CLMA's key differentiator is its **closed-loop feedback mechanism** — rather than executing agents in a fixed sequence, each iteration passes through Verifier + Evaluator stages that measure output quality. Results below threshold automatically trigger another iteration until convergence. This transforms LLM code generation from a "generate and hope" model to a "generate, verify, and iterate" one.

**Q: Why use nested loops when they are slower than single loops?**

A: Nested loops are not intended for simple tasks — the mode selector routes those to Fast Path or Single Loop automatically. Nested loops exist for **strategically complex tasks** where the outer strategy loop and inner execution loop must align. A single-pass pipeline cannot validate architectural decisions against generated code. Analogous to why distributed systems use consensus protocols for critical state transitions: the overhead is justified by correctness guarantees.

**Q: Which programming languages are supported?**

A: The execution layer (Sandbox) supports Python, Bash, and C++ natively. The LLM Solver can generate code in any language. Additional runtime language support can be added through ToolExecutor extensions.

**Q: Is the framework usable offline?**

A: Yes. Configure `provider: "local"` and point `base_url` to your Ollama or vLLM endpoint. All components function without internet access when using a local model.

**Q: Can I try CLMA without an API key?**

A: Yes. The framework includes a **simulated fallback mode** — when LLM calls fail or API configuration is absent, calls are automatically degraded to rule-based simulated agents. Scoring and pipeline mechanics remain fully functional, though code quality is naturally lower than with a real LLM.

**Q: Does CLMA support Chinese-language queries?**

A: Yes. All agent prompts are language-agnostic — Chinese, English, and other natural language queries are processed through the same pipeline. The classification router correctly handles Chinese keywords (e.g., "用 Python" → code query).

---

## License

MIT License

---

## Support

If you find this project useful:
- ⭐ Star the repository
- 🐛 [Submit an issue](https://github.com/yourname/clma/issues) for bugs
- 💡 Pull requests and feature suggestions are welcome
