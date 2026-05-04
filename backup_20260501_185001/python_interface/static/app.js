/**
 * CLMA Framework — Frontend Application
 * Handles all UI interactions, modal logic, and API communication.
 */

// === State ===
let processing = false;
let currentRules = [];
let editingRuleIndex = -1;
// Dynamic provider catalog — populated from /api/llm-catalog on modal open
let llmCatalog = { providers: {}, categories: {}, api_types: {} };

// === Toast ===
function showToast(message, type = 'success') {
  const el = document.getElementById('toast');
  const msgEl = document.getElementById('toastMessage');
  if (!el || !msgEl) return;
  msgEl.textContent = message;
  el.className = 'toast toast-' + type;
  el.classList.remove('hidden');
  setTimeout(() => { el.classList.add('hidden'); }, 3000);
}

// === Init ===
document.addEventListener('DOMContentLoaded', () => {
  loadStatus();
  loadRules();
  refreshSessionList();

  // Keyboard shortcut
  document.getElementById('queryInput').addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.key === 'Enter') submitQuery();
  });

  // Auto-update config value display
  var configIterations = document.getElementById('configIterations');
  if (configIterations) {
    configIterations.addEventListener('input', function() {
      var valEl = document.getElementById('configIterationsVal');
      if (valEl) valEl.textContent = this.value;
    });
  }
  var configThreshold = document.getElementById('configThreshold');
  if (configThreshold) {
    configThreshold.addEventListener('input', function() {
      var valEl = document.getElementById('configThresholdVal');
      if (valEl) valEl.textContent = parseFloat(this.value).toFixed(2);
    });
  }

  // Load saved settings values from backend
  api('/api/settings').then(settings => {
    var timeoutSlider = document.getElementById('configTimeout');
    var timeoutVal = document.getElementById('configTimeoutVal');
    if (timeoutSlider && settings.execution_timeout) {
      timeoutSlider.value = settings.execution_timeout;
      if (timeoutVal) timeoutVal.textContent = settings.execution_timeout + 's';
    }
    var iterSlider = document.getElementById('configIterations');
    var iterVal = document.getElementById('configIterationsVal');
    if (iterSlider && settings.max_iterations !== undefined) {
      iterSlider.value = settings.max_iterations;
      if (iterVal) iterVal.textContent = settings.max_iterations;
    }
    var thresSlider = document.getElementById('configThreshold');
    var thresVal = document.getElementById('configThresholdVal');
    if (thresSlider && settings.threshold !== undefined) {
      thresSlider.value = settings.threshold;
      if (thresVal) thresVal.textContent = parseFloat(settings.threshold).toFixed(2);
    }
    // Load DAG state
    var dagCheckbox = document.getElementById('configDag');
    var dagLabel = document.getElementById('configDagLabel');
    if (dagCheckbox && settings.dag_mode !== undefined) {
      dagCheckbox.checked = settings.dag_mode;
      if (dagLabel) dagLabel.textContent = settings.dag_mode ? 'On' : 'Off';
    }
    // Load architecture mode
    if (settings.arch_mode) {
      document.querySelectorAll('.arch-btn').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.arch === settings.arch_mode);
      });
      currentArchMode = settings.arch_mode;
      updateFlowchartArchBadge(settings.arch_mode);
    }
  });

  // Timeout slider live display
  var configTimeout = document.getElementById('configTimeout');
  if (configTimeout) {
    configTimeout.addEventListener('input', function() {
      var valEl = document.getElementById('configTimeoutVal');
      if (valEl) valEl.textContent = this.value + 's';
    });
  }
});

// === API Calls ===
async function api(url, method = 'GET', body = null) {
  const opts = {
    method,
    headers: { 'Content-Type': 'application/json' },
  };
  if (body) opts.body = JSON.stringify(body);
  const resp = await fetch(url, opts);
  return resp.json();
}

// === Load Dashboard ===
async function loadStatus() {
  const data = await api('/api/status');
  updateStats(data.stats);
  updateMode(data.mode);
}

async function loadRules() {
  const data = await api('/api/rules');
  currentRules = data.rules || [];
  const list = document.getElementById('rulesList');
  if (!list) return;
  const count = document.getElementById('rulesCount');
  if (count) count.textContent = currentRules.length;
  list.innerHTML = currentRules.map(r => `
    <div class="rule-item">
      <div class="rule-pattern">/${r.pattern}/</div>
      <div class="rule-meta">${r.validation_method} · threshold ${r.threshold}</div>
    </div>
  `).join('');
}

// === Submit Query (SSE version) ===
let activeEventSource = null;
let queryCancelled = false;

async function submitQuery() {
  if (processing) return;

  const input = document.getElementById('queryInput');
  const query = input.value.trim();
  if (!query) return;

  processing = true;
  const btn = document.getElementById('submitBtn');
  btn.disabled = true;
  btn.innerHTML = '<div class="spinner" style="width:16px;height:16px;border-width:2px"></div> Processing...';

  // Close any previous SSE connection
  if (activeEventSource) {
    activeEventSource.close();
    activeEventSource = null;
  }

  // Show results section (empty state), show processing status, init flowchart
  const section = document.getElementById('resultsSection');
  section.classList.remove('hidden');

  const flowchartPanel = document.getElementById('flowchartPanel');
  flowchartPanel.classList.remove('hidden');

  const processingStatus = document.getElementById('processingStatus');
  processingStatus.classList.remove('hidden');

  document.getElementById('statusAgentLabel').textContent = 'Starting...';
  document.getElementById('statusIteration').textContent = '1';
  document.getElementById('statusBarFill').style.width = '0%';

  // Init flowchart with idle nodes
  if (currentArchMode === 'multi') {
    initMultiLoopFlowchart();
  } else {
    initFlowchart();
  }

  let agentSequence = ['refiner', 'reasoner', 'solver', 'verifier', 'evaluator'];
  let latestIteration = 1;
  let allEvents = [];
  let accumulatedResult = null;

  // Build SSE URL
  const baseUrl = window.location.origin;
  const sseUrl = baseUrl + '/api/process/stream?query=' + encodeURIComponent(query);

  activeEventSource = new EventSource(sseUrl);

  if (currentArchMode === 'multi') {
    // ===== Multi-Loop mode: 独立 SSE 事件分发 =====
    let outerIter = 0;
    const multiAgentOrder = ['strategy_refiner', 'strategy_reasoner', 'execution_loop', 'outer_verifier', 'outer_evaluator'];

    activeEventSource.addEventListener('agent_start', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const agent = data.agent;
      const iteration = data.outer_iteration || data.iteration || 1;
      const label = data.agent_label || agent;

      // Update processing status
      document.getElementById('statusAgentLabel').textContent = label;
      document.getElementById('statusIteration').textContent = 'Outer #' + iteration;
      document.getElementById('statusBarFill').style.width = '0%';

      // Update multi flowchart
      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('agent_complete', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const agent = data.agent;
      const success = data.success !== false;
      const duration = data.duration_ms || 0;
      const preview = data.content_preview || '';

      // Progress bar: advanced based on outer agent position
      const multiIdx = multiAgentOrder.indexOf(agent);
      if (multiIdx >= 0) {
        const pct = ((multiIdx + 1) / multiAgentOrder.length) * 100;
        document.getElementById('statusBarFill').style.width = Math.min(pct, 95) + '%';
      }

      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('tool_execution', function(e) {
      // Same tool_execution rendering for both modes
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const panel = document.getElementById('toolResultsPanel');
      panel.classList.remove('hidden');
      const list = document.getElementById('toolResultsList');
      const count = document.getElementById('toolResultsCount');
      const icon = data.success ? '<span style="color:#06d6a0">✓</span>' : '<span style="color:#ef476f">✗</span>';
      const durationMs = data.duration_ms || 0;
      const item = document.createElement('div');
      item.className = 'tool-result-item';
      item.style.cssText = 'margin-bottom:8px;padding:8px 12px;background:rgba(255,255,255,0.03);border-radius:6px;border:1px solid rgba(255,255,255,0.06);animation:slideDown 0.2s ease';
      item.innerHTML = `
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px">
          ${icon}
          <span style="font-size:13px;font-weight:500;color:#ccd6f6">${data.tool_name || 'tool'}</span>
          <span style="font-size:11px;color:#8892b0">exit ${data.exit_code} · ${durationMs.toFixed(0)}ms</span>
        </div>
        <pre class="mono" style="font-size:11px;color:#a8b2d1;margin:0;white-space:pre-wrap;max-height:60px;overflow-y:auto">${data.stdout_preview || '(no stdout)'}</pre>
      `;
      list.appendChild(item);
      count.textContent = list.children.length;
    });

    activeEventSource.addEventListener('inner_loop_start', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      document.getElementById('statusAgentLabel').textContent = 'Execution Loop (Inner)';
      document.getElementById('statusBarFill').style.width = '30%';
      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('inner_iteration', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const iIter = data.inner_iteration || '?';
      document.getElementById('statusAgentLabel').textContent = 'Inner Eval #' + iIter;
      if (data.scores) {
        animateBar('scoreBar1', 'scoreVal1', data.scores.reasonableness || 0);
        animateBar('scoreBar2', 'scoreVal2', data.scores.executability || 0);
        animateBar('scoreBar3', 'scoreVal3', data.scores.satisfaction || 0);
        drawGauge(data.scores.overall || 0);
      }
      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('inner_done', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      document.getElementById('statusAgentLabel').textContent =
        'Inner Loop Done (best: ' + (data.best_score || 0).toFixed(2) + ')';
      document.getElementById('statusBarFill').style.width = '60%';
      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('iteration', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const scores = data.scores || { overall: 0, reasonableness: 0, executability: 0, satisfaction: 0 };
      drawGauge(scores.overall);
      animateBar('scoreBar1', 'scoreVal1', scores.reasonableness || 0);
      animateBar('scoreBar2', 'scoreVal2', scores.executability || 0);
      animateBar('scoreBar3', 'scoreVal3', scores.satisfaction || 0);
      document.getElementById('statusBarFill').style.width = '100%';
      updateMultiFlowchartFromSSE(data);
    });

    activeEventSource.addEventListener('done', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      accumulatedResult = data.result || {};
      const scores = accumulatedResult.score || { overall: 0 };

      drawGauge(scores.overall);
      animateBar('scoreBar1', 'scoreVal1', scores.reasonableness || 0);
      animateBar('scoreBar2', 'scoreVal2', scores.executability || 0);
      animateBar('scoreBar3', 'scoreVal3', scores.satisfaction || 0);

      const statusEl = document.getElementById('resultStatus');
      statusEl.className = 'result-status success';
      statusEl.textContent = accumulatedResult.success !== false
        ? '✓ Multi-Loop processed successfully'
        : '✗ Max outer iterations reached';

      const output = document.getElementById('outputContent');
      output.textContent = accumulatedResult.content || '[No output]';

      if (data.stats) updateStats(data.stats);
      if (data.mode) updateMode(data.mode);
      renderTimeline(data.history || []);
      document.getElementById('processingStatus').classList.add('hidden');
      refreshSessionList();

      processing = false;
      btn.disabled = false;
      btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
      activeEventSource.close();
      activeEventSource = null;
    });

    activeEventSource.addEventListener('error', function(e) {
      const data = JSON.parse(e.data || '{}');
      console.error('SSE error:', data.message || 'Connection error');
      document.getElementById('processingStatus').classList.add('hidden');
      if (accumulatedResult) {
        const output = document.getElementById('outputContent');
        output.textContent = (accumulatedResult.content || '') + '\n\n[Error: ' + (data.message || 'Stream interrupted') + ']';
        document.getElementById('resultStatus').className = 'result-status failure';
        document.getElementById('resultStatus').textContent = '✗ ' + (data.message || 'Processing error');
      }
      processing = false;
      btn.disabled = false;
      btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
      if (activeEventSource) {
        activeEventSource.close();
        activeEventSource = null;
      }
    });
  } else {
    // ===== Single-Loop mode: 原有 SSE 事件分发 =====
    activeEventSource.addEventListener('agent_start', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const agent = data.agent;
      const iteration = data.iteration || 1;
      const label = data.agent_label || agent;

      document.getElementById('statusAgentLabel').textContent = label;
      document.getElementById('statusIteration').textContent = iteration;
      document.getElementById('statusBarFill').style.width = '0%';

      const agentIdx = agentSequence.indexOf(agent);
      if (agentIdx >= 0) {
        const pct = ((agentIdx) / agentSequence.length) * 100;
        document.getElementById('statusBarFill').style.width = Math.min(pct + 5, 95) + '%';
      }
      updateFlowNode(agent, 'active', iteration);
    });

    activeEventSource.addEventListener('agent_complete', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const agent = data.agent;
      const success = data.success !== false;
      const duration = data.duration_ms || 0;
      const preview = data.content_preview || '';

      const currentIdx = agentSequence.indexOf(agent);
      if (currentIdx >= 0) {
        const pct = ((currentIdx + 1) / agentSequence.length) * 100;
        document.getElementById('statusBarFill').style.width = Math.min(pct, 95) + '%';
      }
      updateFlowNode(agent, success ? 'done' : 'error', null, preview, duration);
    });

    activeEventSource.addEventListener('tool_execution', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      const panel = document.getElementById('toolResultsPanel');
      panel.classList.remove('hidden');
      const list = document.getElementById('toolResultsList');
      const count = document.getElementById('toolResultsCount');
      const icon = data.success ? '<span style="color:#06d6a0">✓</span>' : '<span style="color:#ef476f">✗</span>';
      const durationMs = data.duration_ms || 0;
      const item = document.createElement('div');
      item.className = 'tool-result-item';
      item.style.cssText = 'margin-bottom:8px;padding:8px 12px;background:rgba(255,255,255,0.03);border-radius:6px;border:1px solid rgba(255,255,255,0.06);animation:slideDown 0.2s ease';
      item.innerHTML = `
        <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px">
          ${icon}
          <span style="font-size:13px;font-weight:500;color:#ccd6f6">${data.tool_name || 'tool'}</span>
          <span style="font-size:11px;color:#8892b0">exit ${data.exit_code} · ${durationMs.toFixed(0)}ms</span>
        </div>
        <pre class="mono" style="font-size:11px;color:#a8b2d1;margin:0;white-space:pre-wrap;max-height:60px;overflow-y:auto">${data.stdout_preview || '(no stdout)'}</pre>
      `;
      list.appendChild(item);
      count.textContent = list.children.length;
    });

    activeEventSource.addEventListener('iteration', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      latestIteration = data.iteration || latestIteration;
      const scores = data.scores || { overall: 0, reasonableness: 0, executability: 0, satisfaction: 0 };

      drawGauge(scores.overall);
      animateBar('scoreBar1', 'scoreVal1', scores.reasonableness || 0);
      animateBar('scoreBar2', 'scoreVal2', scores.executability || 0);
      animateBar('scoreBar3', 'scoreVal3', scores.satisfaction || 0);
      document.getElementById('statusBarFill').style.width = '100%';
      document.getElementById('statusIteration').textContent = latestIteration;
    });

    activeEventSource.addEventListener('done', function(e) {
      const data = JSON.parse(e.data);
      allEvents.push(data);
      accumulatedResult = data.result || {};
      const scores = accumulatedResult.score || { overall: 0 };

      drawGauge(scores.overall);
      animateBar('scoreBar1', 'scoreVal1', scores.reasonableness || 0);
      animateBar('scoreBar2', 'scoreVal2', scores.executability || 0);
      animateBar('scoreBar3', 'scoreVal3', scores.satisfaction || 0);

      const statusEl = document.getElementById('resultStatus');
      statusEl.className = 'result-status success';
      statusEl.textContent = accumulatedResult.success !== false
        ? '✓ Query processed successfully'
        : '✗ Could not meet threshold — max iterations reached';

      const output = document.getElementById('outputContent');
      output.textContent = accumulatedResult.content || '[No output]';

      if (data.stats) updateStats(data.stats);
      if (data.mode) updateMode(data.mode);
      renderTimeline(data.history || []);
      document.getElementById('processingStatus').classList.add('hidden');
      refreshSessionList();

      processing = false;
      btn.disabled = false;
      btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
      activeEventSource.close();
      activeEventSource = null;
    });

    activeEventSource.addEventListener('error', function(e) {
      const data = JSON.parse(e.data || '{}');
      console.error('SSE error:', data.message || 'Connection error');
      document.getElementById('processingStatus').classList.add('hidden');
      if (accumulatedResult) {
        const output = document.getElementById('outputContent');
        output.textContent = (accumulatedResult.content || '') + '\n\n[Error: ' + (data.message || 'Stream interrupted') + ']';
        document.getElementById('resultStatus').className = 'result-status failure';
        document.getElementById('resultStatus').textContent = '✗ ' + (data.message || 'Processing error');
      }
      processing = false;
      btn.disabled = false;
      btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
      if (activeEventSource) {
        activeEventSource.close();
        activeEventSource = null;
      }
    });
  }
}

