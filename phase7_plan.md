# Phase 7: UI 大重构 — 可视化推理流程图 + 对话历史侧边栏 + 实时推流

> **目标:** 将当前"转圈圈等待+静态输出"的 UI 改造为"实时可视化推理流程图+对话历史侧边栏"的交互式界面。参考自动化课程系统框图风格——方块表示每个 agent，执行状态在框内可见，信息传递到下一个 agent 也可见。

**设计原则:**
- 保留现有所有 API 端点（向后兼容）
- 不修改 C++ 核心代码（仅 Python + 前端）
- SSE 推流代替转圈圈，流程图实时渲染
- 会话持久化存储，侧边栏切换历史
- 输出内容完整无截断

---

## 任务分解（共 24 个子任务）

### Task 0: 后端基础设施 — SSE 事件流引擎

**Objective:** 在 `core.py` 中新增 `process_query_stream()` 生成器方法，将同步阻塞的 C++ orchestrator 调用替换为 Python 层手动循环，每步调用 agent callback 后 yield SSE 事件。

**Files:**
- Modify: `python_interface/core.py` — 新增 `process_query_stream()` 方法 (~100行)
- Keep: 现有 `process_query()` 方法作为同步后备

**Key design:**
```python
def process_query_stream(self, query):
    """Generator that yields SSE events during processing."""
    # 1. 清空 agent_memory, tool_results
    # 2. 获取 rule
    # 3. 循环:
    #    a. yield {'event': 'agent_start', 'agent': name, 'iteration': N}
    #    b. 调用 agent callback
    #    c. yield {'event': 'agent_complete', 'agent': name, 'content_preview': ..., 'duration_ms': ...}
    #    d. solver 时执行 _auto_execute_code，yield 'tool_execution' 事件
    #    e. 所有 5 个 agent 完成后 yield {'event': 'iteration', 'iteration': N, 'scores': {...}}
    #    f. 检查阈值，满足则 break
    # 4. yield {'event': 'done', 'result': final_result, 'history': history}
    # 5. 异常时 yield {'event': 'error', 'message': ...}
```

**Details:**
1. 复用 `_agent_memory` 和 `_tool_results`（与现有的 `process_query` 一致）
2. 调用 `self.orchestrator.refineQuery(user_query, rule)` 获取精炼查询（调用 REFINER agent）
3. 手动循环调用 `reasonSolution` → `executeSolution` → `verifySolution` → `evaluateResult`
4. 每个步骤之间 yield 事件
5. 读取 `orchestrator.getExecutionHistory()` 在 yield 'done' 时带上
6. 使用 `time.perf_counter()` 计算各阶段的耗时

**Verification:**
```
python3 -c "
from python_interface.core import CLMAFramework
fw = CLMAFramework()
gen = fw.process_query_stream('写一个Python排序函数')
for event in gen:
    print(event['event'], '-', event.get('agent', ''))
" 2>&1 | head -20
```

---

### Task 1: 后端基础设施 — SSE Flask 端点

**Objective:** 在 `web_app.py` 中新增 `GET /api/process/stream` 端点，使用 Flask SSE 模式推送事件。

**Files:**
- Modify: `python_interface/web_app.py` — 新增 SSE 路由 (~40行)
- Import: `from flask import stream_with_context, Response`

**Details:**
```python
@app.route('/api/process/stream')
def api_process_stream():
    query = request.args.get('query', '').strip()
    if not query:
        return jsonify({'error': 'Query required'}), 400
    
    fw = get_framework()
    
    def generate():
        for event in fw.process_query_stream(query):
            yield f"event: {event['event']}\ndata: {json.dumps(event)}\n\n"
    
    return Response(
        stream_with_context(generate()),
        mimetype='text/event-stream',
        headers={
            'Cache-Control': 'no-cache',
            'Connection': 'keep-alive',
            'X-Accel-Buffering': 'no',
        }
    )
```

