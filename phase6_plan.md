# Phase 6 插件市场 UI — 任务安排

## Phase 进度全景图

| Phase | 内容 | 状态 | 实施程度 |
|-------|------|:----:|:--------:|
| **1** | 核心接口定义 (`PluginInterface`, `PluginInfo`, 生命周期) | ✅ **已完成** | `PluginInterface.hpp` 定义完整，5 个虚方法 (`initialize/start/stop/shutdown/getLastError`) |
| **2** | PluginManager 核心 (`dlopen/dlsym`, 注册表, 依赖解析, 引用计数) | ✅ **已完成** | `PluginManager.hpp/.cpp` 完整实现：`scanPlugins`/`loadPlugin`/`initializePlugin`/`startPlugin`/`stopPlugin`/`unloadPlugin`/`unloadAll` + 拓扑序依赖解析 + 循环依赖检测 + 6 个 `.so` 插件可加载 |
| **3** | 热更新与沙箱 (热替换, 沙箱隔离, 事件通知) | ✅ **已完成** | `hotReload()` 原子交换 + 旧版本回退 + `PluginWatcher` (inotify) + `Sandbox` (fork+seccomp) + `CrashCallback`/`attemptRecovery` |
| **4** | 改造现有模块 (ToolPlugin/StrategyPlugin/JudgePlugin) | ✅ **已完成** | 但计划原文用的 `Tool/Strategy/Judge` 命名，实际实现成了**5 个 Agent 插件** (`agent_refiner/reasoner/solver/verifier/evaluator`) + `example_tool`，通过 `AgentPlugin` 接口扩展 `PluginInterface` + `Orchestrator::loadPluginAgents()` 完成集成 |
| **5** | PluginProvider (LLM 厂商作为插件) | ⚠️ **部分完成** | 实际没有写成 C++ 插件，而是 Python `api_providers.py` 的适配器模式（Provider 注册表 + `@register_provider` 装饰器 + 5 家厂商支持）；C++ 侧无 `ProviderPlugin` 接口 |
| **6** | **插件市场 UI（Python层）** | 🔴 **未开始** | 计划：插件列表/搜索/安装 + 插件配置界面 + 版本管理 |

---

## Phase 6 任务安排

### 架构概览

Phase 6 在已有 Flask Web UI (`web_app.py`) 上扩展，新增插件管理页面。

```
web_app.py (现有)
  ├── /  → index.html (主页面)            ← 现有
  ├── /api/process                        ← 现有
  ├── /api/status                         ← 现有
  ├── /api/plugins  (新增)                ← Phase 6
  ├── /api/plugins/<id>/config  (新增)    ← Phase 6
  └── /api/plugins/<id>/toggle  (新增)    ← Phase 6

templates/
  ├── index.html (现有)
  └── plugins.html (新增)                 ← Phase 6
```

### 任务分解

#### Task 1: 后端 API — `GET /api/plugins`

**目标**：返回完整插件列表，包括状态信息、元数据和当前配置。

**输入**：
- `PluginManager::listPlugins()` / `listPluginsByState()`
- `PluginManager::getPluginState()` / `isPluginLoaded()`

**输出 JSON 格式**：
```json
{
  "plugins": [
    {
      "id": "agent.refiner",
      "name": "Agent Refiner",
      "version": "1.0.0",
      "author": "CLMA",
      "type": "agent",
      "step": "refiner",
      "state": "RUNNING",
      "file_path": "/root/closed-loop-multiagent/build/lib/libagent_refiner.so",
      "dependencies": [],
      "dependents": [],
      "last_modified": "2025-04-28T10:30:00Z",
      "description": "Refines user queries for downstream processing"
    },
    ...
  ],
  "summary": {
    "total": 6,
    "running": 5,
    "loaded": 1,
    "error": 0,
    "unloaded": 0
  }
}
```

**技术点**：
- 必须通过 C++ PluginManager（通过框架实例）获取状态
- `web_app.py` 中已有的 `framework` 实例需要暴露 `plugin_manager`
- 需在 `CLMAFramework` 中增加 `get_plugin_manager()` 方法

**文件**：`python_interface/web_app.py`、`python_interface/core.py`

#### Task 2: 后端 API — `POST /api/plugins/<id>/toggle`

**目标**：启停指定插件。

**动作**：
- 如果 `state == RUNNING` → `stopPlugin(id)` → `shutdown` + `unload`
- 如果 `state == UNLOADED` → `loadPlugin(id)` → `initialize` → `start`

**错误处理**：
- 插件 ID 不存在 → 404 + `{"error": "Plugin not found"}`
- 操作失败 → 500 + `{"error": "...", "details": "..."}`
- 有依赖关系的插件不能单独卸载（应先卸载依赖者）

**文件**：`python_interface/web_app.py`

#### Task 3: 后端 API — `GET /api/plugins/<id>/config`

**目标**：获取指定插件的配置信息。

