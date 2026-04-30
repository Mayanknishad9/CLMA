# Phase 7 UI 大重构 — 现状分析文档

## 1. web_app.py — FlASK 路由与 API 端点

### 现有页面路由 (2)
| Route | Function | Template |
|-------|----------|----------|
| GET `/` | index() | index.html (SPA) |
| GET `/plugins` | plugins_page() | plugins.html (独立页面) |

### 现有 API 端点 (17)

**查询管线 (3)**
| Method | Endpoint | Response |
|--------|----------|----------|
| GET | `/api/status` | `{status, mode, stats, rules, api_configured, api_provider}` |
| POST | `/api/process` | `{query, result, stats, history, mode, tool_results}` — 同步阻塞 !!! |
| GET | `/api/history` | `{history}` |

**设置 (1)**
| Method | Endpoint | Notes |
|--------|----------|-------|
| GET/POST | `/api/settings` | mode, max_iterations, threshold, token_budget, reset |

**API 配置 (3)**
| Method | Endpoint |
|--------|----------|
| GET/POST | `/api/api-config` |
| GET | `/api/api-config/providers` |
| POST | `/api/api-config/test` |

**规则管理 (2)**
| Method | Endpoint |
|--------|----------|
| GET/POST | `/api/rules` |
| GET | `/api/rules/template` |

**统计 (1)**
| GET | `/api/stats` | + token_by_agent, token_by_operation |

**工具执行 (5)**
| Method | Endpoint |
|--------|----------|
| GET | `/api/tools/status` |
| POST | `/api/tools/execute` |
| POST | `/api/tools/sandbox/clean` |
| GET | `/api/tools/sandbox/files` |
| GET | `/api/tools/docker/info` |

**插件管理 (6)**
| Method | Endpoint |
|--------|----------|
| GET | `/api/plugins/scan` |
| GET | `/api/plugins` |
| GET | `/api/plugins/<id>` |
| POST | `/api/plugins/<id>/toggle` |
| POST | `/api/plugins/<id>/hot-reload` |
| POST | `/api/plugins/<id>/recover` |

### 🎯 需要修改/新增的关键点

1. **新增 SSE 端点**: `GET /api/process/stream` — 替代同步 POST /api/process
   - 使用 `flask.Response` + `text/event-stream` MIME type
   - Flask 原生支持 SSE (不需要 extra 依赖), 但需注意:
     - 使用 `Response(mimetype='text/event-stream')`
     - 使用 `stream_with_context()` 保持请求上下文
     - 在生成器函数中 yield `data: {json}\n\n`
   - 事件类型设计:
     - `event: status\n` → `{phase: "refiner|reasoner|solver|verifier|evaluator", iteration: N}`
     - `event: agent_start\n` → `{agent_name, iteration}`
     - `event: agent_complete\n` → `{agent_name, iteration, content_preview, score, duration_ms}`
     - `event: iteration\n` → `{iteration, scores, content}`
     - `event: tool_execution\n` → `{tool_name, success, stdout_preview, duration_ms}`
     - `event: done\n` → final result payload
     - `event: error\n` → error message

2. **新增会话存储 API**:
   - `GET/POST /api/sessions` — 会话列表/新建
   - `GET/DELETE /api/sessions/<id>` — 单个会话详情/删除
   - `POST /api/sessions/<id>/rename` — 重命名
   - 会话数据结构: `{id, name, created_at, updated_at, query_count, last_query, messages: []}`
   - 存储位置: `config/sessions/` 目录下 JSON 文件
   - 每个会话包含消息历史: `[{role: "user|assistant", query, result, timestamp, stats}]`

3. **保留所有现有端点** — 向后兼容，SSE 为新增，不替换原有 POST

---

## 2. index.html — 完整 DOM 结构

