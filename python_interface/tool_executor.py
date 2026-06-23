"""
Tool Executor — 闭环多智能体框架的工具执行层

为 Agent 提供实际执行能力：代码运行、文件操作、Docker 容器化。
所有执行在沙箱目录中运行，有超时和输出限制。

使用方式：
    executor = ToolExecutor(sandbox_dir="/tmp/clma-sandbox")
    result = executor.execute_python("print('hello')")
    print(result.stdout)  # "hello\\n"
"""

import os
import re
import sys
import subprocess
import tempfile
import time
import json
import textwrap
import shutil
import enum


class SandboxLevel(enum.IntEnum):
    """Three-tier sandbox severity classification.

    STRICT = 0 — Safe code with no external interaction (basic computation).
    NORMAL = 1  — Code that uses os/subprocess/eval/exec (local I/O only).
    RELAXED = 2 — Code that requires network access (requests, socket, curl, wget).
    """
    STRICT = 0
    NORMAL = 1
    RELAXED = 2


class SandboxTier(enum.IntEnum):
    """五级沙箱隔离等级，映射 C++ SandboxConfig 的隔离概念。

    ISOLATE  = 0 — 完全隔离：executability<0.3，拒绝执行或走最大限制容器
    STRICT   = 1 — 严格隔离：只允许纯计算，无文件/网络
    MEDIUM   = 2 — 中等隔离：允许本地文件，拒绝网络
    PERMISSIVE = 3 — 宽松：允许文件和网络，走 Docker 沙箱
    NATIVE   = 4 — 无沙箱：直接在 host subprocess 中执行
    """
    ISOLATE = 0
    STRICT = 1
    MEDIUM = 2
    PERMISSIVE = 3
    NATIVE = 4


# 风险检测等级 → SandboxTier 映射（基础映射，不考虑评分）
_RISK_LEVEL_TO_TIER: dict[SandboxLevel, SandboxTier] = {
    SandboxLevel.STRICT: SandboxTier.STRICT,
    SandboxLevel.NORMAL: SandboxTier.MEDIUM,
    SandboxLevel.RELAXED: SandboxTier.PERMISSIVE,
}


def _score_to_tier(executability: float, base_tier: SandboxTier) -> SandboxTier:
    """根据 executability 评分调整沙箱等级。

    评分越高→越宽松。评分越低→越严格。
    executability 为空或 None 时不做调整。
    """
    if executability is None:
        return base_tier

    # 强制降级：评分极低时直接隔离
    if executability < 0.3:
        return SandboxTier.ISOLATE
    elif executability < 0.5:
        # 在 base_tier 基础上再降一级
        tier_vals = list(SandboxTier)
        current_idx = tier_vals.index(base_tier)
        if current_idx > 0:
            return tier_vals[current_idx - 1]
        return base_tier
    elif executability >= 0.9:
        # 评分极高：升一级
        tier_vals = list(SandboxTier)
        current_idx = tier_vals.index(base_tier)
        if current_idx < len(tier_vals) - 1:
            return tier_vals[current_idx + 1]
        return base_tier

    return base_tier


# Patterns that signal potential danger (NORMAL level or above)
_DANGEROUS_PATTERNS = [
    r'import\s+os\s*;?\s*os\.system', r'subprocess\.(call|Popen|run)',
    r'eval\(', r'exec\(', r'__import__\(',
]

# Patterns that signal network access (RELAXED level)
_NETWORK_PATTERNS = [
    r'requests\.(get|post|put|delete)', r'socket\.',
    r'curl ', r'wget ', r'http://', r'https://',
    r'urllib\.request', r'httpx\.',
]