// === Timeline ===
function renderTimeline(history) {
  const container = document.getElementById('executionTimeline');
  if (!container) return;
  const countEl = document.getElementById('timelineCount');
  if (countEl) countEl.textContent = history.length;
  container.innerHTML = history.map((entry, i) => {
    const isLast = i === history.length - 1;
    const score = entry.scores || { overall: 0 };
    const solverPreview = (entry.solver_content || '').substring(0, 80);
    const preview = solverPreview
      ? solverPreview + (solverPreview.length >= 80 ? '...' : '')
      : 'Iteration ' + entry.iteration;
    const cls = isLast ? 'active' : 'done';
    const overall = (score.overall || 0);
    return `
      <div class="timeline-item ${cls}">
        <span class="timeline-step">#${entry.iteration}</span>
        <div class="timeline-content">
          <div class="timeline-agent">Iteration ${entry.iteration}</div>
          <div class="timeline-detail">Score: ${overall.toFixed(3)} · ${preview}</div>
        </div>
      </div>
    `;
  }).join('');
}

// === Gauge Drawing ===
function drawGauge(value) {
  const canvas = document.getElementById('scoreGauge');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  const cx = w / 2, cy = h / 2, r = 56;
  const startAngle = -Math.PI * 0.75;
  const endAngle = Math.PI * 0.75;
  const total = endAngle - startAngle;
  const current = startAngle + total * Math.min(value, 1);

  ctx.clearRect(0, 0, w, h);

  // Background arc
  ctx.beginPath();
  ctx.arc(cx, cy, r, startAngle, endAngle);
  ctx.strokeStyle = '#2a3550';
  ctx.lineWidth = 10;
  ctx.lineCap = 'round';
  ctx.stroke();

  // Value arc
  const gradient = ctx.createLinearGradient(0, 0, w, h);
  if (value < 0.4) {
    gradient.addColorStop(0, '#ef476f');
    gradient.addColorStop(1, '#ff6b6b');
  } else if (value < 0.7) {
    gradient.addColorStop(0, '#ffd166');
    gradient.addColorStop(1, '#ffb703');
  } else {
    gradient.addColorStop(0, '#06d6a0');
    gradient.addColorStop(1, '#00b894');
  }
  ctx.beginPath();
  ctx.arc(cx, cy, r, startAngle, current);
  ctx.strokeStyle = gradient;
  ctx.lineWidth = 10;
  ctx.lineCap = 'round';
  ctx.stroke();

  document.getElementById('scoreOverall').textContent = (value * 100).toFixed(0);
  const color = value < 0.4 ? '#ef476f' : value < 0.7 ? '#ffd166' : '#06d6a0';
  document.getElementById('scoreOverall').style.color = color;
}

// === Helpers ===
function animateBar(barId, valId, value) {
  const bar = document.getElementById(barId);
  const val = document.getElementById(valId);
  if (!bar || !val) return;
  const pct = Math.round(value * 100);
  bar.style.width = pct + '%';
  val.textContent = value.toFixed(2);
}

function updateStats(stats) {
  if (!stats) return;
  var el;
  el = document.getElementById('statQueries'); if (el) el.textContent = stats.queries_processed || 0;
  el = document.getElementById('statIterations'); if (el) el.textContent = stats.iterations_executed || 0;
  el = document.getElementById('statRulesMatches'); if (el) el.textContent = stats.rules_matched || 0;
  el = document.getElementById('statCompleted'); if (el) el.textContent = stats.processes_completed || 0;
  el = document.getElementById('statTokenUsage'); if (el) el.textContent = stats.total_token_usage || 0;
  el = document.getElementById('statTokenBudget'); if (el) el.textContent = stats.token_budget || 10000;
}

function updateMode(mode) {
  if (!mode) return;
  const btn = document.getElementById('modeToggle');
  const text = btn.querySelector('.mode-text');
  btn.className = 'mode-btn';
  if (mode === 'closed') {
    btn.classList.add('closed');
    text.textContent = 'CLOSED';
  } else if (mode === 'open') {
    btn.classList.add('open');
    text.textContent = 'OPEN';
  }
  // Show/hide closed-loop feedback arrow
  var loopGroup = document.getElementById('fn-loop-group');
  if (loopGroup) {
    loopGroup.style.display = (mode === 'closed') ? '' : 'none';
  }
  // Update loop label in flowchart (visual hint)
  var loopLabel = document.getElementById('fn-loop-label');
  if (loopLabel) {
    loopLabel.textContent = (mode === 'closed') ? 'feedback (iterative)' : '';
  }
}

// === Actions ===
async function toggleMode() {
  const btn = document.getElementById('modeToggle');
  btn.classList.add('loading');
  try {
    const data = await api('/api/status');
    const newMode = data.mode === 'closed' ? 'open' : 'closed';
    const resp = await api('/api/settings', 'POST', { mode: newMode });
    if (resp.status === 'updated' || resp.status === 200) {
      updateMode(newMode);
    }
  } catch (err) {
    console.error('Mode toggle failed:', err);
    // Reload status to get actual current mode from server
    const fresh = await api('/api/status');
    updateMode(fresh.mode);
  } finally {
    btn.classList.remove('loading');
  }
}

async function updateSettings() {
  const maxIter = document.getElementById('configIterations').value;
  const threshold = document.getElementById('configThreshold').value;
  const timeout = document.getElementById('configTimeout').value;
  const dagCheckbox = document.getElementById('configDag');
  const dagEnabled = dagCheckbox ? dagCheckbox.checked : false;
  const body = { max_iterations: maxIter, threshold: threshold, execution_timeout: timeout };
  if (dagCheckbox) body.dag_enabled = dagEnabled;
  // Send current arch_mode if set
  if (window.currentArchMode) body.arch_mode = window.currentArchMode;
  await api('/api/settings', 'POST', body);
  showToast('Settings applied', 'success');
  // Update DAG label
  if (dagCheckbox) {
    document.getElementById('configDagLabel').textContent = dagEnabled ? 'On' : 'Off';
  }
}

// Architecture mode switching
let currentArchMode = 'single'; // 'single' | 'multi'

function setArchMode(mode) {
  currentArchMode = mode;
  // Update button highlights
  document.querySelectorAll('.arch-btn').forEach(btn => {
    btn.classList.toggle('active', btn.dataset.arch === mode);
  });
  // Switch flowchart
  const flowchartPanel = document.getElementById('flowchartPanel');
  if (flowchartPanel && !flowchartPanel.classList.contains('hidden')) {
    if (mode === 'multi') {
      initMultiLoopFlowchart();
    } else {
      initFlowchart();
    }
    // Reset node states
    flowchartNodeStates = {};
  }
  // Save to backend
  fetch('/api/settings', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({arch_mode: mode})
  }).catch(() => {});
  // Update the mode badge in flowchart header
  updateFlowchartArchBadge(mode);
}

function updateFlowchartArchBadge(mode) {
  const badge = document.getElementById('flowModeBadge');
  if (badge) {
    badge.textContent = mode === 'multi' ? 'MULTI' : 'CLOSED';
    badge.className = 'badge mode-badge' + (mode === 'multi' ? ' multi' : '');
  }
}

async function resetFramework() {
  await api('/api/settings', 'POST', { reset: true });
  document.getElementById('resultsSection').classList.add('hidden');
  loadStatus();
}

// ============================================================================
// MODAL SYSTEM
// ============================================================================

function openModal(id) {
  document.getElementById(id).classList.remove('hidden');
  if (id === 'apiModal') loadApiConfig();
  if (id === 'ruleModal') loadRulesToEditor();
}

function closeModal(id) {
  document.getElementById(id).classList.add('hidden');
}

function closeOnOverlay(event, id) {
  if (event.target === document.getElementById(id)) closeModal(id);
}

// ============================================================================
// API CONFIGURATION MODAL — Dynamic Provider Catalog
// ============================================================================