### 顶级元素
```
<body>
  <header.header>                     ← sticky header, z-index:100
    .header-brand (logo + title)
    .header-actions
      [API] [Rules] [Tools] [Plugins]  ← header buttons
      .mode-switch                      ← mode toggle
      [Reset button]
    
  <div.app-container>                  ← flex container
    <aside.sidebar#sidebar>            ← width:300px, left column
      <section.stats-panel>
        .stats-grid#statsGrid
          statQueries, statIterations, statRulesMatches, statCompleted, statTokenUsage
      <section.config-panel>
        configIterations (slider), configThreshold (slider)
      <section.rules-panel>
        rulesList (rule items), rulesCount (badge)
    
    <main.main-content>                ← flex:1, right column
      <div.query-section>
        textarea#queryInput
        button#submitBtn
        
      <div#resultsSection.hidden>      ← initially hidden, shown after query
        <div.score-overview>
          canvas#scoreGauge (140x140)
          scoreOverall, scoreBar1/2/3, scoreVal1/2/3
          resultStatus (success/failure)
          
        <div.content-panel>
          button.copy-btn#copyOutputBtn
          pre#outputContent
          
        <div#toolResultsPanel.hidden>
          toolResultsCount, toolResultsList
          
        <div#reasoningChainPanel.hidden>
          chainCount, chainContainer
          
        <div.timeline-panel>
          timelineCount, executionTimeline

  <div#loadingOverlay.hidden>          ← fixed overlay, z-index:999
    .spinner + .loading-text
  
  <!-- MODALS -->
  <div#apiModal.modal-overlay.hidden>  ← API Configuration
  <div#ruleModal.modal-overlay.hidden> ← Rule Editor
  <div#toolsModal.modal-overlay.hidden> ← Tool Sandbox
```

### 🎯 需要修改/新增的关键 DOM 位置

1. **侧边栏 DOM 插入位置** — `<aside.sidebar#sidebar>` 内部:
   - **新增「会话历史面板」** 在 stats-panel 上方或 stats-panel 与 config-panel 之间:
     ```html
     <section class="panel sessions-panel">
       <h3 class="panel-title">Sessions <span class="badge" id="sessionsCount">0</span></h3>
       <div class="sessions-list" id="sessionsList"></div>
       <button class="btn btn-sm btn-ghost" onclick="newSession()">+ New Session</button>
     </section>
     ```
   - **现有面板保留**: stats-panel, config-panel, rules-panel 保持不变
   - 侧边栏本身需要可折叠（移动端响应式）

2. **流程图渲染占位符位置**:
   - **选项 A（推荐）**: 在 resultsSection 内, score-overview 下方, content-panel 上方
     ```html
     <div id="flowchartPanel" class="panel flowchart-panel hidden">
       <h3 class="panel-title">Agent Flow</h3>
       <div id="flowchartContainer" class="flowchart-container"></div>
     </div>
     ```
   - **选项 B**: 作为 reasoningChainPanel 的子元素或替换
   - 流程图使用 SVG 或 Canvas 渲染, 显示 5 个 agent 节点和迭代循环
   - 实时更新: 当前正在执行的 agent 高亮, 已完成 agent 打勾

3. **SSE 实时状态指示器**:
   - **侧边栏底部** 或 **header 右侧** 添加状态指示:
     ```html
     <div id="processingStatus" class="processing-status hidden">
       <div class="status-dot"></div>
       <span class="status-text" id="statusText">Processing...</span>
       <span class="status-phase" id="statusPhase">Refining query</span>
     </div>
     ```
   - 替代全屏 loadingOverlay

4. **保留的元素**:
   - 所有 modal（apiModal, ruleModal, toolsModal）— 完整保留
   - query-section — 保留，但可以添加 min/max/restore 按钮
   - score-overview — 保留，但可以添加实时更新动画
   - outputContent — 保留，流式追加
   - executionTimeline — 保留，实时追加
   - reasoningChainPanel — 保留，实时更新

---

## 3. app.js — 完整 JavaScript 逻辑

### 全局状态 (4 个变量)
```js
let processing = false;        // 提交锁
let currentRules = [];         // 缓存的规则列表
let editingRuleIndex = -1;     // 当前编辑的规则索引
let apiProviderMap = { ... };  // 提供商元数据
```