def _detect_risk_level(code: str) -> SandboxLevel:
    """Analyze code and determine the appropriate sandbox level.

    Classification logic:
    1. If any network pattern matches -> RELAXED (needs network)
    2. If any dangerous pattern matches -> NORMAL (needs approval)
    3. Otherwise -> STRICT (safe, automatic execution)

    Args:
        code: Source code to analyze.

    Returns:
        SandboxLevel indicating required sandbox tier.
    """
    if not code:
        return SandboxLevel.STRICT

    # Network patterns take priority (highest risk)
    for p in _NETWORK_PATTERNS:
        if re.search(p, code, re.IGNORECASE):
            return SandboxLevel.RELAXED

    # Local system access patterns
    for p in _DANGEROUS_PATTERNS:
        if re.search(p, code):
            return SandboxLevel.NORMAL

    return SandboxLevel.STRICT


class ToolResult:
    """工具执行的结果。"""
    
    def __init__(self, tool_name: str = "", stdout: str = "", 
                 stderr: str = "", exit_code: int = -1,
                 duration_ms: float = 0, success: bool = False,
                 input_summary: str = ""):
        self.tool_name = tool_name
        self.input_summary = input_summary
        self.stdout = stdout
        self.stderr = stderr
        self.exit_code = exit_code
        self.duration_ms = duration_ms
        self.success = success
    
    def to_dict(self) -> dict:
        """转为可序列化字典（截断大输出）。"""
        return {
            "tool_name": self.tool_name,
            "input_summary": self.input_summary,
            "stdout": self.stdout,
            "stderr": self.stderr,
            "exit_code": self.exit_code,
            "duration_ms": round(self.duration_ms, 1),
            "success": self.success,
        }
    
    def __repr__(self) -> str:
        s = f"[{self.tool_name}] exit={self.exit_code} dur={self.duration_ms:.0f}ms"
        if self.stdout:
            s += f" stdout={self.stdout[:100]}"
        if self.stderr:
            s += f" stderr={self.stderr[:100]}"
        return s