**Event types sent to frontend:**
| event | data payload |
|-------|-------------|
| `agent_start` | `{agent, iteration, timestamp}` |
| `agent_complete` | `{agent, iteration, content_preview: string, duration_ms, success}` |
| `tool_execution` | `{tool_name, success, exit_code, stdout_preview, stderr_preview, duration_ms}` |
| `iteration` | `{iteration, scores: {overall, reasonableness, executability, satisfaction}, best_so_far}` |
| `done` | `{result, history, stats, mode, session_id}` |
| `error` | `{message, iteration}` |

**Verification:**
```
curl -N -s "http://127.0.0.1:5000/api/process/stream?query=test" | head -20
```
Expected: stream of SSE events, ending with `event: done`

---

### Task 2: 后端基础设施 — 会话存储 API

**Objective:** 在 `web_app.py` 中新增 6 个会话管理 REST API 端点，使用 `config/sessions/` 目录下的 JSON 文件持久化。

**Files:**
- Create: `python_interface/session_store.py` — 会话数据模型和文件操作封装 (~100行)
- Modify: `python_interface/web_app.py` — 新增 6 个路由 (~80行)

**session_store.py design:**
```python
import os, json, uuid, time
SESSIONS_DIR = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'config', 'sessions')

def ensure_dir():
    os.makedirs(SESSIONS_DIR, exist_ok=True)

def list_sessions():
    """Return list of session summaries (no messages)."""
    ...

def create_session(name="New Session"):
    """Create a new session file, return session dict."""
    ...

def get_session(session_id):
    """Return full session (with messages)."""
    ...

def delete_session(session_id):
    """Remove session file."""
    ...

def rename_session(session_id, new_name):
    """Update session name."""
    ...

def add_message(session_id, role, **data):
    """Append a user query or assistant result message."""
    ...
```

**Session data structure:**
```json
{
    "id": "sess_abc123",
    "name": "Python Sorting",
    "created_at": 1234567890.0,
    "updated_at": 1234567895.0,
    "query_count": 2,
    "messages": [
        {
            "id": "msg_001",
            "role": "user",
            "query": "写一个排序函数",
            "timestamp": 1234567890.0
        },
        {
            "id": "msg_002",
            "role": "assistant",
            "query": "写一个排序函数",
            "result": { ... },
            "scores": { ... },
            "stats": { ... },
            "duration_ms": 1234,
            "timestamp": 1234567895.0
        }
    ]
}
```

**API Routes:**
| Method | Endpoint | Function |
|--------|----------|----------|
| GET | `/api/sessions` | `api_sessions()` — list summaries |
| POST | `/api/sessions` | `api_create_session()` — create new |
| GET | `/api/sessions/<id>` | `api_get_session()` — full detail |
| DELETE | `/api/sessions/<id>` | `api_delete_session()` — remove |
| POST | `/api/sessions/<id>/rename` | `api_rename_session()` — rename |
| POST | `/api/sessions/<id>/messages` | `api_add_message()` — add message |

**Verification:**
```
curl -s -X POST http://127.0.0.1:5000/api/sessions | python3 -m json.tool
curl -s http://127.0.0.1:5000/api/sessions | python3 -m json.tool
```

---

### Task 3: 前端 — 侧边栏会话历史面板

**Objective:** 在 `index.html` 的侧边栏中添加会话历史面板 DOM 结构，在 `app.js` 中添加会话管理函数。

**Files:**
- Modify: `python_interface/templates/index.html` — 侧边栏插入 sessions-panel (~50行)
- Modify: `python_interface/static/app.js` — 新增会话管理函数 (~120行)
- Modify: `python_interface/static/style.css` — 新增会话样式 (~80行)

**index.html changes — 在 sidebar 内 stats-panel 上方插入:**
```html
<section class="panel sessions-panel">
  <div class="panel-header">
    <h3 class="panel-title">Sessions</h3>
    <span class="badge" id="sessionsCount">0</span>
    <button class="btn btn-sm btn-ghost" onclick="createSession()" title="New Session">
      <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
        <line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>
      </svg>
    </button>
  </div>
  <div class="sessions-list" id="sessionsList">
    <div class="sessions-empty">No saved sessions</div>
  </div>
</section>
```