### 核心函数 (24 个)

**初始化 (1)**
| Function | 行 | 动作 |
|----------|----|------|
| DOMContentLoaded | 19 | loadStatus(), loadRules(), 键盘快捷键绑定, slider 显示绑定 |

**API 调用 (1)**
| Function | 行 | 说明 |
|----------|----|------|
| api(url, method, body) | 38 | 通用 fetch 封装，自动 JSON 序列化 |

**数据加载 (2)**
| Function | 行 | 说明 |
|----------|----|------|
| loadStatus() | 49 | GET /api/status → updateStats() + updateMode() |
| loadRules() | 55 | GET /api/rules → 渲染侧边栏 rulesList |

**核心查询流程 (2)**
| Function | 行 | 说明 |
|----------|----|------|
| submitQuery() | 69 | **同步 POST /api/process** → loadingOverlay → displayResults() |
| displayResults() | 98 | 更新 score、output、timeline、reasoning_chain、tool_results |

**渲染函数 (5)**
| Function | 行 | 说明 |
|----------|----|------|
| renderTimeline(history) | 128 | 遍历 history 创建 timeline-item |
| drawGauge(value) | 150 | Canvas 弧形仪表盘 |
| renderReasoningChain(data) | 713 | 迭代分数柱状图 + agent 步骤链 |
| renderRuleList(container) | 393 | 规则列表渲染 |
| animateBar(barId, valId, value) | 196 | 动画化分数条 |

**状态更新 (2)**
| Function | 行 | 说明 |
|----------|----|------|
| updateStats(stats) | 205 | 更新 5 个 stat-card |
| updateMode(mode) | 215 | 切换 CLOSED/OPEN 模式按钮样式 |

**操作函数 (4)**
| Function | 行 | 说明 |
|----------|----|------|
| toggleMode() | 230 | 切换闭环/开环 |
| updateSettings() | 250 | 保存 slider 设置 |
| resetFramework() | 256 | 重置 + 隐藏结果 |
| copyOutput() | 832 | 复制 output 内容到剪贴板 |

**Modal 系统 (3)**
| Function | 行 | 说明 |
|----------|----|------|
| openModal(id) | 266 | 打开 + 加载对应数据 |
| closeModal(id) | 272 | 关闭 |
| closeOnOverlay(event, id) | 276 | 点击蒙层关闭 |

**API 配置 (5)**
| Function | 行 |
|----------|----|
| selectProvider() | 284 |
| loadApiConfig() | 303 |
| saveApiConfig() | 317 |
| testApiConnection() | 337 |

**规则编辑器 (9)**
| Function | 行 |
|----------|----|
| loadRulesToEditor() | 377 |
| addNewRule() | 410 |
| openRuleEditor(index) | 425 |
| cancelRuleEdit() | 460 |
| saveRuleEdit() | 468 |
| deleteRule(index) | 497 |
| deleteCurrentRule() | 512 |
| saveAllRules() | 683 |

**工具 (4)**
| Function | 行 |
|----------|----|
| openToolsModal() | 521 |
| refreshSandbox() | 526 |
| cleanSandbox() | 561 |
| executeTool() | 570 |
| refreshSandboxFiles() | 549 |

**工具结果显示增强 (1)**
| Function | 行 | 说明 |
|----------|----|------|
| displayResults (override) | 637 | 包装原 displayResults, 追加 tool_results 渲染 |

**工具函数 (2)**
| Function | 行 |
|----------|----|
| escapeHtml(text) | 672 |
| attrEncode(text) | 679 |
| toggleChainContent(contentId, btn) | 816 |

### 🎯 需要修改/新增的关键 JS 逻辑

