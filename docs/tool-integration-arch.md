# 工具集成架构设计
## 闭环多智能体推理框架 · 工具层

## 1. 问题域分析

### 1.1 当前局限

Agent 流程现有状态：
```
用户查询 → [Refiner LLM] → [Reasoner LLM] → [Solver LLM] → [Verifier LLM] → [Evaluator LLM] → 结果
                                                                                           ↓
                                                                                    只有文本评价，
                                                                                    没有实际执行验证
```

Solver 只能输出文本（如代码片段），无法真正运行/编译/测试。
Verifier 只能通过 LLM "思考"验证，无法实际执行代码检查行为。

### 1.2 目标

引入工具执行能力，让 Agent 能：
- 执行生成的代码（Python/Shell/C++）
- 在隔离环境中编译和运行
- 读取/写入文件
- 使用 Docker 容器化执行
- 将工具执行结果作为后续 Agent 的上下文

## 2. 系统架构

### 2.1 工具抽象层

```
┌─────────────────────────────────────────────────────────┐
│                    ToolExecutor                           │
│  ┌────────────────────────────────────────────────────┐  │
│  │  execute_code(code, lang) → {stdout, stderr, rc}  │  │
│  │  execute_command(cmd, cwd) → {stdout, stderr, rc} │  │
│  │  read_file(path) → content                        │  │
│  │  write_file(path, content) → ok                   │  │
│  │  docker_run(image, cmd) → {output, logs}          │  │
│  │  get_available_tools() → [tool names]             │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  运行时策略：                                              │
│  ┌──────────────────┐  ┌──────────────────┐             │
│  │  LocalExecutor   │  │  DockerExecutor  │             │
│  │  (直接执行/沙箱)  │  │  (容器化隔离)     │             │
│  └──────────────────┘  └──────────────────┘             │
│         ↓                       ↓                       │
│  subprocess / tempfile    docker sdk / subprocess       │
└─────────────────────────────────────────────────────────┘
```

### 2.2 与 Agent 流水线的集成

```
用户查询 → Refiner → Reasoner → Solver ──→ Verifier ──→ Evaluator
                                    │           │
                                    ↓           ↓
                              ToolExecutor  ToolExecutor
                                    │           │
                                    ↓           ↓
                              代码执行结果  验证执行结果
                                    │           │
                                    ↓           ↓
                           ┌───────────────────────┐
                           │  Agent 会话上下文       │
                           │  {solver_output,      │
                           │   tool_results: [     │
                           │     {tool, input,     │
                           │      output, rc}      │
                           │   ]}                  │
                           └───────────────────────┘
```

### 2.3 数据流

```
Solver阶段（在Python回调中，非C++）:
  1. LLM 生成代码
  2. Python 回调检查输出 → 如果是代码 → 调用 ToolExecutor
  3. 执行结果注入到下一个 Solver 调用（迭代时）
  4. 执行结果也注入到 Verifier 的上下文中
  
Verifier阶段:
  1. 接收 Solver 输出 + 工具执行结果
  2. 可选：重新执行代码对比行为是否一致
  3. 基于实际输出（而非仅文本分析）做验证判断
```

## 3. 模块设计

### 3.1 ToolExecutor (python_interface/tool_executor.py)

```python
class ToolResult:
    tool_name: str          # "execute_code" | "docker_run" | "read_file"
    input_summary: str      # 截断的输入描述
    stdout: str             # 标准输出
    stderr: str             # 错误输出
    exit_code: int          # 返回码
    duration_ms: float      # 执行耗时
    success: bool           # 成功/失败标志

class ToolExecutor:
    def __init__(self, sandbox_dir: str = None, timeout: int = 30):
        self._sandbox_dir = sandbox_dir or temp dir
        self._timeout = timeout
        self._enable_docker = self._check_docker()
    
    def execute_code(self, code: str, language: str = "python") -> ToolResult
    def execute_python(code) -> ToolResult  # python3 -c
    def execute_sh(script) -> ToolResult    # bash -c
    def execute_cpp(code) -> ToolResult      # g++ → ./a.out
    def read_file(path) -> ToolResult        # cat file
    def write_file(path, content) -> ToolResult
    def docker_run(image, cmd, mounts) -> ToolResult  # docker run
    def get_capabilities() -> dict
```

### 3.2 安全沙箱策略

- 所有文件操作限制在 sandbox_dir 内
- 超时控制（默认 30s，可配置）
- 输出上限（stdout/stderr 各 64KB）
- Docker 可用时作为首选执行方式
- 不可用时降级为本地 subprocess

### 3.3 Docker 集成策略

Windows 主机有 Docker Desktop（`/mnt/c/Program Files/Docker/`）
但 WSL 中没有 docker CLI。

策略：
1. 如果系统中存在 docker CLI → 直接使用
2. 如果不存在 → 从 WSL 调用 Windows Docker（通过 `/mnt/c/.../docker.exe`）
3. 如果都不行 → 降级到本地执行，标记 docker 不可用
4. 未来可以安装 docker-ce 到 WSL

## 4. API 设计

### 4.1 新 API 端点 (web_app.py)

```
POST /api/tools/execute
  Body: {code: str, language: str, timeout: int}
  Response: {stdout, stderr, exit_code, duration_ms, success}

POST /api/tools/file
  Body: {action: "read"|"write", path: str, content: str (for write)}
  Response: {content, success}

GET  /api/tools/capabilities
  Response: {python: true, cpp: true, docker: false, shell: true}

POST /api/tools/docker
  Body: {image: str, command: [str], mounts: [{host, container}]}
  Response: {output, exit_code, success}
```

### 4.2 现有 API 增强

```
POST /api/process 返回增强:
  tool_results: [{tool_name, stdout_summary, exit_code, ...}]
  execution_verified: true/false
```

## 5. 实现计划

| 阶段 | 内容 | 文件 | 依赖 |
|------|------|------|------|
| P1 | ToolExecutor 基础 (Python/sh 执行 + 文件系统) | tool_executor.py | 无 |
| P2 | C++ 编译执行 + Docker 适配 | tool_executor.py | g++ |
| P3 | Agent 流水线集成 (Solver/Verifier 使用工具) | core.py, api_providers.py | P1 |
| P4 | API 端点 | web_app.py | P1 |
| P5 | 前端 UI 展示工具结果 | static/app.js | P4 |