class ToolExecutor:
    """安全执行各种工具操作的执行器。
    
    特性：
    - 所有文件操作限制在沙箱目录内
    - 超时控制（默认 30s）
    - 输出上限（stdout/stderr 各 64KB）
    - Docker 可用时优先使用
    - 不可用时代码在本地 subprocess 中运行
    
    Args:
        sandbox_dir: 沙箱根目录（默认为临时目录）
        timeout: 单次执行超时（秒，默认 30）
        max_output_chars: 输出截断（默认 65536）
        enable_docker: 是否启用 Docker（默认自动检测）
    """
    
    def __init__(self, sandbox_dir: str = None, timeout: int = 120,
                 max_output_chars: int = 262144, enable_docker: bool = None):
        # 沙箱目录
        if sandbox_dir is None:
            sandbox_dir = tempfile.mkdtemp(prefix="clma-sandbox-")
        self._sandbox_dir = os.path.abspath(sandbox_dir)
        os.makedirs(self._sandbox_dir, exist_ok=True)
        
        self._timeout = timeout
        self._max_output = max_output_chars
        
        # Docker 检测
        if enable_docker is None:
            self._docker_available = self._check_docker()
        else:
            self._docker_available = enable_docker
        
        # 语言编译器/运行时检测
        self._has_gpp = shutil.which("g++") is not None
        self._has_python3 = shutil.which("python3") is not None
        self._has_bash = shutil.which("bash") is not None
        self._has_node = shutil.which("node") is not None
    
    # ── 能力查询 ──────────────────────────────────
    
    def get_capabilities(self) -> dict:
        """返回当前执行器的能力。"""
        return {
            "python": self._has_python3,
            "shell": self._has_bash,
            "cpp": self._has_gpp,
            "javascript": self._has_node,
            "docker": self._docker_available,
            "sandbox_dir": self._sandbox_dir,
            "timeout": self._timeout,
        }
    
    def get_execution_tier(self, code: str, executability: float = None) -> SandboxTier:
        """综合代码风险分析和评分，返回应使用的沙箱隔离等级。

        Args:
            code: 要执行的代码。
            executability: 本次执行的可执行性评分（0-1）。None 表示不评分影响。

        Returns:
            确定的 SandboxTier 等级。
        """
        # 1. 基于代码本身的静态分析
        risk = _detect_risk_level(code)
        base_tier = _RISK_LEVEL_TO_TIER.get(risk, SandboxTier.STRICT)
        
        # 2. 可执行性评分修正
        return _score_to_tier(executability, base_tier)
    
    def execute_code_with_tier(self, code: str, language: str = "python",
                                timeout: int = None, env: dict = None,
                                executability: float = None) -> ToolResult:
        """根据评分感知的沙箱等级执行代码。

        Args:
            code: 代码内容
            language: 编程语言
            timeout: 超时覆盖
            env: 环境变量
            executability: 可执行性评分（影响沙箱隔离等级）

        Returns:
            ToolResult
        """
        tier = self.get_execution_tier(code, executability)
        
        if tier == SandboxTier.ISOLATE:
            # ISOLATE 等级：拒绝执行
            result = ToolResult(
                tool_name=f"execute_{language}",
                input_summary=code[:100] if code else "",
            )
            result.exit_code = -1
            result.success = False
            result.stderr = (
                f"[BLOCKED] Execution denied — sandbox tier ISOLATE (executability={executability}). "
                f"Code quality score too low for safe execution."
            )
            return result
        
        # 非 ISOLATE：正常执行
        return self.execute_code(code, language, timeout, env)
    
    def is_docker_available(self) -> bool:
        return self._docker_available
    
    # ── 核心执行方法 ──────────────────────────────
    
    def execute_code(self, code: str, language: str = "python",
                     timeout: int = None, env: dict = None) -> ToolResult:
        """执行代码，自动根据语言选择执行方式。
        
        Args:
            code: 代码内容
            language: "python" | "shell" | "sh" | "bash" | "cpp" | "c" | "text"
            timeout: 覆盖默认超时
            env: 附加环境变量
        """
        lang = language.lower()
        if lang in ("python", "py"):
            return self.execute_python(code, timeout, env)
        elif lang in ("sh", "shell", "bash"):
            return self.execute_sh(code, timeout, env)
        elif lang in ("cpp", "c++"):
            return self.execute_cpp(code, timeout, env)
        elif lang in ("c",):
            return self.execute_c(code, timeout, env)
        elif lang in ("javascript", "js", "node"):
            return self.execute_javascript(code, timeout, env)
        else:
            # 文本/未知类型 — 只保存文件
            return self._save_and_report(code, language)
    
    def execute_python(self, code: str, timeout: int = None,
                       env: dict = None) -> ToolResult:
        """执行 Python 代码。"""
        start = time.monotonic()
        result = ToolResult(tool_name="execute_python",
                           input_summary=code[:100])
        try:
            # 写入临时文件以便支持多行/import
            script_path = self._write_temp_file(code, "py")
            completed = subprocess.run(
                ["python3", script_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
                env={**os.environ, **(env or {})},
            )
            result.stdout = completed.stdout[-self._max_output:]
            result.stderr = completed.stderr[-self._max_output:]
            result.exit_code = completed.returncode
            result.success = completed.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] Execution exceeded {timeout or self._timeout}s"
            result.success = False
        except FileNotFoundError as e:
            result.stderr = f"[ERROR] python3 not found: {e}"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    def execute_sh(self, script: str, timeout: int = None,
                   env: dict = None) -> ToolResult:
        """执行 Shell 脚本。"""
        start = time.monotonic()
        result = ToolResult(tool_name="execute_sh",
                           input_summary=script[:100])
        try:
            script_path = self._write_temp_file(script, "sh")
            completed = subprocess.run(
                ["bash", script_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
                env={**os.environ, **(env or {})},
            )
            result.stdout = completed.stdout[-self._max_output:]
            result.stderr = completed.stderr[-self._max_output:]
            result.exit_code = completed.returncode
            result.success = completed.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] Execution exceeded {timeout or self._timeout}s"
            result.success = False
        except FileNotFoundError:
            result.stderr = "[ERROR] bash not found"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    def execute_cpp(self, code: str, timeout: int = None,
                    env: dict = None) -> ToolResult:
        """编译并执行 C++ 代码。
        
        步骤：写入临时 .cpp → g++ 编译 → 运行可执行文件
        """
        start = time.monotonic()
        result = ToolResult(tool_name="execute_cpp",
                           input_summary=code[:100])
        if not self._has_gpp:
            result.stderr = "[ERROR] g++ not available on this system"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        
        try:
            src_path = self._write_temp_file(code, "cpp")
            bin_path = src_path.replace(".cpp", "")
            
            # 编译
            compile_proc = subprocess.run(
                ["g++", "-std=c++17", "-O2", src_path, "-o", bin_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
            )
            if compile_proc.returncode != 0:
                result.stdout = compile_proc.stdout[-self._max_output:]
                result.stderr = compile_proc.stderr[-self._max_output:]
                result.exit_code = compile_proc.returncode
                result.success = False
                result.duration_ms = (time.monotonic() - start) * 1000
                return result
            
            # 运行
            run_proc = subprocess.run(
                [bin_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
                env={**os.environ, **(env or {})},
            )
            result.stdout = run_proc.stdout[-self._max_output:]
            result.stderr = run_proc.stderr[-self._max_output:]
            result.exit_code = run_proc.returncode
            result.success = run_proc.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] C++ compilation/execution exceeded {timeout or self._timeout}s"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
            # 清理二进制
            if 'bin_path' in locals() and os.path.exists(bin_path):
                try:
                    os.remove(bin_path)
                except OSError:
                    pass
        return result
    
    def execute_c(self, code: str, timeout: int = None,
                  env: dict = None) -> ToolResult:
        """编译并执行 C 代码。"""
        start = time.monotonic()
        result = ToolResult(tool_name="execute_c",
                           input_summary=code[:100])
        if not self._has_gpp:
            result.stderr = "[ERROR] g++ not available"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        
        try:
            src_path = self._write_temp_file(code, "c")
            bin_path = src_path.replace(".c", "")
            
            compile_proc = subprocess.run(
                ["gcc" if shutil.which("gcc") else "g++",
                 "-std=c11", "-O2", src_path, "-o", bin_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
            )
            if compile_proc.returncode != 0:
                result.stdout = compile_proc.stdout[-self._max_output:]
                result.stderr = compile_proc.stderr[-self._max_output:]
                result.exit_code = compile_proc.returncode
                result.success = False
                result.duration_ms = (time.monotonic() - start) * 1000
                return result
            
            run_proc = subprocess.run(
                [bin_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
            )
            result.stdout = run_proc.stdout[-self._max_output:]
            result.stderr = run_proc.stderr[-self._max_output:]
            result.exit_code = run_proc.returncode
            result.success = run_proc.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] C execution exceeded {timeout or self._timeout}s"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
            if 'bin_path' in locals() and os.path.exists(bin_path):
                try:
                    os.remove(bin_path)
                except OSError:
                    pass
        return result

    def execute_javascript(self, code: str, timeout: int = None,
                           env: dict = None) -> ToolResult:
        """执行 JavaScript 代码（Node.js）。

        步骤：写入临时 .js → node 执行
        """
        start = time.monotonic()
        result = ToolResult(tool_name="execute_javascript",
                           input_summary=code[:100])
        if not self._has_node:
            result.stderr = "[ERROR] Node.js (node) not available on this system"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result

        try:
            script_path = self._write_temp_file(code, "js")
            completed = subprocess.run(
                ["node", script_path],
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
                cwd=self._sandbox_dir,
                env={**os.environ, **(env or {})},
            )
            result.stdout = completed.stdout[-self._max_output:]
            result.stderr = completed.stderr[-self._max_output:]
            result.exit_code = completed.returncode
            result.success = completed.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] Node.js execution exceeded {timeout or self._timeout}s"
            result.success = False
        except FileNotFoundError:
            result.stderr = "[ERROR] node command not found"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result

    # ── 文件操作 ──────────────────────────────────
    
    def read_file(self, path: str) -> ToolResult:
        """读取沙箱内的文件。"""
        start = time.monotonic()
        result = ToolResult(tool_name="read_file",
                           input_summary=f"read {path}")
        safe_path = self._resolve_sandbox_path(path)
        if safe_path is None:
            result.stderr = f"[DENIED] Path outside sandbox: {path}"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        try:
            with open(safe_path, "r", errors="replace") as f:
                content = f.read()
            result.stdout = content[-self._max_output:]
            result.exit_code = 0
            result.success = True
        except FileNotFoundError:
            result.stderr = f"[ERROR] File not found: {path}"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    def write_file(self, path: str, content: str) -> ToolResult:
        """写入沙箱内的文件。"""
        start = time.monotonic()
        result = ToolResult(tool_name="write_file",
                           input_summary=f"write {path} ({len(content)} chars)")
        safe_path = self._resolve_sandbox_path(path)
        if safe_path is None:
            result.stderr = f"[DENIED] Path outside sandbox: {path}"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        try:
            os.makedirs(os.path.dirname(safe_path), exist_ok=True)
            with open(safe_path, "w") as f:
                f.write(content)
            result.stdout = f"Wrote {len(content)} bytes to {path}"
            result.exit_code = 0
            result.success = True
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    def list_files(self, path: str = ".") -> ToolResult:
        """列出沙箱内的文件。"""
        start = time.monotonic()
        result = ToolResult(tool_name="list_files",
                           input_summary=f"ls {path}")
        safe_path = self._resolve_sandbox_path(path)
        if safe_path is None:
            result.stderr = f"[DENIED] Path outside sandbox: {path}"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        try:
            items = os.listdir(safe_path)
            result.stdout = "\n".join(sorted(items))
            result.exit_code = 0
            result.success = True
        except FileNotFoundError:
            result.stderr = f"[ERROR] Directory not found: {path}"
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    # ── Docker 集成 ───────────────────────────────
    
    def docker_run(self, image: str, command: list = None,
                   timeout: int = None, mounts: list = None,
                   workdir: str = None, remove: bool = True) -> ToolResult:
        """运行 Docker 容器。
        
        Args:
            image: Docker 镜像名（如 "python:3.11-slim"）
            command: 容器内命令（如 ["python3", "-c", "print(1)"]）
            timeout: 超时
            mounts: 挂载点列表 [{"host": "/tmp", "container": "/mnt"}]
            workdir: 容器内工作目录
            remove: 运行后自动删除容器
        """
        start = time.monotonic()
        result = ToolResult(tool_name="docker_run",
                           input_summary=f"{image} {command}")
        if not self._docker_available:
            result.stderr = "[ERROR] Docker is not available on this system"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        
        try:
            cmd = self._build_docker_cmd(image, command, mounts, workdir, remove)
            completed = subprocess.run(
                cmd,
                capture_output=True, text=True,
                timeout=timeout or self._timeout,
            )
            result.stdout = completed.stdout[-self._max_output:]
            result.stderr = completed.stderr[-self._max_output:]
            result.exit_code = completed.returncode
            result.success = completed.returncode == 0
        except subprocess.TimeoutExpired:
            result.stderr = f"[TIMEOUT] Docker execution exceeded {timeout or self._timeout}s"
            result.success = False
        except FileNotFoundError:
            result.stderr = "[ERROR] docker command not found"
            self._docker_available = False
            result.success = False
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    def docker_info(self) -> ToolResult:
        """查询 Docker 信息。"""
        start = time.monotonic()
        result = ToolResult(tool_name="docker_info")
        if not self._docker_available:
            result.stderr = "[ERROR] Docker is not available"
            result.success = False
            result.duration_ms = (time.monotonic() - start) * 1000
            return result
        try:
            completed = subprocess.run(
                ["docker", "info", "--format", "{{json .}}"],
                capture_output=True, text=True, timeout=10,
            )
            if completed.returncode == 0:
                info = json.loads(completed.stdout)
                result.stdout = json.dumps({
                    "server_version": info.get("ServerVersion", "?"),
                    "images": info.get("Images", 0),
                    "containers": info.get("Containers", 0),
                    "running": info.get("ContainersRunning", 0),
                }, indent=2)
            else:
                result.stderr = completed.stderr
            result.exit_code = completed.returncode
            result.success = completed.returncode == 0
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        finally:
            result.duration_ms = (time.monotonic() - start) * 1000
        return result
    
    # ── 沙箱管理 ──────────────────────────────────
    
    def get_sandbox_path(self) -> str:
        return self._sandbox_dir
    
    def reset_sandbox(self):
        """清空沙箱目录内容。"""
        for item in os.listdir(self._sandbox_dir):
            path = os.path.join(self._sandbox_dir, item)
            if os.path.isfile(path):
                os.remove(path)
            elif os.path.isdir(path):
                shutil.rmtree(path)
    
    def cleanup(self):
        """删除沙箱目录。"""
        if os.path.exists(self._sandbox_dir):
            shutil.rmtree(self._sandbox_dir, ignore_errors=True)
    
    # ── 内部辅助方法 ──────────────────────────────
    
    def _check_docker(self) -> bool:
        """检测 Docker CLI 是否可用。"""
        docker_paths = [
            "docker",
            "/usr/bin/docker",
            "/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe",
        ]
        for dp in docker_paths:
            if shutil.which(dp) is not None or os.path.exists(dp):
                return True
        return False
    
    def _resolve_sandbox_path(self, path: str) -> str | None:
        """Resolve path strictly inside the sandbox; return None if outside.

        Security fix: block path traversal via `..`, absolute paths, and the
        classic startswith bypass (e.g. /tmp/sandbox vs /tmp/sandbox_evil).

        release/v2: also reject empty paths early (release-line stability).
        """
        if not path or not str(path).strip():
            return None
        # Never honor attacker-controlled absolute paths
        if os.path.isabs(path):
            return None
        # Reject null bytes and explicit parent-directory segments
        if "\x00" in path:
            return None
        parts = path.replace("\\", "/").split("/")
        if ".." in parts:
            return None
        sandbox_root = os.path.realpath(self._sandbox_dir)
        combined = os.path.realpath(os.path.join(sandbox_root, path))
        try:
            common = os.path.commonpath([sandbox_root, combined])
        except ValueError:
            return None
        if common != sandbox_root:
            return None
        return combined

    def _write_temp_file(self, content: str, ext: str) -> str:
        """将内容写入沙箱内的临时文件。"""
        path = os.path.join(self._sandbox_dir, f"script_{int(time.time()*1000000)}.{ext}")
        with open(path, "w") as f:
            f.write(content)
        return path
    
    def _build_docker_cmd(self, image: str, command: list,
                          mounts: list, workdir: str, remove: bool) -> list:
        """构建 docker run 命令。"""
        cmd = ["docker", "run"]
        if remove:
            cmd.append("--rm")
        # 挂载沙箱目录
        cmd.extend(["-v", f"{self._sandbox_dir}:/sandbox"])
        if mounts:
            for m in mounts:
                cmd.extend(["-v", f"{m['host']}:{m['container']}"])
        if workdir:
            cmd.extend(["-w", workdir])
        else:
            cmd.extend(["-w", "/sandbox"])
        cmd.append(image)
        if command:
            cmd.extend(command)
        return cmd
    
    def _save_and_report(self, code: str, language: str) -> ToolResult:
        """保存不可执行的代码类型并返回报告。"""
        result = ToolResult(tool_name=f"save_{language}",
                           input_summary=code[:100])
        try:
            path = self._write_temp_file(code, language)
            result.stdout = f"Saved to {os.path.basename(path)} ({len(code)} chars)"
            result.exit_code = 0
            result.success = True
        except Exception as e:
            result.stderr = f"[ERROR] {e}"
            result.success = False
        return result
    
    def __enter__(self):
        return self
    
    def __exit__(self, *args):
        self.cleanup()