1. **替换 submitQuery() 为 SSE 版本**:
   - 当前: `await api('/api/process', 'POST', { query })` → 同步等待
   - 改为: 创建 `EventSource('/api/process/stream?query=' + encodeURIComponent(query))`
   - 移除 loadingOverlay 的显式 show/hide
   - 在 SSE 事件中逐步渲染:
     - `agent_start` → 更新 processingStatus (当前 agent, 迭代数)
     - `agent_complete` → 追加到 timeline
     - `iteration` → 更新分数图表 + 流程图
     - `tool_execution` → 追加到 toolResultsPanel
     - `done` → 最终 displayResults()
     - `error` → 显示错误

2. **新增 SSE 连接管理**:
   ```js
   let currentEventSource = null;
   function connectSSE(query) { ... }
   function disconnectSSE() { if (currentEventSource) currentEventSource.close(); }
   ```
   - 组件卸载/新查询时自动关闭旧连接

3. **新增会话管理状态和函数**:
   ```js
   let sessions = [];
   let currentSessionId = null;
   function loadSessions() { ... }
   function switchSession(id) { ... }
   function newSession() { ... }
   function deleteSession(id) { ... }
   function saveToSession(query, result) { ... }
   ```

4. **新增流程图渲染函数**:
   ```js
   function renderFlowchart(data) { /* SVG/Canvas 5-agent flow */ }
   function updateFlowchartPhase(agentName, status) { /* highlight active node */ }
   ```

5. **替换 loadingOverlay 逻辑**:
   - 删除 `document.getElementById('loadingOverlay').classList.remove('hidden')` 调用
   - 改为: `showProcessingStatus(agentName, iteration)`
   - `hideProcessingStatus()` 在 SSE `done` 或 `error` 事件中调用
   - loadingOverlay 元素本身可以保留（作为后备），但不主动使用

6. **当前 processing 变量保留** — 但改为 SSE 连接状态跟踪

7. **displayResults() 需要扩展** — 接收 SSE 增量更新, 非全量替换

---

## 4. style.css — 完整样式体系

### CSS 变量 (21 个)
```css
--bg-primary: #0a0e1a;       --bg-secondary: #111827;
--bg-card: #1a2035;           --bg-card-hover: #1f2a44;
--border-color: #2a3550;      --border-active: #4a6cf7;
--text-primary: #e8edf5;      --text-secondary: #8892b0;
--text-muted: #555e7a;
--accent-1: #4a6cf7;          --accent-2: #7c5cfc;
--accent-3: #06d6a0;          --accent-4: #ffd166;
--accent-5: #ef476f;
--radius: 12px;               --radius-sm: 8px;
--shadow: 0 4px 24px rgba(0,0,0,0.4);
--font: 'Inter', ...;         --font-mono: 'JetBrains Mono', ...;
```

### 组件类 (按模块)

| 模块 | 类名 | 行数 |
|------|------|------|
| Header | `.header, .header-brand, .header-logo, .header-title, .header-subtitle, .header-actions` | 42-59 |
| Mode Switch | `.mode-switch, .mode-label, .mode-btn, .mode-indicator` | 61-82 |
| Buttons | `.btn, .btn-primary, .btn-ghost, .btn-secondary, .btn-danger, .btn-sm` | 84-103, 754-764 |
| Layout | `.app-container` | 104-112 |
| Sidebar | `.sidebar` | 114-121 |
| Panels | `.panel, .panel-title, .badge` | 123-146 |
| Stats | `.stats-grid, .stat-card, .stat-wide, .stat-value, .stat-label` | 148-173 |
| Settings | `.config-group, .config-label, .config-slider, .config-value` | 175-210 |
| Rules | `.rules-list, .rule-item, .rule-pattern, .rule-meta` | 212-222 |
| Main Content | `.main-content` | 224-231 |
| Query | `.query-section, .query-box, .query-input, .query-actions, .query-hint` | 233-261 |
| Results | `.results-section, .hidden` | 263-265 |
| Score | `.score-overview, .gauge-container, .gauge-center, .gauge-value, .gauge-label, .score-bars, .score-bar-*` | 267-333 |
| Output | `.content-panel, .output-content` | 335-353 |
| Reasoning Chain | `.reasoning-chain-panel, .chain-container, .chain-empty, .chain-step, .chain-step-*, .score-progression` | 355-500 |
| Timeline | `.timeline-panel, .timeline, .timeline-item, .timeline-step, .timeline-content, .timeline-agent, .timeline-detail` | 538-579 |
| Loading | `.loading-overlay, .spinner, .loading-text` | 581-607 |
| Modal | `.modal-overlay, .modal, .modal-lg, .modal-xl, .modal-header, .modal-title, .modal-close, .modal-body, .modal-footer` | 609-651 |
| Provider Grid | `.provider-grid, .provider-card, .provider-icon` | 653-674 |
| Forms | `.form-group, .form-label, .form-input, .form-hint, .eg-hint, .form-inline, .toggle, .toggle-track` | 676-720 |
| Tool Results | `.tool-results-panel, .tool-results-list, .tool-result-item` | 695-699 |
| Copy Button | `.copy-btn, .copied` | 503-536 |
| Rule Editor | `.rule-list-container, .rule-list-header, .rule-items, .rule-list-item, .rule-editor, .rule-editor-*, .rule-weights` | 727-790 |
| Responsive | `@media (max-width: 900px)` | 792-797 |