**CSS styles to add:**
- `.sessions-panel` — 限制最大高度，可折叠
- `.sessions-list` — 滚动容器，max-height: 180px
- `.session-item` — 会话卡片，flex 布局，hover 高亮
- `.session-item.active` — 当前会话蓝色边框
- `.session-name` — 单行截断，font-weight 500
- `.session-meta` — 小字显示 query count + 时间
- `.session-delete` — 悬停显示的删除按钮
- `.sessions-empty` — 空状态提示

**JS functions to add:**
```javascript
// State
let sessions = [];
let currentSessionId = null;

// Load session list from API
async function loadSessions() { ... }

// Create new session
async function createSession() { ... }

// Switch to a session (load its messages into view)
function switchSession(sessionId) { ... }

// Delete a session
async function deleteSession(sessionId, event) { ... }

// After a process completes, save results to current session
async function saveToSession(query, result, stats, durationMs) { ... }

// Format relative time (e.g., "2m ago")
function timeAgo(timestamp) { ... }
```

**Verification of DOM:**
```bash
curl -s http://127.0.0.1:5000/ | grep -c 'sessions-panel'
# Expected: 1
```

---

### Task 4: 前端 — 可视化推理流程图

**Objective:** 在 resultsSection 中插入推理流程图面板，用 CSS/SVG 绘制 5 个 agent 方块和连接箭头，实时高亮当前执行 agent。

**Files:**
- Modify: `python_interface/templates/index.html` — 插入 flowchart-panel DOM (~30行)
- Modify: `python_interface/static/app.js` — 新增流程图渲染函数 (~150行)
- Modify: `python_interface/static/style.css` — 新增流程图样式 (~120行)

**Flowchart design (SVG-based, not canvas):**
```
     ┌──────────┐     ┌──────────┐     ┌──────────┐
     │ REFINER  │     │ REASONER │     │  SOLVER  │
     │ [state]  │ ──► │ [state]  │ ──► │ [state]  │
     │ score: X │     │ idea: Y  │     │ code: Z  │
     └──────────┘     └──────────┘     └──────────┘
          ▲                                  │
          │           ┌──────────┐           │
          │           │EVALUATOR │           │
          ├───────────│ [state]  │◄──────────┘
          │           │ score: W │
          │           └──────────┘
          │                │
     ┌────┴────┐     ┌─────▼─────┐
     │  LOOP   │     │ VERIFIER  │
     │ cnt: N  │     │ [state]   │
     │ th: X.Y │     │ result: V │
     └─────────┘     └───────────┘
```

**index.html — 在 score-overview 下方, content-panel 上方插入:**
```html
<div id="flowchartPanel" class="panel flowchart-panel hidden">
  <h3 class="panel-title">Agent Flow</h3>
  <div class="flowchart-container" id="flowchartContainer">
    <div class="flowchart-empty">Execute a query to see the agent flow.</div>
  </div>
</div>
```

**JS rendering approach:**
Use dynamically created SVG inside `#flowchartContainer`:
```javascript
function renderFlowchart() {
    // Create SVG with viewBox
    // Position 5 agent nodes in a flow layout
    // Each node = rect + text label + status indicator + content preview
    // Arrows between nodes
}
function updateFlowNode(agentName, status, data) {
    // status: 'idle' | 'active' | 'done' | 'error'
    // Update fill color of the node rect
    // Update status indicator and content preview text
    // Animate arrow for active transition
}
```

**Nodes layout (positions in SVG):**
- Refiner: 左上 (x=50, y=20)
- Reasoner: 中上 (x=250, y=20)
- Solver: 右上 (x=450, y=20)
- Verifier: 中下 (x=450, y=230)
- Evaluator: 左下 (x=250, y=230)
- Loop back arrow from Evaluator → Refiner
- Arrow down from Solver → Verifier

**CSS states:**
- `.fn-node` — base rect: fill #1a2035, stroke #2a3550, rx=8
- `.fn-node.active` — fill #1a2a44, stroke #4a6cf7, box-shadow glow
- `.fn-node.done` — stroke #06d6a0, checkmark icon
- `.fn-node.error` — stroke #ef476f, X icon
- `.fn-arrow` — line marker-end, color #2a3550
- `.fn-arrow.active` — color #4a6cf7, animated dash