/** Fetch full LLM catalog from the server and render the provider UI. */
async function loadApiConfig() {
  // Fetch catalog + current config in one call
  const data = await api('/api/llm-catalog');
  llmCatalog = data.catalog || { providers: {}, categories: {}, api_types: {} };
  const current = data.current || {};

  // Render provider cards (dynamic, from catalog)
  renderProviderGrid(current.provider);

  // Restore current config values (DO THIS AFTER selectProvider so overrides survive)
  document.getElementById('apiKey').value = current.api_key || '';
  document.getElementById('baseUrl').value = current.base_url || '';
  document.getElementById('apiTemperature').value = current.temperature || 0.7;
  document.getElementById('tempVal').textContent = current.temperature || 0.7;
  document.getElementById('apiMaxTokens').value = current.max_tokens || 8192;
  document.getElementById('apiEnabled').checked = current.enabled || false;
  document.getElementById('testResult').textContent = '';

  // Select the current provider (syncs hidden input, model dropdown, default baseUrl)
  // NOTE: baseUrl is restored AFTER from current config, so custom overrides survive
  if (current.provider) {
    selectProvider(current.provider);
    // Re-apply base_url from config after selectProvider overwrites it with default
    if (current.base_url) {
      document.getElementById('baseUrl').value = current.base_url;
    }
  }
}

/** Render the provider grid from llmCatalog into categories. */
function renderProviderGrid(selectedProvider) {
  const grid = document.getElementById('providerGrid');
  grid.innerHTML = '';

  const providers = llmCatalog.providers || {};
  const categories = llmCatalog.categories || {};
  const providerKeys = Object.keys(providers);

  if (providerKeys.length === 0) {
    grid.innerHTML = '<div class="text-muted" style="padding:8px;font-size:0.78rem;">No providers in catalog. Run auto_update_providers.py first.</div>';
    return;
  }

  // Group providers by category
  const grouped = {};
  for (const [key, p] of Object.entries(providers)) {
    const cat = p.category || 'cloud';
    if (!grouped[cat]) grouped[cat] = [];
    grouped[cat].push({ id: key, ...p });
  }

  // Render each category section
  for (const [catKey, catProviders] of Object.entries(grouped)) {
    const catInfo = categories[catKey] || { name: catKey, description: '' };
    const section = document.createElement('div');
    section.className = 'provider-category-section';

    const label = document.createElement('div');
    label.className = 'provider-category-label';
    label.setAttribute('data-category', catKey);
    label.textContent = catInfo.name || catKey;
    section.appendChild(label);

    const cards = document.createElement('div');
    cards.className = 'provider-category-cards';
    cards.id = `providerCategory_${catKey}`;

    for (const p of catProviders) {
      const card = document.createElement('label');
      card.className = 'provider-card' + (p.id === selectedProvider ? ' selected' : '');
      card.setAttribute('data-provider', p.id);
      card.onclick = (e) => selectProvider(p.id);

      const icon = document.createElement('span');
      icon.className = 'provider-icon';
      icon.textContent = p.icon || p.id.charAt(0).toUpperCase();

      const name = document.createElement('span');
      name.className = 'provider-name';
      name.textContent = p.short_name || p.name || p.id;
      // Add model count
      const modelCount = (p.models || []).length;
      if (modelCount > 0) {
        const countBadge = document.createElement('span');
        countBadge.style.cssText = 'font-size:0.6rem;color:var(--text-muted);margin-left:4px;';
        countBadge.textContent = `(${modelCount})`;
        name.appendChild(countBadge);
      }

      const check = document.createElement('span');
      check.className = 'provider-check';

      card.appendChild(icon);
      card.appendChild(name);
      card.appendChild(check);
      cards.appendChild(card);
    }

    section.appendChild(cards);
    grid.appendChild(section);
  }
}

/** Populate the model <select> dropdown from catalog data. */
function populateModelSelect(providerId, selectedModel) {
  const select = document.getElementById('apiModelSelect');
  const hint = document.getElementById('modelHint');
  const infoBadge = document.getElementById('modelInfo');
  select.innerHTML = '';

  const pInfo = (llmCatalog.providers || {})[providerId];
  const models = (pInfo && pInfo.models) || [];

  if (!pInfo) {
    // Fallback: free-text input mode
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = 'Custom model...';
    select.appendChild(opt);
    hint.textContent = 'Type the model ID manually in the field';
    infoBadge.style.display = 'none';
    return;
  }

  if (models.length === 0) {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = 'No models listed — type manually';
    select.appendChild(opt);
    hint.textContent = `No models in catalog for ${pInfo.name}. Type model ID manually.`;
    infoBadge.style.display = 'none';
    return;
  }

  // Add grouped options
  for (const m of models) {
    const opt = document.createElement('option');
    opt.value = m.id;
    opt.textContent = m.name || m.id;
    if (m.description) opt.title = m.description;
    if (m.pricing && m.pricing !== 'unknown') {
      opt.textContent += `  [${m.pricing}]`;
    }
    select.appendChild(opt);
  }

  // Try to select the current model
  if (selectedModel) {
    const matched = Array.from(select.options).find(o => o.value === selectedModel);
    if (matched) {
      select.value = selectedModel;
    }
  }

  // Show model info badge
  const cat = pInfo.category || 'cloud';
  const catNames = (llmCatalog.categories || {});
  const catName = (catNames[cat] && catNames[cat].name) || cat;
  infoBadge.style.display = 'inline-block';
  infoBadge.textContent = `${models.length} models · ${catName}`;
  hint.textContent = 'Select a model from the catalog. Models updated automatically every 3 days.';
}

function selectProvider(provider) {
  // Remove all selected
  document.querySelectorAll('.provider-card').forEach(c => c.classList.remove('selected'));
  // Select this one
  const card = document.querySelector(`.provider-card[data-provider="${provider}"]`);
  if (card) card.classList.add('selected');
  document.getElementById('selectedProvider').value = provider;

  // Get provider info from catalog
  const pInfo = (llmCatalog.providers || {})[provider];
  if (!pInfo) return;

  // Show/hide API key and base URL based on provider type
  const apiKeyRequired = pInfo.api_key_required !== false;  // default true
  const isAnthropic = pInfo.api_type === 'anthropic';
  const isGemini = pInfo.api_type === 'gemini';
  const isLocal = pInfo.category === 'local';

  document.getElementById('apiKeyGroup').style.display = isLocal ? 'none' : '';
  if (isLocal) {
    document.getElementById('apiKey').placeholder = 'No API key needed for local';
  } else {
    document.getElementById('apiKey').placeholder = 'sk-... / your API key';
  }

  document.getElementById('baseUrlGroup').style.display = (isLocal || isAnthropic || isGemini) ? '' : 'none';
  // Always set baseUrl value when selecting a provider (fix stale base_url bug)
  if (pInfo.default_base_url) {
    document.getElementById('baseUrl').value = pInfo.default_base_url;
    document.getElementById('baseUrl').placeholder = pInfo.default_base_url;
  } else {
    document.getElementById('baseUrl').placeholder = 'https://api.example.com/v1';
  }

  // Populate model dropdown
  const currentModel = document.getElementById('apiModelSelect').value || '';
  populateModelSelect(provider, currentModel);

  // Update hint
  document.getElementById('modelHint').textContent =
    `Provider: ${pInfo.name} · API: ${pInfo.api_type || 'openai'}${pInfo.website ? ' · ' + pInfo.website : ''}`;
}

async function saveApiConfig() {
  const provider = document.getElementById('selectedProvider').value;
  if (!provider) {
    alert('Please select an LLM provider');
    return;
  }

  const config = {
    provider: provider,
    api_key: document.getElementById('apiKey').value,
    model: document.getElementById('apiModelSelect').value,
    base_url: document.getElementById('baseUrl').value,
    temperature: parseFloat(document.getElementById('apiTemperature').value),
    max_tokens: parseInt(document.getElementById('apiMaxTokens').value, 10) || 8192,
    enabled: document.getElementById('apiEnabled').checked,
  };

  await api('/api/api-config', 'POST', config);
  closeModal('apiModal');
}

async function testApiConnection() {
  const provider = document.getElementById('selectedProvider').value;
  if (!provider) {
    document.getElementById('testResult').textContent = 'Select a provider first';
    document.getElementById('testResult').className = 'test-result err';
    return;
  }

  const btn = document.getElementById('testBtn');
  btn.disabled = true;
  btn.innerHTML = 'Testing...';
  document.getElementById('testResult').textContent = '';

  try {
    const data = await api('/api/api-config/test', 'POST', {
      provider: provider,
      api_key: document.getElementById('apiKey').value,
      model: document.getElementById('apiModelSelect').value,
      base_url: document.getElementById('baseUrl').value,
    });
    if (data.success) {
      document.getElementById('testResult').textContent = '✓ ' + (data.message || 'Connected successfully!');
      document.getElementById('testResult').className = 'test-result ok';
    } else {
      document.getElementById('testResult').textContent = '✗ ' + (data.message || 'Connection failed');
      document.getElementById('testResult').className = 'test-result err';
    }
  } catch (err) {
    document.getElementById('testResult').textContent = '✗ ' + err.message;
    document.getElementById('testResult').className = 'test-result err';
  } finally {
    btn.disabled = false;
    btn.innerHTML = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/></svg> Test Connection';
  }
}

// ============================================================================
// RULE EDITOR MODAL
// ============================================================================

function loadRulesToEditor() {
  const container = document.getElementById('ruleItems');
  if (!container) return;

  // Try to fetch fresh from server first
  api('/api/rules').then(data => {
    currentRules = data.rules || [];
    renderRuleList(container);
    // If we were editing, preserve the editor state
    if (editingRuleIndex >= 0) openRuleEditor(editingRuleIndex);
  }).catch(() => {
    // Use cached rules as fallback
    renderRuleList(container);
  });
}

function renderRuleList(container) {
  if (!container) return;
  container.innerHTML = currentRules.map((r, i) => `
    <div class="rule-list-item ${i === editingRuleIndex ? 'active' : ''}" onclick="openRuleEditor(${i})">
      <div>
        <div class="rule-list-pattern">/${r.pattern}/</div>
        <div class="rule-list-meta">${r.validation_method} · th=${r.threshold}</div>
      </div>
      <div class="rule-list-actions">
        <button class="btn btn-sm btn-danger" onclick="event.stopPropagation(); deleteRule(${i})">
          <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
        </button>
      </div>
    </div>
  `).join('');
}

function addNewRule() {
  // Create a blank rule template
  const newRule = {
    pattern: '^write a .+ (script|program|app)',
    validation_method: 'regex',
    recommended_tools: [],
    weights: { reasonableness: 0.4, executability: 0.4, satisfaction: 0.2 },
    threshold: 0.7,
  };
  currentRules.push(newRule);
  editingRuleIndex = currentRules.length - 1;
  renderRuleList(document.getElementById('ruleItems'));
  openRuleEditor(editingRuleIndex);
}

function openRuleEditor(index) {
  editingRuleIndex = index;
  const rule = currentRules[index];
  if (!rule) return;

  // Update list highlighting
  document.querySelectorAll('.rule-list-item').forEach((el, i) => {
    el.classList.toggle('active', i === index);
  });

  // Show editor
  const editor = document.getElementById('ruleEditor');
  editor.classList.remove('hidden');
  document.getElementById('ruleEditorTitle').textContent = `Edit Rule #${index + 1}`;

  // Fill fields
  document.getElementById('editPattern').value = rule.pattern || '';
  document.getElementById('editValidation').value = rule.validation_method || 'regex';
  document.getElementById('editTools').value = (rule.recommended_tools || []).join(', ');

  const w = rule.weights || { reasonableness: 0.4, executability: 0.4, satisfaction: 0.2 };
  document.getElementById('editW1').value = w.reasonableness;
  document.getElementById('w1Val').textContent = w.reasonableness.toFixed(2);
  document.getElementById('editW2').value = w.executability;
  document.getElementById('w2Val').textContent = w.executability.toFixed(2);
  document.getElementById('editW3').value = w.satisfaction;
  document.getElementById('w3Val').textContent = w.satisfaction.toFixed(2);

  document.getElementById('editThreshold').value = rule.threshold || 0.7;
  document.getElementById('thVal').textContent = (rule.threshold || 0.7).toFixed(2);

  // Show delete button
  document.getElementById('deleteRuleBtn').style.display = '';
}

function cancelRuleEdit() {
  const editor = document.getElementById('ruleEditor');
  editor.classList.add('hidden');
  editingRuleIndex = -1;
  // Remove active highlighting
  document.querySelectorAll('.rule-list-item').forEach(el => el.classList.remove('active'));
}