### 🎯 需要新增的样式模块

1. **流程图样式**:
   ```css
   .flowchart-panel { ... }
   .flowchart-container { ... }
   .flowchart-node { /* agent 节点 */ }
   .flowchart-node.active { /* 高亮当前 agent */ }
   .flowchart-node.done { /* 已完成 agent */ }
   .flowchart-edge { /* 连接线 */ }
   .flowchart-iteration-label { /* 迭代标记 */ }
   ```

2. **会话历史样式**:
   ```css
   .sessions-panel { ... }
   .sessions-list { max-height: 200px; overflow-y: auto; }
   .session-item { /* 单个会话项 */ }
   .session-item.active { /* 当前会话高亮 */ }
   .session-name { ... }
   .session-meta { ... }
   ```

3. **实时状态指示器样式**:
   ```css
   .processing-status { ... }
   .status-dot { animation: pulse 1.5s infinite; }
   .status-text { ... }
   .status-phase { ... }
   ```

4. **滚动条微调** — 侧边栏会话列表和流程图可能需要自定义滚动条

5. **保留所有现有样式** — 仅追加, 不修改现有 class

---

## 5. core.py — process_query 流程和 agent callback 机制

### CLMAFramework 类结构

**初始化流程** (__init__, 行 96-157):
```
RuleEngine() → TokenMonitor() → LoopController() → Orchestrator()
  ↓
Load rules from YAML
  ↓
Wire components: orchestrator.set_rule_engine/loop_controller/token_monitor
  ↓
PluginManager setup
  ↓
ToolExecutor setup
  ↓
_register_default_agents()
```

**Agent 回调注册** (_register_default_agents, 行 320-387):
```
make_agent_callback(agent_name) → closure
  ↓
orchestrator.register_agent(AgentType.REFINER, cb)
orchestrator.register_agent(AgentType.REASONER, cb)
orchestrator.register_agent(AgentType.SOLVER, cb)
orchestrator.register_agent(AgentType.VERIFIER, cb)
orchestrator.register_agent(AgentType.EVALUATOR, cb)
```

每个 callback 签名: `callback(query, method)` → `AgentResult`
- 先尝试 LLM 调用 (_llm_agent_call)
- 失败后回退到模拟 (_simulated_agent_call)
- **solver callback** 额外执行 _auto_execute_code() 自动运行代码

**核心流程** (process_query, 行 420-430):
```
1. 清空 _agent_memory 和 _tool_results
2. orchestrator.process_query(query)  ← C++ 侧执行主循环
3. _format_result(result)  ← Python 侧格式化
```