**Verification:**
```
curl -s http://127.0.0.1:5000/ | grep -c 'flowchart-panel'
# Expected: 1
```

---

### Task 5: 前端 — 实时状态指示器 (替换转圈圈)

**Objective:** 移除全屏 `loadingOverlay` 的手动控制，改为内联的 processing status 指示器 + 流程图实时更新。

**Files:**
- Modify: `python_interface/templates/index.html` — 插入 processingStatus DOM (~20行)
- Modify: `python_interface/static/app.js` — 重写 submitQuery() 为 SSE 版本 (~100行)
- Modify: `python_interface/static/style.css` — 新增状态指示器样式 (~50行)

**index.html — 在 header actions 右侧或 query-section 上方插入:**
```html
<div id="processingStatus" class="processing-status hidden">
  <div class="status-indicator">
    <span class="status-pulse"></span>
    <span class="status-text" id="statusText">Processing</span>
  </div>
  <span class="status-detail" id="statusDetail">
    <span id="statusAgent">Refiner</span>
    <span class="status-sep">·</span>
    Iteration <span id="statusIteration">1</span>
  </span>
  <div class="status-bar" id="statusBar">
    <div class="status-bar-fill" id="statusBarFill"></div>
  </div>
</div>
```

**app.js — 重写 submitQuery():**
```javascript
async function submitQuery() {
  if (processing) return;
  const query = document.getElementById('queryInput').value.trim();
  if (!query) return;
  
  processing = true;
  document.getElementById('submitBtn').disabled = true;
  
  // Show processing status instead of loading overlay
  showProcessingStatus();
  
  // Close any existing SSE connection
  if (currentEventSource) currentEventSource.close();
  
  const url = '/api/process/stream?query=' + encodeURIComponent(query);
  currentEventSource = new EventSource(url);
  
  currentEventSource.addEventListener('agent_start', (e) => {
    const data = JSON.parse(e.data);
    updateFlowNode(data.agent, 'active', {});
    updateStatusText(data.agent, data.iteration);
  });
  
  currentEventSource.addEventListener('agent_complete', (e) => {
    const data = JSON.parse(e.data);
    updateFlowNode(data.agent, 'done', { content_preview: data.content_preview });
    // Also append to timeline
    appendTimelineEntry(data);
  });
  
  currentEventSource.addEventListener('tool_execution', (e) => {
    const data = JSON.parse(e.data);
    appendToolResult(data);
  });
  
  currentEventSource.addEventListener('iteration', (e) => {
    const data = JSON.parse(e.data);
    updateFlowChartScores(data.scores);
    updateScoreBars(data.scores);
  });
  
  currentEventSource.addEventListener('done', (e) => {
    const data = JSON.parse(e.data);
    currentEventSource.close();
    currentEventSource = null;
    hideProcessingStatus();
    displayResults(data);  // reuse existing displayResults
    saveToSession(query, data.result, data.stats, data.duration_ms);
    processing = false;
    document.getElementById('submitBtn').disabled = false;
  });
  
  currentEventSource.addEventListener('error', (e) => {
    // SSE connection error or server-sent error event
    const data = e.data ? JSON.parse(e.data) : {};
    currentEventSource.close();
    currentEventSource = null;
    hideProcessingStatus();
    showError(data.message || 'Connection lost');
    processing = false;
    document.getElementById('submitBtn').disabled = false;
  });
}
```

**CSS styles:**
- `.processing-status` — inline flex container, padding, border
- `.status-pulse` — 8px circle with pulse animation (CSS keyframes)
- `.status-text` — "Processing" text
- `.status-detail` — current agent + iteration info
- `.status-bar` — thin progress bar (100% width, 3px height)
- `.status-bar-fill` — animated fill, indeterminate animation

**Verification:**
```bash
# Start Flask, then test SSE
curl -N -s "http://127.0.0.1:5000/api/process/stream?query=test" | head -5
# Should see: event: agent_start / event: agent_complete / ... / event: done
```

---

### Task 6: 输出完整显示修复