function saveRuleEdit() {
  if (editingRuleIndex < 0) return;

  const toolsStr = document.getElementById('editTools').value.trim();
  const tools = toolsStr ? toolsStr.split(',').map(t => t.trim()).filter(t => t) : [];

  currentRules[editingRuleIndex] = {
    pattern: document.getElementById('editPattern').value,
    validation_method: document.getElementById('editValidation').value,
    recommended_tools: tools,
    weights: {
      reasonableness: parseFloat(document.getElementById('editW1').value),
      executability: parseFloat(document.getElementById('editW2').value),
      satisfaction: parseFloat(document.getElementById('editW3').value),
    },
    threshold: parseFloat(document.getElementById('editThreshold').value),
  };

  // Save to server
  api('/api/rules', 'POST', { rules: currentRules }).then(() => {
    // Refresh sidebar rules
    loadRules();
    // Re-render list
    renderRuleList(document.getElementById('ruleItems'));
    // Close editor
    cancelRuleEdit();
  });
}

function deleteRule(index) {
  if (!confirm(`Delete rule #${index + 1}: /${currentRules[index].pattern}/?`)) return;

  // Close editor if editing this rule
  if (editingRuleIndex === index) cancelRuleEdit();

  currentRules.splice(index, 1);

  // Save to server
  api('/api/rules', 'POST', { rules: currentRules }).then(() => {
    loadRules();
    renderRuleList(document.getElementById('ruleItems'));
  });
}

function deleteCurrentRule() {
  if (editingRuleIndex < 0) return;
  deleteRule(editingRuleIndex);
}

// ============================================================================
// TOOLS MODAL
// ============================================================================

function openToolsModal() {
  openModal('toolsModal');
  refreshSandbox();
}

function refreshSandbox() {
  api('/api/tools/status').then(d => {
    document.getElementById('sandboxPath').textContent = d.sandbox_path || '/tmp/clma-sandbox';
    document.getElementById('sandboxPathFooter').textContent = d.sandbox_path || '/tmp/clma-sandbox';
    document.getElementById('sandboxFileCount').textContent = (d.file_count || 0) + ' files';
    const dockerEl = document.getElementById('dockerStatus');
    if (d.capabilities && d.capabilities.docker_available) {
      dockerEl.textContent = 'Docker: ✓';
      dockerEl.style.color = '#06d6a0';
    } else {
      dockerEl.textContent = 'Docker: ✗';
      dockerEl.style.color = '#ef476f';
    }
    // Show sandbox files
    const filesEl = document.getElementById('sandboxFilesList');
    if (d.files && d.files.length > 0) {
      filesEl.textContent = d.files.join('\n');
    } else {
      filesEl.textContent = '(empty)';
    }
  }).catch(() => {});
}

function refreshSandboxFiles() {
  api('/api/tools/sandbox/files').then(d => {
    const el = document.getElementById('sandboxFilesList');
    if (d.files && d.files.length > 0) {
      el.textContent = d.files.join('\n');
    } else {
      el.textContent = '(empty)';
    }
    document.getElementById('sandboxFileCount').textContent = (d.count || 0) + ' files';
  }).catch(() => {});
}

function cleanSandbox() {
  api('/api/tools/sandbox/clean', 'POST').then(d => {
    document.getElementById('sandboxFilesList').textContent = '(empty)';
    document.getElementById('sandboxFileCount').textContent = '0 files';
    // Clear tool results
    document.getElementById('toolResultSection').classList.add('hidden');
  }).catch(() => {});
}

async function executeTool() {
  const code = document.getElementById('toolCodeInput').value.trim();
  if (!code) return;

  const language = document.getElementById('toolLanguage').value;
  const btn = document.getElementById('toolExecBtn');
  const prevText = btn.innerHTML;
  btn.disabled = true;
  btn.innerHTML = '<div class="spinner" style="width:14px;height:14px;border-width:2px;display:inline-block;vertical-align:middle;margin-right:6px"></div> Executing...';

  // Hide previous result
  document.getElementById('toolResultSection').classList.add('hidden');

  try {
    const d = await api('/api/tools/execute', 'POST', { code, language });

    // Show result
    const section = document.getElementById('toolResultSection');
    section.classList.remove('hidden');

    const header = document.getElementById('toolResultHeader');
    const icon = document.getElementById('toolResultIcon');
    const summary = document.getElementById('toolResultSummary');

    if (d.success) {
      header.style.backgroundColor = 'rgba(6, 214, 160, 0.1)';
      header.style.border = '1px solid rgba(6, 214, 160, 0.3)';
      icon.innerHTML = '<span style="color:#06d6a0">✓</span>';
      summary.textContent = `${d.tool_name} — exit ${d.exit_code} in ${d.duration_ms.toFixed(0)}ms`;
      summary.style.color = '#06d6a0';
    } else {
      header.style.backgroundColor = 'rgba(239, 71, 111, 0.1)';
      header.style.border = '1px solid rgba(239, 71, 111, 0.3)';
      icon.innerHTML = '<span style="color:#ef476f">✗</span>';
      summary.textContent = `${d.tool_name || language} — exit ${d.exit_code || -1}`;
      summary.style.color = '#ef476f';
    }

    document.getElementById('toolStdout').textContent = d.stdout || '(no output)';
    const stderrGroup = document.getElementById('toolStderrGroup');
    const stderrEl = document.getElementById('toolStderr');
    if (d.stderr) {
      stderrGroup.classList.remove('hidden');
      stderrEl.textContent = d.stderr;
    } else {
      stderrGroup.classList.add('hidden');
    }

    // Refresh files list
    refreshSandboxFiles();
  } catch (err) {
    const section = document.getElementById('toolResultSection');
    section.classList.remove('hidden');
    const header = document.getElementById('toolResultHeader');
    header.style.backgroundColor = 'rgba(239, 71, 111, 0.1)';
    header.style.border = '1px solid rgba(239, 71, 111, 0.3)';
    document.getElementById('toolResultIcon').innerHTML = '<span style="color:#ef476f">✗</span>';
    document.getElementById('toolResultSummary').textContent = 'Error: ' + (err.message || 'Unknown error');
    document.getElementById('toolResultSummary').style.color = '#ef476f';
    document.getElementById('toolStdout').textContent = err.message || 'Execution failed';
  } finally {
    btn.disabled = false;
    btn.innerHTML = prevText;
  }
}

// Helper: escape HTML
function escapeHtml(text) {
  const div = document.createElement('div');
  div.textContent = text;
  return div.innerHTML;
}

// Safe data attribute encoding — HTML-entities, then entify any raw newlines for HTML attr safety
function attrEncode(text) {
  return escapeHtml(text).replace(/\n/g, '&#10;').replace(/\r/g, '&#13;');
}

function saveAllRules() {
  // First flush any unsaved edits
  if (editingRuleIndex >= 0) {
    // Apply current editor state to the rule being edited
    const toolsStr = document.getElementById('editTools').value.trim();
    const tools = toolsStr ? toolsStr.split(',').map(t => t.trim()).filter(t => t) : [];
    currentRules[editingRuleIndex] = {
      pattern: document.getElementById('editPattern').value,
      validation_method: document.getElementById('editValidation').value,
      recommended_tools: tools,
      weights: {
        reasonableness: parseFloat(document.getElementById('editW1').value),
        executability: parseFloat(document.getElementById('editW2').value),
        satisfaction: parseFloat(document.getElementById('editW3').value),
      },
      threshold: parseFloat(document.getElementById('editThreshold').value),
    };
  }

  api('/api/rules', 'POST', { rules: currentRules }).then(() => {
    loadRules();
    renderRuleList(document.getElementById('ruleItems'));
    cancelRuleEdit();
    alert('All rules saved successfully!');
  }).catch(err => {
    alert('Error saving rules: ' + err.message);
  });
}

// === Reasoning Chain ===
function renderReasoningChain(data) {
  const panel = document.getElementById('reasoningChainPanel');
  const container = document.getElementById('chainContainer');
  const countEl = document.getElementById('chainCount');
  if (!panel || !container) return;

  // Try to get iteration-level agent data
  // History entries each have: result.metadata with reasoned_content, solved_content, verified_content
  // Result also has: metadata.refined_query, metadata.reasoned_content/metadata.solved_content/metadata.verified_content
  // Plus iteration score data in result.iterations

  const history = data.history || [];
  const result = data.result || {};
  const meta = result.metadata || {};
  const iterations = result.iterations || [];

  // Show iterations score progression
  let html = '';

  // Score progression chart
  if (iterations.length > 0) {
    const maxScore = Math.max(...iterations.map(i => i.score || 0), 0.01);
    html += '<div class="score-progression">';
    html += '<h4>📈 Score Progression per Iteration</h4>';
    html += '<div class="score-bars">';
    iterations.forEach((iter, idx) => {
      const pct = Math.max((iter.score / maxScore) * 100, 4);
      const col = iter.score < 0.4 ? '#ef476f' : iter.score < 0.7 ? '#ffd166' : '#06d6a0';
      html += `<div class="score-bar-wrapper">
        <div class="score-bar-vis" style="height:${pct}%;background:${col}" data-value="${iter.score.toFixed(3)}"></div>
        <span class="score-bar-label">#${iter.iteration}</span>
      </div>`;
    });
    html += '</div></div>';
  }

  // For each history entry (iteration), show the agent chain
  if (history.length > 0) {
    history.forEach((entry, entryIdx) => {
      const res = entry.result || entry;
      const entryMeta = res.metadata || {};
      const iterScore = (res.score && res.score.overall) ? res.score.overall : (res.iteration_score_overall || 0);
      const refQuery = entryMeta.refined_query || '';
      const reasoned = entryMeta.reasoned_content || '';
      const solved = entryMeta.solved_content || '';
      const verified = entryMeta.verified_content || '';
      const agentName = entryMeta.agent || '';

      // Also check: if entry is from the timeline's history with agent info
      const agentRoles = ['refiner', 'reasoner', 'solver', 'verifier', 'evaluator'];
      const stages = [];

      if (refQuery) stages.push({ name: 'refiner', label: 'Refiner', icon: 'R', content: refQuery });
      if (reasoned) stages.push({ name: 'reasoner', label: 'Reasoner', icon: 'R', content: reasoned });
      if (solved)   stages.push({ name: 'solver', label: 'Solver', icon: 'S', content: solved });
      if (verified) stages.push({ name: 'verifier', label: 'Verifier', icon: 'V', content: verified });

      // If we have no metadata stages but have an agent name, show it
      if (stages.length === 0 && agentName && agentRoles.includes(agentName)) {
        stages.push({ name: agentName, label: agentName, icon: agentName.charAt(0).toUpperCase(), content: res.content || '(empty)' });
      }

      if (stages.length === 0) return;

      html += `<div class="chain-step"><div class="chain-step-header">
        <span style="font-size:0.8rem;font-weight:600;color:var(--text-tertiary)">Iteration #${entryIdx + 1}</span>
        <span class="chain-step-meta">Score: ${(iterScore || 0).toFixed(3)}</span>
      </div>`;

      stages.forEach((stage, si) => {
        const truncated = stage.content.length > 200;
        const dispContent = truncated ? stage.content.substring(0, 200) + '...' : stage.content;
        const contentId = `chainContent_${entryIdx}_${si}`;
        html += `<div style="margin-top:${si > 0 ? '8px' : '0'}">
          ${si > 0 ? '<div class="chain-step-arrow">↓</div>' : ''}
          <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px;padding:0 2px">
            <div class="chain-step-icon ${stage.name}">${stage.icon}</div>
            <span class="chain-step-name">${stage.label}</span>
          </div>
          <pre class="chain-step-content" id="${contentId}" data-original="${attrEncode(stage.content)}">${escapeHtml(dispContent)}</pre>
          ${truncated ? `<button class="chain-toggle-btn" onclick="toggleChainContent('${contentId}', this)">Show all (${stage.content.length} chars)</button>` : ''}
          ${!truncated && stage.content.length > 0 ? `<span style="font-size:0.7rem;color:var(--text-tertiary);padding:0 2px">${stage.content.length} chars</span>` : ''}
        </div>`;
      });

      html += '</div>';
    });
  }

  // Fallback: show the main output content as a single chain if no history
  if (history.length === 0 && result.content) {
    html += '<div style="color:var(--text-tertiary);font-size:0.85rem;text-align:center;padding:8px">No multi-step chain data available. Showing final output only.</div>';
  }

  container.innerHTML = html || '<div class="chain-empty">No reasoning chain data available.</div>';
  const visibleCount = history.filter(e => {
    const m = (e.result || e).metadata || {};
    return m.reasoned_content || m.solved_content || m.verified_content;
  }).length;
  countEl.textContent = visibleCount || (history.length > 0 ? history.length : 0);
  panel.classList.remove('hidden');
}