**C++ Orchestrator::processQuery** 内部循环 (Orchestrator.cpp 行 164-306):
```
do {
  1. refineQuery(user_query, rule)           → calls agents_[REFINER]
  2. executeIteration(user_query, rule, refined)  → 内部3步:
     a. reasonSolution(refined, rule)        → calls agents_[REASONER]
     b. executeSolution(reasoned, rule)       → calls agents_[SOLVER]  
     c. verifySolution(executed, rule)        → calls agents_[VERIFIER]
  3. evaluateResult(verified, rule)          → calls agents_[EVALUATOR]
  4. 记录 token 用量
  5. 更新最佳结果
  6. 记录执行历史
  7. 检查是否满足阈值 → 满足则 break
} while (shouldContinue())
```

### 🎯 SSE 集成点分析

**关键问题**: C++ Orchestrator::processQuery 是同步阻塞的, 无法在中间插入 Python SSE yield。

**解决方案 (3 种方案)**:

**方案 A (推荐): Python 侧模拟循环, 绕开 C++ Orchestrator**
- 在 core.py 中新增 `process_query_stream()` 方法
- 不调用 `orchestrator.process_query(query)`, 而是手动在 Python 侧循环:
  ```python
  def process_query_stream(self, query):
      rule = self.rule_engine.getBestRule(query)
      for iteration in range(max_iterations):
          # 手动调用每个 agent callback, 每个之间 yield
          for agent_name in [refiner, reasoner, solver, verifier, evaluator]:
              yield {'event': 'agent_start', 'agent': agent_name, 'iteration': iteration}
              result = agent_callbacks[agent_name](input, method)
              yield {'event': 'agent_complete', ...}
          # 检查阈值
          yield {'event': 'iteration', ...}
          if score.meetsThreshold: break
      yield {'event': 'done', 'result': final_result}
  ```
- **优点**: 完全控制, 每步可 yield
- **缺点**: 绕过 C++ orchestrator, 失去 C++ 性能优势

**方案 B: 在 web_app.py 中用线程包装 C++ 调用**
- 在单独的线程中运行 `orchestrator.process_query(query)`
- 通过 callback 钩子把中间状态推送到线程安全的队列
- web_app.py 的 SSE 生成器从队列中读取事件
- **优点**: 保留 C++ 核心
- **缺点**: 需要修改 C++ Orchestrator 添加回调钩子, 复杂度高

**方案 C (折中): 使用 Python 层面的 orchestrator 模拟**
- 保留 C++ orchestrator 的组件 (rule_engine, loop_controller, token_monitor)
- 在 Python 层重新实现主循环逻辑, 每个迭代步骤调用 C++ callback
- 实际上等价于方案 A, 但复用 C++ 组件

**推荐方案 A** — 因为:
1. C++ orchestrator 的 processQuery 是自包含的, 没有中间回调机制
2. 修改 C++ 代码会增加构建复杂度
3. Python 侧模拟可以保持与现有 callback 机制完全兼容
4. 可以保持原有 C++ processQuery 作为后备/同步模式

---

## 6. plugins.html — 独立页面模式参考

### 页面结构特点
- 独立 HTML 页面, 非 SPA 内嵌
- 使用 `/plugins` 路由
- 共享 style.css (通过 link 引入)
- 内联 <style> 添加插件特定样式 (不污染主样式)
- 内联 <script> 添加插件 JS (不污染主 JS)
- 使用 grid 布局展示卡片

### 可借鉴的模式
1. **内联 JS 模式**: 独立功能页面的 JS 直接写在 HTML template 中, 避免全局污染
2. **CSS 隔离**: 通过特定前缀 (`.plugins-page`, `.plugin-card`) 隔离样式
3. **Toast 通知系统**: plugins.html 实现了自己的 toast 组件, 可复用
4. **slide-in detail panel**: 右侧滑入详情面板, 可用于流程图放大/详情
5. **状态管理**: `let pluginsData = []` 局部变量, 不依赖全局状态
6. **简洁的事件流程**: DOMContentLoaded → loadPlugins → renderSummary + renderGrid