**Objective:** 确保 `outputContent` 中展示的内容完整无截断，且所有 agent 阶段的输出通过流程图和 detail 面板可见。

**Files:**
- Modify: `python_interface/static/style.css` — 调整 outputContent CSS
- Modify: `python_interface/static/app.js` — 确保 displayResults 显示完整 content

**CSS fix:**
```css
.output-content {
  max-height: none;        /* 移除高度限制 */
  overflow-y: visible;      /* 不截断 */
}
/* 或者在面板上加一个高度限制，让面板本身滚动 */
.flowchart-panel + .content-panel {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
}
```

**JS enhancement — 在 displayResults 中确保 content 完整:**
```javascript
const output = document.getElementById('outputContent');
output.textContent = result.content || '[No output]';
// 确保滚动到底部可见
output.scrollTop = output.scrollHeight;
```

**Verification:**
```
curl -s -X POST http://127.0.0.1:5000/api/process -H "Content-Type: application/json" -d '{"query":"写一个Python函数"}' | python3 -c "
import json,sys
d=json.load(sys.stdin)
c=d['result']['content']
print(len(c))
# 确认不是截断的
assert not c.endswith('['), 'truncated!'
assert len(c) > 100, 'too short!'
"
```

---

### Task 7: 集成测试 — 全流程验证

**Objective:** 启动 Flask 服务器，通过 SSE 发查询，验证所有事件按序到达，流程图实时更新，会话存储正常。

**Steps:**
1. 重启 Flask 服务器
2. 用 `curl -N` 测试 SSE 端点
3. 用 curl 测试会话 API（CRUD）
4. 用 `curl -s http://127.0.0.1:5000/` 验证 HTML 包含所有新面板
5. 用 `curl -s http://127.0.0.1:5000/static/app.js | grep -c 'EventSource'` 确认 SSE 代码
6. 用 `curl -s http://127.0.0.1:5000/static/style.css | grep -c 'flowchart-node` 确认 CSS
7. 验证所有现有 API 端点仍然工作（/api/status, /api/settings, 等）
8. 验证插件页面仍然工作

---

## 文件修改清单总结

| 文件 | 修改类型 | 行数预估 |
|------|----------|----------|
| `python_interface/core.py` | **新增** process_query_stream() | +100行 |
| `python_interface/web_app.py` | **新增** SSE 端点 + 6 个会话路由 | +120行 |
| `python_interface/session_store.py` | **新建** 会话文件存储模块 | +100行 |
| `python_interface/templates/index.html` | **新增** sessions-panel + flowchart-panel + processingStatus | +100行 |
| `python_interface/static/app.js` | **修改** submitQuery→SSE + **新增** 会话管理 + 流程图 | +200行 |
| `python_interface/static/style.css` | **新增** 流程图 + 会话 + 状态指示器样式 | +250行 |

**总预估**: ~870 行新增/修改代码

## 保留不变的部分

- C++ 核心代码（Orchestrator.cpp, PluginManager, 所有 include/）
- plugins.html 独立页面
- 所有现有 API 端点 (17个)
- 所有现有 JS 函数（displayResults, renderTimeline, drawGauge, 所有 modal 等）
- 所有现有 CSS class（不修改现有 class，只追加新 class）
- 现有 process_query() 方法（保留作同步后备）

---

## 检查清单

- [ ] process_query_stream() 生成器正确发出 7 种事件
- [ ] SSE 端点 /api/process/stream 正常工作
- [ ] 会话 CRUD API 全部正常工作
- [ ] 侧边栏会话列表正确渲染
- [ ] 流程图显示 5 个 agent 节点 + 连接箭头
- [ ] 流程图实时高亮当前 agent（active/done/error 状态）
- [ ] 前端用 SSE 替代 fetch POST，无 loadingOverlay
- [ ] 状态指示器实时显示当前 agent + 迭代
- [ ] outputContent 完整显示无截断
- [ ] 会话保存/加载/切换功能正常
- [ ] 所有现有 API 端点仍然工作
- [ ] 插件页面仍然工作
- [ ] 原有 10 个 C++ 测试套件仍然通过