function toggleChainContent(contentId, btn) {
  const el = document.getElementById(contentId);
  if (!el) return;
  const fullContent = el.dataset.original || '';
  if (el.dataset.expanded === 'true') {
    el.textContent = fullContent.substring(0, 200) + '...';
    el.dataset.expanded = 'false';
    btn.textContent = 'Show all (' + fullContent.length + ' chars)';
  } else {
    el.textContent = fullContent;
    el.dataset.expanded = 'true';
    btn.textContent = 'Collapse';
  }
}

// === Copy Output ===
async function copyOutput() {
  const content = document.getElementById('outputContent');
  const text = content.textContent || '';
  if (!text.trim()) return;

  const btn = document.getElementById('copyOutputBtn');
  const span = btn.querySelector('span');
  const origText = span.textContent;

  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(text);
    } else {
      // Fallback for older browsers
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      document.body.removeChild(ta);
    }
    span.textContent = 'Copied!';
    btn.classList.add('copied');
    setTimeout(() => {
      span.textContent = origText;
      btn.classList.remove('copied');
    }, 2000);
  } catch (e) {
    span.textContent = 'Failed';
    setTimeout(() => { span.textContent = origText; }, 1500);
  }
}

// ============================================================================
// === SVG FLOWCHART FUNCTIONS ===
let flowchartNodeStates = {};

// Dynamic layout parameters
const FN_RADIUS = 22;
const FN_CIRCLE_GAP = 80; // minimum gap between circle edge and text (>= radius)
const FN_PADDING_LEFT = 16;
const FN_PADDING_RIGHT = 16;
const FN_NODE_H = 130;
const FN_GAP_X = 350; // horizontal spacing between node centers
const FN_START_X = 40;
const FN_Y = 60;

// Dynamic node — x will be set during init based on computed widths
// We use a helper to compute node width from text + circle + margins
function getNodeWidth(textLength) {
  return FN_PADDING_LEFT + FN_RADIUS * 2 + FN_CIRCLE_GAP + textLength + FN_PADDING_RIGHT;
}

const FLOWCHART_AGENT_DEFS = [
  { id: 'refiner',   label: 'Refiner',   icon: 'R' },
  { id: 'reasoner',  label: 'Reasoner',  icon: 'R' },
  { id: 'solver',    label: 'Solver',    icon: 'S' },
  { id: 'verifier',  label: 'Verifier',  icon: 'V' },
  { id: 'evaluator', label: 'Evaluator', icon: 'E' },
];

// Will be populated at runtime
let FLOWCHART_AGENTS = [];
let FN_NODE_W = 0;

const FLOWCHART_ARROWS = [
  { from: 0, to: 1, label: 'refined query' },
  { from: 1, to: 2, label: 'reasoning steps' },
  { from: 2, to: 3, label: 'solution' },
  { from: 3, to: 4, label: 'verification' },
];

// Closed-loop arrow: evaluator -> refiner (feedback loop, bottom curve)
const CLOSED_LOOP_ARROW = { from: 4, to: 0, label: 'feedback (iterative)' };

function initFlowchart() {
  const svg = document.getElementById('flowchartSvg');
  if (!svg) return;

  flowchartNodeStates = {};

  // --- Dynamic layout computation ---
  // Temporarily append a hidden text element to measure label widths
  var measurer = document.createElementNS('http://www.w3.org/2000/svg', 'text');
  measurer.setAttribute('font-size', '18');
  measurer.setAttribute('font-weight', '600');
  measurer.setAttribute('font-family', "'SF Mono', 'Fira Code', 'Cascadia Code', monospace");
  measurer.setAttribute('visibility', 'hidden');
  svg.appendChild(measurer);

  var maxTextWidth = 0;
  for (var i = 0; i < FLOWCHART_AGENT_DEFS.length; i++) {
    measurer.textContent = FLOWCHART_AGENT_DEFS[i].label;
    var tw = measurer.getComputedTextLength();
    if (tw > maxTextWidth) maxTextWidth = tw;
  }
  svg.removeChild(measurer);

  // Compute uniform node width
  FN_NODE_W = getNodeWidth(maxTextWidth);

  // Build FLOWCHART_AGENTS with computed x positions
  FLOWCHART_AGENTS = [];
  for (var i = 0; i < FLOWCHART_AGENT_DEFS.length; i++) {
    var def = FLOWCHART_AGENT_DEFS[i];
    FLOWCHART_AGENTS.push({
      id: def.id,
      label: def.label,
      icon: def.icon,
      x: FN_START_X + FN_GAP_X * i,
      y: FN_Y,
    });
  }

  var totalW = FN_START_X + FN_GAP_X * 4 + FN_NODE_W + 40;
  var totalH = 280;
  svg.setAttribute('viewBox', '0 0 ' + totalW + ' ' + totalH);

  // Clear existing content but keep the SVG element
  svg.innerHTML = '';

  // Add SVG defs for glow filter
  const defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
  defs.innerHTML = `
    <filter id="glow-blue" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="3" result="blur"/>
      <feMerge>
        <feMergeNode in="blur"/>
        <feMergeNode in="SourceGraphic"/>
      </feMerge>
    </filter>
    <filter id="glow-green" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="3" result="blur"/>
      <feMerge>
        <feMergeNode in="blur"/>
        <feMergeNode in="SourceGraphic"/>
      </feMerge>
    </filter>
    <!-- Arrow marker -->
    <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#2a3550"/>
    </marker>
    <marker id="arrowhead-active" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#4a6cf7"/>
    </marker>
    <marker id="arrowhead-done" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#06d6a0"/>
    </marker>
  `;
  svg.appendChild(defs);

  // Arrow group (drawn before nodes so they're behind)
  const arrowGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
  arrowGroup.setAttribute('class', 'fn-arrow-group');

  FLOWCHART_ARROWS.forEach((arrow, idx) => {
    const fromAgent = FLOWCHART_AGENTS[arrow.from];
    const toAgent = FLOWCHART_AGENTS[arrow.to];
    // Line from center-right of from node to center-left of to node (aligned with circle center)
    const x1 = fromAgent.x + FN_NODE_W;
    const y1 = fromAgent.y + 35;
    const x2 = toAgent.x;
    const y2 = toAgent.y + 35;

    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', x1);
    line.setAttribute('y1', y1);
    line.setAttribute('x2', x2);
    line.setAttribute('y2', y2);
    line.setAttribute('class', 'fn-arrow-line');
    line.setAttribute('id', 'fn-arrow-' + idx);
    line.setAttribute('marker-end', 'url(#arrowhead)');
    arrowGroup.appendChild(line);

    // Arrow label at midpoint
    const mx = (x1 + x2) / 2;
    const my = (y1 + y2) / 2 - 8;
    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    label.setAttribute('x', mx);
    label.setAttribute('y', my);
    label.setAttribute('class', 'fn-arrow-label');
    label.setAttribute('id', 'fn-arrow-label-' + idx);
    label.textContent = arrow.label;
    arrowGroup.appendChild(label);
  });

  svg.appendChild(arrowGroup);

  // Node group
  const nodeGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');

  FLOWCHART_AGENTS.forEach((agent) => {
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');

    // Background rectangle
    const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    rect.setAttribute('x', agent.x);
    rect.setAttribute('y', agent.y);
    rect.setAttribute('width', FN_NODE_W);
    rect.setAttribute('height', FN_NODE_H);
    rect.setAttribute('class', 'fn-node-bg idle');
    rect.setAttribute('id', 'fn-node-' + agent.id);
    g.appendChild(rect);

    // Icon circle (left side, vertically centered in upper area)
    const cx = agent.x + FN_PADDING_LEFT + FN_RADIUS;
    const cy = agent.y + 35; // fixed vertical center for circle
    const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    circle.setAttribute('cx', cx);
    circle.setAttribute('cy', cy);
    circle.setAttribute('r', FN_RADIUS);
    circle.setAttribute('class', 'fn-icon-circle');
    circle.setAttribute('id', 'fn-icon-' + agent.id);
    g.appendChild(circle);

    // Icon letter (centered in circle)
    const iconText = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    iconText.setAttribute('x', cx);
    iconText.setAttribute('y', cy + 6);
    iconText.setAttribute('class', 'fn-agent-label');
    iconText.setAttribute('font-size', '16');
    iconText.setAttribute('font-weight', '700');
    iconText.setAttribute('text-anchor', 'middle');
    iconText.textContent = agent.icon;
    g.appendChild(iconText);

    // Agent label (right of circle, same vertical line as circle)
    const labelX = agent.x + FN_PADDING_LEFT + FN_RADIUS * 2 + FN_CIRCLE_GAP;
    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    label.setAttribute('x', labelX);
    label.setAttribute('y', cy + 7);
    label.setAttribute('class', 'fn-agent-label');
    label.setAttribute('font-size', '18');
    label.setAttribute('font-weight', '600');
    label.setAttribute('text-anchor', 'start');
    label.setAttribute('id', 'fn-label-' + agent.id);
    label.textContent = agent.label;
    g.appendChild(label);

    // Score text (top-right corner, aligned to right edge)
    const score = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    score.setAttribute('x', agent.x + FN_NODE_W - 20);
    score.setAttribute('y', agent.y + 20);
    score.setAttribute('class', 'fn-score-text');
    score.setAttribute('font-size', '11');
    score.setAttribute('text-anchor', 'end');
    score.setAttribute('id', 'fn-score-' + agent.id);
    score.textContent = '';
    g.appendChild(score);

    // Status text (above preview line)
    const status = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    status.setAttribute('x', agent.x + FN_NODE_W / 2);
    status.setAttribute('y', agent.y + 80);
    status.setAttribute('class', 'fn-agent-status');
    status.setAttribute('font-size', '13');
    status.setAttribute('text-anchor', 'middle');
    status.setAttribute('id', 'fn-status-' + agent.id);
    status.textContent = 'idle';
    g.appendChild(status);

    // Preview text (bottom area, multi-line support)
    // We'll use a container group — individual tspans are added in updateFlowNode
    const previewGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    previewGroup.setAttribute('id', 'fn-preview-' + agent.id + '-group');
    g.appendChild(previewGroup);
    // Initial empty tspan
    const preview = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    preview.setAttribute('x', agent.x + FN_NODE_W / 2);
    preview.setAttribute('y', agent.y + 100);
    preview.setAttribute('class', 'fn-preview-text');
    preview.setAttribute('font-size', '10');
    preview.setAttribute('text-anchor', 'middle');
    preview.setAttribute('id', 'fn-preview-' + agent.id);
    preview.textContent = '';
    previewGroup.appendChild(preview);

    nodeGroup.appendChild(g);

    // Initialize state
    flowchartNodeStates[agent.id] = 'idle';
  });

  svg.appendChild(nodeGroup);

  // === Closed-loop arrow (evaluator -> refiner, bottom curve) ===
  var fromAgent = FLOWCHART_AGENTS[CLOSED_LOOP_ARROW.from];
  var toAgent = FLOWCHART_AGENTS[CLOSED_LOOP_ARROW.to];
  // evaluator bottom-center: x = fromAgent.x + width/2, y = fromAgent.y + height
  // refiner bottom-center: x = toAgent.x + width/2, y = toAgent.y + height
  var x1 = fromAgent.x + FN_NODE_W / 2;
  var y1 = fromAgent.y + FN_NODE_H;
  var x2 = toAgent.x + FN_NODE_W / 2;
  var y2 = toAgent.y + FN_NODE_H;
  var cy = y1 + 55; // control point Y for the curve bottom

  var loopGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
  loopGroup.setAttribute('class', 'fn-arrow-group');
  loopGroup.setAttribute('id', 'fn-loop-group');

  var path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  path.setAttribute('d', 'M ' + x1 + ',' + y1 + ' C ' + x1 + ',' + cy + ' ' + x2 + ',' + cy + ' ' + x2 + ',' + y2);
  path.setAttribute('class', 'fn-loop-arrow');
  path.setAttribute('id', 'fn-loop-arrow');
  path.setAttribute('fill', 'none');
  path.setAttribute('marker-end', 'url(#arrowhead)');
  loopGroup.appendChild(path);

  // Loop label
  var mx = (x1 + x2) / 2;
  var label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
  label.setAttribute('x', mx);
  label.setAttribute('y', cy - 4);
  label.setAttribute('class', 'fn-arrow-label');
  label.setAttribute('text-anchor', 'middle');
  label.setAttribute('id', 'fn-loop-label');
  label.textContent = CLOSED_LOOP_ARROW.label;
  loopGroup.appendChild(label);

  svg.appendChild(loopGroup);
}