### 与主 SPA 的集成方式
- plugins 作为独立页面, 通过 header 中的 `<a href="/plugins">` 导航
- 返回主页使用 header 中的 `<a href="/">`
- **Phase 7 建议保持这种独立页面模式** — 插件管理是 admin 功能, 无需放在 SPA 中
- 如果未来需要, 可以在侧边栏添加插件状态指示器 (从 `/api/plugins` 获取摘要)

---

## 7. 会话存储后端设计建议

### 数据模型
```python
# 会话文件: config/sessions/{session_id}.json
{
    "id": "uuid-string",
    "name": "Session 1",
    "created_at": "2026-04-28T11:23:00Z",
    "updated_at": "2026-04-28T11:25:00Z",
    "query_count": 3,
    "messages": [
        {
            "id": "msg-uuid",
            "role": "user",
            "query": "write a python script...",
            "timestamp": "..."
        },
        {
            "id": "msg-uuid",
            "role": "assistant",
            "query": "write a python script...",
            "result": { ... },  # 完整 result payload
            "scores": { ... },
            "stats": { ... },
            "timestamp": "...",
            "duration_ms": 1234
        }
    ]
}
```

### API 设计
```python
# web_app.py 新增路由

@app.route('/api/sessions', methods=['GET'])
def api_sessions():
    """List all sessions, ordered by updated_at desc."""
    # 返回摘要列表 (不含 messages)
    return jsonify({"sessions": [...]})

@app.route('/api/sessions', methods=['POST'])
def api_create_session():
    """Create a new session."""
    data = request.get_json() or {}
    name = data.get('name', f"Session {len(sessions)+1}")
    # 创建 session_id, 写入文件
    return jsonify({"session": {...}})

@app.route('/api/sessions/<session_id>', methods=['GET'])
def api_get_session(session_id):
    """Get full session data (with messages)."""
    return jsonify({"session": {...}})

@app.route('/api/sessions/<session_id>', methods=['DELETE'])
def api_delete_session(session_id):
    """Delete a session."""
    os.remove(path)
    return jsonify({"status": "deleted"})

@app.route('/api/sessions/<session_id>/rename', methods=['POST'])
def api_rename_session(session_id):
    data = request.get_json()
    new_name = data.get('name', 'Session')
    # Update name in file
    return jsonify({"status": "renamed", "name": new_name})

@app.route('/api/sessions/<session_id>/messages', methods=['POST'])
def api_add_message(session_id):
    """Append a message (user query or assistant result) to session."""
    data = request.get_json()
    # Append to messages list, update query_count, updated_at
    return jsonify({"status": "added", "message_id": msg_id})
```

---

## 8. 总结: 修改清单

| 文件 | 修改类型 | 内容 |
|------|----------|------|
| web_app.py | **新增** | SSE 端点 /api/process/stream |
| web_app.py | **新增** | 会话存储 API (6 个端点) |
| web_app.py | **保留** | 所有现有 API 端点 |
| core.py | **新增** | process_query_stream() 生成器 |
| core.py | **保留** | 现有 process_query() 作为同步后备 |
| index.html | **新增** | 侧边栏: sessions-panel |
| index.html | **新增** | resultsSection: flowchart-panel |
| index.html | **新增** | header 或 sideba: processing-status |
| index.html | **保留** | 所有现有 sections/modals |
| app.js | **修改** | submitQuery() → SSE 连接 |
| app.js | **新增** | SSE 事件处理 (6 种事件) |
| app.js | **新增** | 会话管理函数 (5 个) |
| app.js | **新增** | 流程图渲染函数 |
| app.js | **新增** | 实时状态更新函数 |
| app.js | **移除** | loadingOverlay 手动 show/hide |
| app.js | **保留** | 所有现有函数 (displayResults, renderTimeline, 等) |
| style.css | **新增** | 流程图样式 |
| style.css | **新增** | 会话历史样式 |
| style.css | **新增** | 状态指示器样式 |
| style.css | **保留** | 所有现有样式 |