**输出**：
```json
{
  "id": "example.tool",
  "config": {
    "enabled": true,
    "parameters": {},
    "saved_at": null
  },
  "available_config_keys": []
}
```

**注意**：当前 `PluginInterface` 的 `configure(key, val)` 是 push 模式，没有 get_config 接口。需要权衡：

- **方案 A**（简单）：返回从 `PluginManager::saveConfig()` 读出的 JSON
- **方案 B**（完整）：在 `PluginInterface` 增加 `getConfig()` 虚方法

**建议**：先做方案 A，Phase 6 后续迭代再做方案 B。

**文件**：`python_interface/web_app.py`

#### Task 4: 前端页面 — `templates/plugins.html`

**目标**：插件管理页面，响应式暗色主题 UI。

**功能模块**：

```
┌─────────────────────────────────────────┐
│  🔌 插件管理              [× 关闭]     │
├─────────────────────────────────────────┤
│  搜索: [___________________]            │
│  筛选: [全部|运行中|已加载|错误|未加载]  │
├─────────────────────────────────────────┤
│  ┌─ agent.refiner ──────────── 🟢 ──┐  │
│  │ 版本 1.0.0  |  作者 CLMA        │  │
│  │ Agent - Refiner                 │  │
│  │ 文件: libagent_refiner.so       │  │
│  │ ┌─依赖───────────────┐          │  │
│  │ │ (无)               │          │  │
│  │ └────────────────────┘          │  │
│  │ [🔁 热更新] [⚙ 配置] [⏹ 停止] │  │
│  └─────────────────────────────────┘  │
│  ┌─ agent.reasoner ─────────── 🟢 ──┐ │
│  │ ...                              │  │
│  └─────────────────────────────────┘  │
│  ┌─ example.tool ──────────── 🟡 ──┐  │
│  │ ...                              │  │
│  └─────────────────────────────────┘  │
└─────────────────────────────────────────┘
```

**状态颜色**：
- 🟢 `RUNNING` — 绿色
- 🟡 `LOADED` — 黄色
- 🔴 `ERROR` — 红色
- ⚪ `UNLOADED` — 灰色
- 🔵 `INITIALIZED` — 蓝色

**交互**：
- 点击卡片展开/收起详情（配置文件、依赖树）
- 搜索框实时过滤
- 状态筛选标签
- 按钮 hover 效果（与现有 copy-btn 风格一致）

**文件**：`python_interface/templates/plugins.html`

#### Task 5: 前端路由 & 菜单集成

**目标**：添加导航入口 + URL 路由。

**改动**：
- `web_app.py` 增加 `@app.route('/plugins')` → 返回 `plugins.html`
- `index.html` 侧边栏/导航栏增加 "🔌 插件管理" 链接
- 可选的：在 index.html 右下角显示插件数量状态小 badge

**文件**：`python_interface/web_app.py`、`python_interface/templates/index.html`

#### Task 6: 后端 API — `POST /api/plugins/<id>/hot-reload`

**目标**：触发热更新。

**动作**：
- 调用 `PluginManager::hotReload(id)`
- 返回新的状态

**验证**：
- 返回 `{"success": true, "state": "RUNNING", ...}`
- 如果热更新失败（新版本不可用），返回回退后的旧状态

**文件**：`python_interface/web_app.py`

---

## 工作量估计

| 任务 | 估计 | 依赖 |
|------|:----:|------|
| Task 1: GET /api/plugins | 0.5-1h | core.py 暴露 plugin_manager |
| Task 2: POST /api/plugins/toggle | 0.5h | Task 1 |
| Task 3: GET /api/plugins/config | 0.5h | Task 1 |
| Task 4: templates/plugins.html | 2-3h | Task 1-3 |
| Task 5: 前端路由 & 菜单 | 0.5h | Task 4 |
| Task 6: POST /api/plugins/hot-reload | 0.5h | Task 1 |
| **合计** | **4.5-6.5h** | |

---

## 前置条件

1. ✅ `CLMAFramework` 中已有 `self.orchestrator`，通过 `orchestrator->getPluginManager()` 即可暴露
2. ❌ `core.py` 当前 `CLMAFramework.__init__()` 没有创建 `PluginManager` 实例
3. ❌ `Orchestrator` 的 C++ 侧需要 `getPluginManager()` getter（检查：当前有 `registerPluginManager()` 设置但没有 getter？）
4. ✅ 所有 C++ 测试通过（10/10）
5. ✅ 所有插件 `.so` 已构建（6 个在 `build/lib/`）

---

## 建议执行顺序

```
1. 先确认 C++ 侧 getPluginManager() getter 是否存在
   └─ 如缺则添加（5 分钟）
2. 在 CLMAFramework 中创建 PluginManager 实例
   └─ 集成到 Orchestrator
3. Task 1 + 2 + 3（后端 API，可并行写）
4. Task 4（前端页面）
5. Task 5 + 6（菜单集成 + 热更新 API）
6. 端到端测试：启动 web_app → 访问 /plugins → 操作 → 验证数据
```