// ===================== 多级嵌套闭环流程图 =====================
// 架构：外层 Strategy Loop (Strategy Refiner → Strategy Reasoner → [内层 Execution Loop] → Outer Verifier → Outer Evaluator)
// 布局参数参照单闭环（FN_RADIUS=22, FN_CIRCLE_GAP=80, FN_NODE_H=130, FN_GAP_X=350），多闭环使用 GAP_X=390
// 使文字、圆圈大小、间距与单闭环完全一致，再无重叠

const MULTI_AGENT_DEFS = [
  { id: 'strategy_refiner',  label: 'Strategy Refiner',  icon: 'R' },
  { id: 'strategy_reasoner', label: 'Strategy Reasoner', icon: 'R' },
  { id: 'execution_loop',    label: 'Execution Loop',    icon: '⚡' },
  { id: 'outer_verifier',    label: 'Outer Verifier',    icon: 'V' },
  { id: 'outer_evaluator',   label: 'Outer Evaluator',   icon: 'E' },
];

// 使用与单闭环完全相同的布局参数
const MULTI_FN_RADIUS = 22;
const MULTI_FN_CIRCLE_GAP = 80;
const MULTI_FN_PADDING_LEFT = 16;
const MULTI_FN_PADDING_RIGHT = 16;
const MULTI_FN_NODE_H = 130;
const MULTI_FN_GAP_X = 390;  // 比单闭环 350 稍宽，为内层框留空间
const MULTI_FN_START_X = 40;
const MULTI_FN_Y = 60;

// 节点中圆圈中心 y 坐标（与单闭环一致：FN_Y + 35）
const MULTI_CIRCLE_CY = MULTI_FN_Y + 35;

// 内层框
const MULTI_INNER_BOX_Y = MULTI_FN_Y + MULTI_FN_NODE_H + 20;  // 210
const MULTI_INNER_BOX_H = 115;
// 内层框 x: 从 reasoner 右缘 +25px 到 outer_verifier 左缘 -30px
const MULTI_INNER_BOX_X = MULTI_FN_START_X + MULTI_FN_GAP_X * 1 + (function(){
  // 估算 nodeW = 16 + 44 + 80 + 165 + 16 = 321
  return 321;
})() + 25;
const MULTI_INNER_BOX_W = (MULTI_FN_START_X + MULTI_FN_GAP_X * 3) - MULTI_INNER_BOX_X - 30;

// 内层小圆圈
const MULTI_INNER_R = 16;
const MULTI_INNER_GAP = 100;

function initMultiLoopFlowchart() {
  const svg = document.getElementById('flowchartSvg');
  if (!svg) return;

  const nodeW = (function(){
    // 动态测量最长标签宽度，同单闭环
    const measurer = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    measurer.setAttribute('font-size', '18');
    measurer.setAttribute('font-family', "'Inter', sans-serif");
    measurer.setAttribute('visibility', 'hidden');
    svg.appendChild(measurer);
    let maxTextWidth = 0;
    for (let i = 0; i < MULTI_AGENT_DEFS.length; i++) {
      measurer.textContent = MULTI_AGENT_DEFS[i].label;
      const tw = measurer.getComputedTextLength();
      if (tw > maxTextWidth) maxTextWidth = tw;
    }
    svg.removeChild(measurer);
    return MULTI_FN_PADDING_LEFT + MULTI_FN_RADIUS * 2 + MULTI_FN_CIRCLE_GAP + maxTextWidth + MULTI_FN_PADDING_RIGHT;
  })();

  // Compute agent positions
  const agents = MULTI_AGENT_DEFS.map((def, i) => ({
    id: def.id,
    label: def.label,
    icon: def.icon,
    x: MULTI_FN_START_X + MULTI_FN_GAP_X * i,
    y: MULTI_FN_Y,
  }));

  const totalW = MULTI_FN_START_X + MULTI_FN_GAP_X * 4 + nodeW + 40;
  const totalH = MULTI_INNER_BOX_Y + MULTI_INNER_BOX_H + 45 + 25;
  svg.setAttribute('viewBox', '0 0 ' + totalW + ' ' + totalH);

  // 内层框位置（动态计算）
  const innerBoxX = MULTI_FN_START_X + MULTI_FN_GAP_X * 1 + nodeW + 25;
  const innerBoxW = (MULTI_FN_START_X + MULTI_FN_GAP_X * 3) - innerBoxX - 30;

  // Clear and add defs
  svg.innerHTML = '';
  const defs = document.createElementNS('http://www.w3.org/2000/svg', 'defs');
  defs.innerHTML = `
    <filter id="glow-blue" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="3" result="blur"/>
      <feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <filter id="glow-green" x="-50%" y="-50%" width="200%" height="200%">
      <feGaussianBlur stdDeviation="3" result="blur"/>
      <feMerge><feMergeNode in="blur"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#2a3550"/>
    </marker>
    <marker id="arrowhead-active" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#4a6cf7"/>
    </marker>
    <marker id="arrowhead-done" markerWidth="10" markerHeight="7" refX="10" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#06d6a0"/>
    </marker>
  `;
  svg.appendChild(defs);

  // === Outer loop boundary (dashed rectangle) ===
  const outerBorder = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
  outerBorder.setAttribute('x', 20);
  outerBorder.setAttribute('y', 25);
  outerBorder.setAttribute('width', totalW - 40);
  outerBorder.setAttribute('height', totalH - 50);
  outerBorder.setAttribute('rx', '12');
  outerBorder.setAttribute('fill', 'rgba(74, 108, 247, 0.03)');
  outerBorder.setAttribute('stroke', '#4a6cf7');
  outerBorder.setAttribute('stroke-width', '1.5');
  outerBorder.setAttribute('stroke-dasharray', '6,4');
  outerBorder.setAttribute('id', 'multi-outer-border');
  svg.appendChild(outerBorder);

  // Outer loop label
  const outerLabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
  outerLabel.setAttribute('x', 35);
  outerLabel.setAttribute('y', 48);
  outerLabel.setAttribute('fill', '#4a6cf7');
  outerLabel.setAttribute('font-size', '12');
  outerLabel.setAttribute('font-weight', '700');
  outerLabel.setAttribute('font-family', "'SF Mono', 'Fira Code', monospace");
  outerLabel.textContent = 'Strategy Loop (Outer)';
  svg.appendChild(outerLabel);

  // === Inner loop boundary (solid rectangle) ===
  const innerBorder = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
  innerBorder.setAttribute('x', innerBoxX);
  innerBorder.setAttribute('y', MULTI_INNER_BOX_Y);
  innerBorder.setAttribute('width', innerBoxW);
  innerBorder.setAttribute('height', MULTI_INNER_BOX_H);
  innerBorder.setAttribute('rx', '10');
  innerBorder.setAttribute('fill', 'rgba(6, 214, 160, 0.05)');
  innerBorder.setAttribute('stroke', '#06d6a0');
  innerBorder.setAttribute('stroke-width', '1.5');
  innerBorder.setAttribute('id', 'multi-inner-border');
  svg.appendChild(innerBorder);

  // Inner loop label
  const innerLabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
  innerLabel.setAttribute('x', innerBoxX + innerBoxW / 2);
  innerLabel.setAttribute('y', MULTI_INNER_BOX_Y + 18);
  innerLabel.setAttribute('fill', '#06d6a0');
  innerLabel.setAttribute('font-size', '11');
  innerLabel.setAttribute('font-weight', '700');
  innerLabel.setAttribute('text-anchor', 'middle');
  innerLabel.setAttribute('font-family', "'SF Mono', 'Fira Code', monospace");
  innerLabel.textContent = 'Execution Loop (Inner) — converge ↺';
  svg.appendChild(innerLabel);

  // === Inner micro-nodes (S → V → E) in compact form ===
  const innerAgents = [
    { id: 'inner_solver',    letter: 'S', label: 'Solver' },
    { id: 'inner_verifier',  letter: 'V', label: 'Verifier' },
    { id: 'inner_evaluator', letter: 'E', label: 'Evaluator' },
  ];
  const innerCircleCY = MULTI_INNER_BOX_Y + MULTI_INNER_BOX_H / 2;
  const innerTotalW = MULTI_INNER_R * 2 * 3 + MULTI_INNER_GAP * 2;
  const innerStartX = innerBoxX + (innerBoxW - innerTotalW) / 2;

  for (let i = 0; i < 3; i++) {
    const ix = innerStartX + i * (MULTI_INNER_R * 2 + MULTI_INNER_GAP);

    // Circle
    const c = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    c.setAttribute('cx', ix + MULTI_INNER_R);
    c.setAttribute('cy', innerCircleCY);
    c.setAttribute('r', MULTI_INNER_R);
    c.setAttribute('fill', '#0a192f');
    c.setAttribute('stroke', '#06d6a0');
    c.setAttribute('stroke-width', '2');
    c.setAttribute('id', 'multi-inner-circle-' + i);
    svg.appendChild(c);

    // Letter inside circle
    const t = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    t.setAttribute('x', ix + MULTI_INNER_R);
    t.setAttribute('y', innerCircleCY + 6);
    t.setAttribute('fill', '#06d6a0');
    t.setAttribute('font-size', '14');
    t.setAttribute('font-weight', '700');
    t.setAttribute('text-anchor', 'middle');
    t.setAttribute('font-family', "'Inter', sans-serif");
    t.textContent = innerAgents[i].letter;
    svg.appendChild(t);

    // Full label below circle
    const tl = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    tl.setAttribute('x', ix + MULTI_INNER_R);
    tl.setAttribute('y', innerCircleCY + MULTI_INNER_R + 14);
    tl.setAttribute('fill', '#8892b0');
    tl.setAttribute('font-size', '10');
    tl.setAttribute('font-weight', '600');
    tl.setAttribute('text-anchor', 'middle');
    tl.setAttribute('font-family', "'Inter', sans-serif");
    tl.textContent = innerAgents[i].label;
    svg.appendChild(tl);

    // Arrow between inner nodes
    if (i < 2) {
      const arr = document.createElementNS('http://www.w3.org/2000/svg', 'line');
      arr.setAttribute('x1', ix + MULTI_INNER_R * 2 + 2);
      arr.setAttribute('y1', innerCircleCY);
      arr.setAttribute('x2', ix + MULTI_INNER_R * 2 + MULTI_INNER_GAP - 2);
      arr.setAttribute('y2', innerCircleCY);
      arr.setAttribute('stroke', '#2a3550');
      arr.setAttribute('stroke-width', '1.5');
      arr.setAttribute('marker-end', 'url(#arrowhead)');
      svg.appendChild(arr);
    }
  }

  // Inner feedback loop arrow (evaluator → solver, bottom arc)
  const evalCX = innerStartX + 2 * (MULTI_INNER_R * 2 + MULTI_INNER_GAP) + MULTI_INNER_R;
  const solverCX = innerStartX + MULTI_INNER_R;
  const innerLoopPath = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  innerLoopPath.setAttribute('d',
    'M ' + (evalCX + MULTI_INNER_R + 5) + ',' + innerCircleCY +
    ' Q ' + (evalCX + MULTI_INNER_R + 30) + ',' + (innerCircleCY - 30) +
    ' ' + (solverCX - MULTI_INNER_R - 5) + ',' + innerCircleCY
  );
  innerLoopPath.setAttribute('class', 'fn-loop-arrow');
  innerLoopPath.setAttribute('id', 'multi-inner-feedback');
  innerLoopPath.setAttribute('fill', 'none');
  innerLoopPath.setAttribute('stroke', '#2a3550');
  innerLoopPath.setAttribute('stroke-width', '1');
  innerLoopPath.setAttribute('stroke-dasharray', '3,2');
  svg.appendChild(innerLoopPath);

  // === Arrows between main agents (0→1, 1→2, 2→3, 3→4) ===
  // y = MULTI_CIRCLE_CY (aligned with circle centers, same as single-loop)
  const arrowGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
  arrowGroup.setAttribute('class', 'fn-arrow-group');
  for (let i = 0; i < agents.length - 1; i++) {
    const from = agents[i];
    const to = agents[i + 1];
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', from.x + nodeW);
    line.setAttribute('y1', MULTI_CIRCLE_CY);
    line.setAttribute('x2', to.x);
    line.setAttribute('y2', MULTI_CIRCLE_CY);
    line.setAttribute('class', 'fn-arrow-line');
    line.setAttribute('id', 'multi-arrow-' + i);
    line.setAttribute('marker-end', 'url(#arrowhead)');
    arrowGroup.appendChild(line);
  }
  svg.appendChild(arrowGroup);

  // === Arrow from inner box right edge to outer_verifier left edge ===
  const inToOut = document.createElementNS('http://www.w3.org/2000/svg', 'line');
  inToOut.setAttribute('x1', innerBoxX + innerBoxW);
  inToOut.setAttribute('y1', innerCircleCY);
  inToOut.setAttribute('x2', agents[3].x);
  inToOut.setAttribute('y2', innerCircleCY);
  inToOut.setAttribute('stroke', '#2a3550');
  inToOut.setAttribute('stroke-width', '1.5');
  inToOut.setAttribute('marker-end', 'url(#arrowhead)');
  inToOut.setAttribute('id', 'multi-arrow-inner-out');
  svg.appendChild(inToOut);

  // === Feedback loop (Outer Evaluator → Strategy Refiner, bottom curve) ===
  const evalAgent = agents[4];
  const refAgent = agents[0];
  const fx1 = evalAgent.x + nodeW / 2;
  const fy1 = evalAgent.y + MULTI_FN_NODE_H;
  const fx2 = refAgent.x + nodeW / 2;
  const fy2 = refAgent.y + MULTI_FN_NODE_H;
  const fcy = totalH - 20;

  const loopGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');
  loopGroup.setAttribute('id', 'multi-loop-group');

  const fpath = document.createElementNS('http://www.w3.org/2000/svg', 'path');
  fpath.setAttribute('d', 'M ' + fx1 + ',' + fy1 + ' C ' + fx1 + ',' + fcy + ' ' + fx2 + ',' + fcy + ' ' + fx2 + ',' + fy2);
  fpath.setAttribute('class', 'fn-loop-arrow');
  fpath.setAttribute('id', 'multi-loop-arrow');
  fpath.setAttribute('fill', 'none');
  fpath.setAttribute('marker-end', 'url(#arrowhead)');
  loopGroup.appendChild(fpath);

  const flabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
  flabel.setAttribute('x', (fx1 + fx2) / 2);
  flabel.setAttribute('y', fcy - 4);
  flabel.setAttribute('class', 'fn-arrow-label');
  flabel.setAttribute('text-anchor', 'middle');
  flabel.setAttribute('id', 'multi-loop-label');
  flabel.textContent = 'outer feedback (iterative) — score < 0.8';
  loopGroup.appendChild(flabel);
  svg.appendChild(loopGroup);

  // === Agent node rendering (exactly like single-loop layout) ===
  const nodeGroup = document.createElementNS('http://www.w3.org/2000/svg', 'g');

  agents.forEach((agent) => {
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');

    // Background rect (match single-loop: rx=8)
    const rect = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    rect.setAttribute('x', agent.x);
    rect.setAttribute('y', agent.y);
    rect.setAttribute('width', nodeW);
    rect.setAttribute('height', MULTI_FN_NODE_H);
    rect.setAttribute('rx', '8');
    rect.setAttribute('class', 'fn-node-bg idle');
    rect.setAttribute('id', 'multi-node-' + agent.id);
    g.appendChild(rect);

    // Icon circle — 和单闭环完全一致：cx = x + FN_PADDING_LEFT + FN_RADIUS, cy = y + 35
    const cx = agent.x + MULTI_FN_PADDING_LEFT + MULTI_FN_RADIUS;
    const cy = MULTI_CIRCLE_CY;
    const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    circle.setAttribute('cx', cx);
    circle.setAttribute('cy', cy);
    circle.setAttribute('r', MULTI_FN_RADIUS);
    circle.setAttribute('class', 'fn-icon-circle');
    circle.setAttribute('id', 'multi-icon-' + agent.id);
    g.appendChild(circle);

    // Icon letter (centered in circle, same font-size & y: cy+6)
    const iconText = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    iconText.setAttribute('x', cx);
    iconText.setAttribute('y', cy + 6);
    iconText.setAttribute('class', 'fn-agent-label');
    iconText.setAttribute('font-size', '16');
    iconText.setAttribute('font-weight', '700');
    iconText.setAttribute('text-anchor', 'middle');
    iconText.textContent = agent.icon;
    g.appendChild(iconText);

    // Agent label (right of circle) — 和单闭环一致：x + FN_PADDING_LEFT + FN_RADIUS*2 + FN_CIRCLE_GAP
    const labelX = agent.x + MULTI_FN_PADDING_LEFT + MULTI_FN_RADIUS * 2 + MULTI_FN_CIRCLE_GAP;
    const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    label.setAttribute('x', labelX);
    label.setAttribute('y', cy + 7);
    label.setAttribute('class', 'fn-agent-label');
    label.setAttribute('font-size', '18');
    label.setAttribute('font-weight', '600');
    label.setAttribute('text-anchor', 'start');
    label.setAttribute('id', 'multi-label-' + agent.id);
    label.textContent = agent.label;
    g.appendChild(label);

    // Status text (top-right corner)
    const status = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    status.setAttribute('x', agent.x + nodeW - 16);
    status.setAttribute('y', agent.y + 20);
    status.setAttribute('class', 'fn-agent-status');
    status.setAttribute('font-size', '11');
    status.setAttribute('text-anchor', 'end');
    status.setAttribute('id', 'multi-status-' + agent.id);
    status.textContent = 'idle';
    g.appendChild(status);

    nodeGroup.appendChild(g);
  });

  svg.appendChild(nodeGroup);
}

function updateMultiFlowNode(agentId, status) {
  const rect = document.getElementById('multi-node-' + agentId);
  const statusText = document.getElementById('multi-status-' + agentId);
  if (!rect) return;
  rect.setAttribute('class', 'fn-node-bg ' + status);
  if (statusText) {
    statusText.textContent = status === 'active' ? 'running...' : status === 'done' ? '✓ done' : status;
  }
}

function updateMultiInnerNodeStatus(status) {
  for (let i = 0; i < 3; i++) {
    const circle = document.getElementById('multi-inner-circle-' + i);
    if (circle) {
      if (status === 'active') {
        circle.setAttribute('stroke', '#4a6cf7');
        circle.setAttribute('filter', 'url(#glow-blue)');
      } else if (status === 'done') {
        circle.setAttribute('stroke', '#06d6a0');
        circle.setAttribute('filter', '');
      } else {
        circle.setAttribute('stroke', '#06d6a0');
        circle.setAttribute('filter', '');
      }
    }
  }
}

function updateMultiArrow(index, status) {
  const arrow = document.getElementById('multi-arrow-' + index);
  if (!arrow) return;
  const marker = status === 'active' ? 'url(#arrowhead-active)' :
                 status === 'done' ? 'url(#arrowhead-done)' : 'url(#arrowhead)';
  arrow.setAttribute('marker-end', marker);
  arrow.setAttribute('stroke', status === 'active' ? '#4a6cf7' :
                                  status === 'done' ? '#06d6a0' : '#2a3550');
}

function resetMultiFlowchart() {
  const agents = ['strategy_refiner', 'strategy_reasoner', 'execution_loop', 'outer_verifier', 'outer_evaluator'];
  agents.forEach(id => updateMultiFlowNode(id, 'idle'));
  for (let i = 0; i < 4; i++) updateMultiArrow(i, 'idle');
  updateMultiInnerNodeStatus('idle');
  const loopArrow = document.getElementById('multi-loop-arrow');
  if (loopArrow) {
    loopArrow.setAttribute('stroke', '#2a3550');
    loopArrow.setAttribute('marker-end', 'url(#arrowhead)');
  }
}

function updateMultiLoopArrow(status) {
  const arrow = document.getElementById('multi-loop-arrow');
  if (!arrow) return;
  const marker = status === 'active' ? 'url(#arrowhead-active)' :
                 status === 'done' ? 'url(#arrowhead-done)' : 'url(#arrowhead)';
  arrow.setAttribute('marker-end', marker);
  arrow.setAttribute('stroke', status === 'active' ? '#4a6cf7' :
                                  status === 'done' ? '#06d6a0' : '#2a3550');
}

function updateMultiFlowchartFromSSE(event) {
  const ev = event.event;
  const agent = event.agent || '';
  const loopLevel = event.loop_level || '';

  if (loopLevel === 'outer') {
    const agentToId = {
      'strategy_refiner': 'strategy_refiner',
      'strategy_reasoner': 'strategy_reasoner',
      'execution_loop': 'execution_loop',
      'outer_verifier': 'outer_verifier',
      'outer_evaluator': 'outer_evaluator',
    };
    if (ev === 'agent_start' && agentToId[agent]) {
      updateMultiFlowNode(agentToId[agent], 'active');
    } else if (ev === 'agent_complete' && agentToId[agent]) {
      updateMultiFlowNode(agentToId[agent], 'done');
      const arrowIdx = {strategy_refiner: 0, strategy_reasoner: 1, execution_loop: 2, outer_verifier: 3};
      if (arrowIdx[agent] !== undefined) updateMultiArrow(arrowIdx[agent], 'done');
    }
    if (ev === 'inner_loop_start') {
      updateMultiFlowNode('execution_loop', 'active');
      updateMultiInnerNodeStatus('active');
    }
    if (ev === 'iteration' && loopLevel === 'outer') {
      const scores = event.scores || {};
      if (scores.overall >= 0.8) {
        updateMultiLoopArrow('done');
      }
    }
  }

  if (loopLevel === 'inner') {
    if (ev === 'agent_start') {
      const innerIdx = {inner_solver: 0, inner_verifier: 1, inner_evaluator: 2}[agent];
      if (innerIdx !== undefined) {
        const circles = document.querySelectorAll('[id^="multi-inner-circle-"]');
        circles.forEach((c, i) => {
          c.setAttribute('stroke', i === innerIdx ? '#4a6cf7' : '#06d6a0');
          c.setAttribute('filter', i === innerIdx ? 'url(#glow-blue)' : '');
        });
      }
    } else if (ev === 'agent_complete') {
      const innerIdx = {inner_solver: 0, inner_verifier: 1, inner_evaluator: 2}[agent];
      if (innerIdx !== undefined) {
        const circle = document.getElementById('multi-inner-circle-' + innerIdx);
        if (circle) {
          circle.setAttribute('stroke', '#06d6a0');
          circle.setAttribute('filter', '');
        }
      }
    } else if (ev === 'inner_iteration') {
      updateMultiInnerNodeStatus('done');
    } else if (ev === 'inner_done') {
      updateMultiInnerNodeStatus('done');
      updateMultiFlowNode('execution_loop', 'done');
    }
  }
}

function updateFlowNode(agentId, status, iteration, preview, durationMs) {
  const rect = document.getElementById('fn-node-' + agentId);
  const statusText = document.getElementById('fn-status-' + agentId);
  const previewText = document.getElementById('fn-preview-' + agentId);
  const scoreText = document.getElementById('fn-score-' + agentId);

  if (!rect) return;

  // Update state tracking and SVG class
  flowchartNodeStates[agentId] = status;
  rect.setAttribute('class', 'fn-node-bg ' + status);

  if (statusText) {
    if (status === 'active') {
      statusText.textContent = 'running' + (iteration ? ' [#' + iteration + ']' : '');
    } else if (status === 'done') {
      statusText.textContent = '✓ done';
    } else if (status === 'error') {
      statusText.textContent = '✗ error';
    } else {
      statusText.textContent = 'idle';
    }
  }

  if (previewText && preview) {
    var nodeW = parseFloat(rect.getAttribute('width'));
    var availWidth = nodeW - 30; // 15px padding each side

    // Use SVG's getSubStringLength for precise width measurement (handles CJK + mixed scripts)
    var measurer = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    measurer.setAttribute('font-size', '10');
    measurer.setAttribute('font-family', "'SF Mono', 'Fira Code', 'Cascadia Code', monospace");
    measurer.setAttribute('visibility', 'hidden');
    rect.parentNode.appendChild(measurer);

    var lines = [];
    var remaining = preview;
    while (remaining.length > 0) {
      // Find how many characters fit in one line using binary search on getSubStringLength
      measurer.textContent = remaining;
      var n = remaining.length;
      var lo = 1, hi = n;
      while (lo < hi) {
        var mid = Math.ceil((lo + hi) / 2);
        var w = measurer.getSubStringLength(0, mid);
        if (w <= availWidth) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      var fitCount = lo;

      if (fitCount >= n) {
        // All fits
        lines.push(remaining);
        break;
      }

      // Try to break at a space for nice word-wrap
      var breakAt = fitCount;
      if (fitCount > 10) {
        var spaceIdx = remaining.lastIndexOf(' ', fitCount);
        if (spaceIdx > 5) breakAt = spaceIdx;
        // For CJK: try break after punctuation
        var punctIdx = -1;
        for (var ci = fitCount; ci > 0; ci--) {
          var ch = remaining.charAt(ci);
          if (ch === '，' || ch === '。' || ch === '、' || ch === '；' || ch === '：' || ch === '）' || ch === '！' || ch === '？') {
            punctIdx = ci;
            break;
          }
        }
        if (punctIdx > 5 && punctIdx < fitCount) breakAt = punctIdx + 1;
      }

      lines.push(remaining.substring(0, breakAt));
      remaining = remaining.substring(breakAt).trim();
      if (lines.length >= 3) {
        // For the last line, check if we need '...'
        var lastW = measurer.getSubStringLength(0, Math.min(n, breakAt));
        var linesW = 0;
        // Use the already-pushed line's substring length
        lines[lines.length - 1] = lines[lines.length - 1] + '...';
        break;
      }
    }
    rect.parentNode.removeChild(measurer);

    // Clear old content and create tspans
    var group = document.getElementById('fn-preview-' + agentId + '-group');
    if (group) {
      group.innerHTML = '';
      var baseY = parseFloat(previewText.getAttribute('y'));
      for (var li = 0; li < lines.length; li++) {
        var tspan = document.createElementNS('http://www.w3.org/2000/svg', 'text');
        tspan.setAttribute('x', previewText.getAttribute('x'));
        tspan.setAttribute('y', baseY + li * 14);
        tspan.setAttribute('class', 'fn-preview-text');
        tspan.setAttribute('font-size', '10');
        tspan.setAttribute('text-anchor', 'middle');
        tspan.textContent = lines[li];
        group.appendChild(tspan);
      }
    }
  }

  if (scoreText && status === 'done' && durationMs) {
    scoreText.textContent = (durationMs < 1000 ? durationMs.toFixed(0) + 'ms' : (durationMs / 1000).toFixed(1) + 's');
  }

  // Animate arrows
  const agentIdx = FLOWCHART_AGENTS.findIndex(a => a.id === agentId);
  if (status === 'active' && agentIdx < FLOWCHART_ARROWS.length) {
    // Activate the arrow coming out of this node
    const arrowEl = document.getElementById('fn-arrow-' + agentIdx);
    if (arrowEl) {
      arrowEl.setAttribute('class', 'fn-arrow-line active');
      arrowEl.setAttribute('marker-end', 'url(#arrowhead-active)');
    }
  } else if (status === 'done' && agentIdx < FLOWCHART_ARROWS.length) {
    const arrowEl = document.getElementById('fn-arrow-' + agentIdx);
    if (arrowEl) {
      arrowEl.setAttribute('class', 'fn-arrow-line done');
      arrowEl.setAttribute('marker-end', 'url(#arrowhead-done)');
    }
  }
}

// ============================================================================
// SESSION (HISTORY SIDEBAR)
// ============================================================================

var currentSessionId = null;

async function refreshSessionList() {
  try {
    const data = await api('/api/sessions');
    const list = document.getElementById('sessionList');
    const count = data.count || 0;

    if (count === 0) {
      list.innerHTML = '<div class="session-empty">No sessions yet.<br>Execute a query to begin.</div>';
      return;
    }

    list.innerHTML = data.sessions.map(s => {
      const activeClass = currentSessionId === s.id ? ' active' : '';
      const preview = s.preview ? escapeHtml(s.preview).substring(0, 60) : '';
      const time = s.updated_at ? new Date(s.updated_at * 1000).toLocaleString('zh-CN', { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }) : '';
      return `
        <div class="session-item${activeClass}" onclick="loadSession('${s.id}')">
          <div class="session-item-content">
            <div class="session-item-name">${escapeHtml(s.name || 'Untitled').substring(0, 40)}</div>
            <div class="session-item-preview">${preview || time}</div>
          </div>
          <div class="session-item-meta">${s.query_count || 0} queries</div>
          <button class="session-item-delete" onclick="event.stopPropagation(); deleteSession('${s.id}')" title="Delete">×</button>
        </div>
      `;
    }).join('');
  } catch (e) {
    console.error('Failed to load sessions:', e);
  }
}

async function loadSession(sessionId) {
  try {
    const session = await api('/api/sessions/' + sessionId);
    if (!session || session.error) return;

    currentSessionId = session.id;

    // Highlight in sidebar
    document.querySelectorAll('.session-item').forEach(el => el.classList.remove('active'));
    const item = document.querySelector(`.session-item[onclick*="'${sessionId}'"]`);
    if (item) item.classList.add('active');

    // Restore the last query and result
    const messages = session.messages || [];
    const assistantMsgs = messages.filter(m => m.role === 'assistant');
    if (assistantMsgs.length > 0) {
      const last = assistantMsgs[assistantMsgs.length - 1];

      // Restore query in input
      if (last.query) {
        document.getElementById('queryInput').value = last.query;
      }

      // Show results
      const section = document.getElementById('resultsSection');
      section.classList.remove('hidden');

      // Show flowchart (empty state)
      const flowchartPanel = document.getElementById('flowchartPanel');
      flowchartPanel.classList.remove('hidden');
      if (currentArchMode === 'multi') {
        initMultiLoopFlowchart();
      } else {
        initFlowchart();
      }

      // Show scores
      const scores = last.scores || {};
      drawGauge(scores.overall || 0);
      animateBar('scoreBar1', 'scoreVal1', scores.reasonableness || 0);
      animateBar('scoreBar2', 'scoreVal2', scores.executability || 0);
      animateBar('scoreBar3', 'scoreVal3', scores.satisfaction || 0);

      // Show status
      const statusEl = document.getElementById('resultStatus');
      statusEl.className = 'result-status success';
      statusEl.textContent = '↩ Loaded from history';

      // Show output
      const result = last.result || {};
      const output = document.getElementById('outputContent');
      output.textContent = result.content || '[No output]';

      // Timeline
      const history = result.iterations || [];
      renderTimeline(history);
    }
  } catch (e) {
    console.error('Failed to load session:', e);
  }
}

async function deleteSession(sessionId) {
  if (!confirm('Delete this session?')) return;
  try {
    await api('/api/sessions/' + sessionId, 'DELETE');
    if (currentSessionId === sessionId) {
      currentSessionId = null;
    }
    refreshSessionList();
  } catch (e) {
    console.error('Failed to delete session:', e);
  }
}

function newSession() {
  currentSessionId = null;
  // Clear the input
  document.getElementById('queryInput').value = '';
  // Hide results
  document.getElementById('resultsSection').classList.add('hidden');
  document.getElementById('flowchartPanel').classList.add('hidden');
  document.getElementById('processingStatus').classList.add('hidden');

  // Deselect all sidebar items
  document.querySelectorAll('.session-item').forEach(el => el.classList.remove('active'));
  document.getElementById('queryInput').focus();
}

function filterSessions() {
  const query = document.getElementById('sessionSearchInput').value.toLowerCase().trim();
  document.querySelectorAll('.session-item').forEach(el => {
    const text = el.textContent.toLowerCase();
    el.style.display = (!query || text.includes(query)) ? '' : 'none';
  });
}

function toggleSidebar() {
  const sidebar = document.getElementById('sidebar');
  sidebar.classList.toggle('collapsed');
}

// === Cancel Query ===
function cancelQuery() {
  if (!processing) return;

  queryCancelled = true;

  // Close the SSE connection
  if (activeEventSource) {
    activeEventSource.close();
    activeEventSource = null;
  }

  // Show cancelled status
  document.getElementById('processingStatus').classList.add('hidden');

  const statusEl = document.getElementById('resultStatus');
  statusEl.className = 'result-status failure';
  statusEl.textContent = '⏹ Processing cancelled by user';

  // Restore submit button
  processing = false;
  const btn = document.getElementById('submitBtn');
  btn.disabled = false;
  btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
}

function resetProcessingState() {
  processing = false;
  queryCancelled = false;
  if (activeEventSource) {
    activeEventSource.close();
    activeEventSource = null;
  }
  const btn = document.getElementById('submitBtn');
  btn.disabled = false;
  btn.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12h14M12 5l7 7-7 7"/></svg> Execute';
}

// ============================================================================
// FLOWCHART — Pan & Zoom Controls
// ============================================================================

// State for flowchart pan/zoom
var _fcScale = 1;
var _fcPanX = 0;
var _fcPanY = 0;
var _fcIsDragging = false;
var _fcDragStartX, _fcDragStartY;
var _fcStartPanX, _fcStartPanY;

function initFlowchartPanZoom() {
  var viewport = document.getElementById('flowchartViewport');
  if (!viewport) return;

  // Mouse wheel zoom centered on cursor
  viewport.addEventListener('wheel', function(e) {
    if (!document.getElementById('flowchartPanel').classList.contains('hidden')) {
      e.preventDefault();
      var rect = viewport.getBoundingClientRect();
      var mx = e.clientX - rect.left;
      var my = e.clientY - rect.top;
      // Cursor position in SVG coords before zoom
      var cx = (mx - _fcPanX) / _fcScale;
      var cy = (my - _fcPanY) / _fcScale;
      var factor = e.deltaY < 0 ? 1.1 : 0.9;
      var newScale = Math.min(3, Math.max(0.3, _fcScale * factor));
      // Adjust pan so cursor stays in place
      _fcPanX = mx - cx * newScale;
      _fcPanY = my - cy * newScale;
      _fcScale = newScale;
      applyFlowchartTransform();
    }
  }, { passive: false });

  // Mouse drag to pan
  viewport.addEventListener('mousedown', function(e) {
    if (e.target.tagName === 'BUTTON') return;
    _fcIsDragging = true;
    _fcDragStartX = e.clientX;
    _fcDragStartY = e.clientY;
    _fcStartPanX = _fcPanX;
    _fcStartPanY = _fcPanY;
    viewport.style.cursor = 'grabbing';
  });

  document.addEventListener('mousemove', function(e) {
    if (!_fcIsDragging) return;
    _fcPanX = _fcStartPanX + (e.clientX - _fcDragStartX);
    _fcPanY = _fcStartPanY + (e.clientY - _fcDragStartY);
    applyFlowchartTransform();
  });

  document.addEventListener('mouseup', function() {
    if (_fcIsDragging) {
      _fcIsDragging = false;
      viewport.style.cursor = 'grab';
    }
  });
}

function applyFlowchartTransform() {
  var container = document.getElementById('flowchartContainer');
  if (!container) return;
  container.style.transform = 'translate(' + _fcPanX + 'px, ' + _fcPanY + 'px) scale(' + _fcScale + ')';
  container.style.transformOrigin = '0 0';
}

function zoomFlowchart(factor) {
  var viewport = document.getElementById('flowchartViewport');
  if (!viewport) return;
  var cx = viewport.clientWidth / 2;
  var cy = viewport.clientHeight / 2;
  var svgX = (cx - _fcPanX) / _fcScale;
  var svgY = (cy - _fcPanY) / _fcScale;
  var newScale = Math.min(3, Math.max(0.3, _fcScale * factor));
  _fcPanX = cx - svgX * newScale;
  _fcPanY = cy - svgY * newScale;
  _fcScale = newScale;
  applyFlowchartTransform();
}

function resetFlowchartView() {
  _fcScale = 1;
  _fcPanX = 0;
  _fcPanY = 0;
  applyFlowchartTransform();
}

// Update mode badge in flowchart header
function updateFlowModeBadge(mode) {
  var badge = document.getElementById('flowModeBadge');
  if (!badge) return;
  badge.textContent = mode === 'closed' ? 'CLOSED' : 'OPEN';
  badge.className = 'badge mode-badge ' + (mode === 'closed' ? 'badge-closed' : 'badge-open');
}

// Patch updateMode to also update flowchart badge
var _origUpdateMode = window.updateMode;
window.updateMode = function(mode) {
  if (_origUpdateMode) _origUpdateMode(mode);
  updateFlowModeBadge(mode);
};

// Init pan/zoom on DOM load
document.addEventListener('DOMContentLoaded', function() {
  setTimeout(initFlowchartPanZoom, 100);
});
