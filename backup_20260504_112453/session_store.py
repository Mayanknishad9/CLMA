"""
CLMA Session Store — JSON file-based persistent session storage.

Stores conversation sessions as individual JSON files in config/sessions/.
Each file contains messages, scores, stats, and timestamps.
"""
import os
import json
import time
import uuid
import glob
import threading
from datetime import datetime, timezone

# Sessions directory (one JSON file per session)
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS_DIR = os.path.join(BASE_DIR, 'config', 'sessions')

_lock = threading.Lock()


def ensure_dir():
    """Create sessions directory if it doesn't exist."""
    os.makedirs(SESSIONS_DIR, exist_ok=True)


def _safe_path(session_id):
    """Return safe file path for a session ID; raises ValueError on invalid ID."""
    import re
    if not re.match(r'^[a-zA-Z0-9_-]+$', session_id):
        raise ValueError(f"Invalid session ID: {session_id}")
    ensure_dir()
    return os.path.join(SESSIONS_DIR, f"{session_id}.json")


def _load_session(session_id):
    """Load session dict from file, or return None."""
    path = _safe_path(session_id)
    if not os.path.exists(path):
        return None
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except (json.JSONDecodeError, IOError):
        return None


def _save_session(session):
    """Write session dict to file."""
    path = _safe_path(session['id'])
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(session, f, ensure_ascii=False, indent=2)


def _ts_to_date_label(ts):
    """Convert a unix timestamp to a date label like '2026.5.3'."""
    if not ts:
        return ''
    dt = datetime.fromtimestamp(ts)
    return f"{dt.year}.{dt.month}.{dt.day}"


def _ts_to_date_key(ts):
    """Convert a unix timestamp to a sortable date key like '2026-05-03'."""
    if not ts:
        return ''
    dt = datetime.fromtimestamp(ts)
    return f"{dt.year}-{dt.month:02d}-{dt.day:02d}"


def _is_today(ts):
    """Check if a unix timestamp falls on today's date (UTC)."""
    if not ts:
        return False
    dt = datetime.fromtimestamp(ts)
    today = datetime.fromtimestamp(time.time())
    return (dt.year, dt.month, dt.day) == (today.year, today.month, today.day)


def _session_summary(data):
    """Build a summary dict from a full session dict (no messages)."""
    msgs = data.get('messages', [])
    preview = ''
    for msg in reversed(msgs):
        if msg.get('role') == 'assistant' and msg.get('query'):
            preview = msg['query'][:100]
            break
        if msg.get('role') == 'user':
            preview = msg['query'][:100]
            break
    return {
        'id': data['id'],
        'name': data.get('name', 'Untitled'),
        'created_at': data.get('created_at', 0),
        'updated_at': data.get('updated_at', 0),
        'query_count': data.get('query_count', len(msgs) // 2),
        'preview': preview,
    }


def list_sessions_grouped(limit=200):
    """Return sessions grouped by date, newest first.

    Returns:
        dict with:
          - groups: list of {date, dateLabel, sessions, queries, completes}
          - today: {queries, completes}
          - count: total sessions
    """
    ensure_dir()
    pattern = os.path.join(SESSIONS_DIR, '*.json')
    files = glob.glob(pattern)
    files.sort(key=os.path.getmtime, reverse=True)
    files = files[:limit]

    # Group by date key
    groups_map = {}  # date_key -> {date, dateLabel, sessions, queries, completes, _earliest_ts}
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            summary = _session_summary(data)
            ts = data.get('updated_at', data.get('created_at', 0))
            dk = _ts_to_date_key(ts)
            dl = _ts_to_date_label(ts)
            if dk not in groups_map:
                groups_map[dk] = {
                    'date': dk,
                    'dateLabel': dl,
                    'sessions': [],
                    'queries': 0,
                    'completes': 0,
                    'tokens': 0,
                    '_ts': ts,
                }
            grp = groups_map[dk]
            grp['sessions'].append(summary)
            grp['queries'] += data.get('query_count', len(data.get('messages', [])) // 2)
            # 'completes' = number of assistant messages (successful completions)
            assistant_count = len([m for m in data.get('messages', []) if m.get('role') == 'assistant'])
            grp['completes'] += assistant_count
            # 'tokens' = sum of total_token_usage from assistant messages' stats
            for msg in data.get('messages', []):
                if msg.get('role') == 'assistant':
                    msg_stats = msg.get('stats', {})
                    grp['tokens'] += int(msg_stats.get('total_token_usage', 0))
            # Keep track of earliest timestamp for group ordering
            if ts > grp['_ts']:
                grp['_ts'] = ts
        except (json.JSONDecodeError, KeyError, IOError):
            continue

    # Sort groups by timestamp (newest first)
    sorted_groups = sorted(groups_map.values(), key=lambda g: g['_ts'], reverse=True)
    for grp in sorted_groups:
        del grp['_ts']

    # Calculate today's stats — aggregate queries, completes, and tokens
    today_queries = 0
    today_completes = 0
    today_tokens = 0
    for grp in sorted_groups:
        if grp['date']:
            try:
                dt = datetime.strptime(grp['date'], '%Y-%m-%d')
                if _is_today(dt.timestamp()):
                    today_queries += grp['queries']
                    today_completes += grp['completes']
                    today_tokens += grp['tokens']
            except ValueError:
                pass

    total_count = sum(len(g['sessions']) for g in sorted_groups)

    return {
        'groups': sorted_groups,
        'today': {
            'queries': today_queries,
            'completes': today_completes,
            'tokens': today_tokens,
        },
        'count': total_count,
    }


def create_session(name="New Session"):
    """Create a new empty session, return session dict."""
    with _lock:
        session_id = f"sess_{uuid.uuid4().hex[:12]}"
        now = time.time()
        session = {
            'id': session_id,
            'name': name,
            'created_at': now,
            'updated_at': now,
            'query_count': 0,
            'messages': [],
        }
        _save_session(session)
        return session


def get_session(session_id):
    """Return full session dict (with messages), or None if not found."""
    return _load_session(session_id)


def delete_session(session_id):
    """Remove session file. Returns True if deleted, False if not found."""
    path = _safe_path(session_id)
    if os.path.exists(path):
        os.remove(path)
        return True
    return False


def delete_sessions_by_date(date_key):
    """Delete all sessions whose date key matches the given date (e.g. '2026-05-03').
    Returns the number of deleted sessions."""
    ensure_dir()
    pattern = os.path.join(SESSIONS_DIR, '*.json')
    files = glob.glob(pattern)
    deleted = 0
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            ts = data.get('updated_at', data.get('created_at', 0))
            dk = _ts_to_date_key(ts)
            if dk == date_key:
                os.remove(path)
                deleted += 1
        except (json.JSONDecodeError, IOError):
            continue
    return deleted


def rename_session(session_id, new_name):
    """Update session name. Returns updated session or None."""
    with _lock:
        session = _load_session(session_id)
        if session is None:
            return None
        session['name'] = new_name
        session['updated_at'] = time.time()
        _save_session(session)
        return session


def add_message(session_id, query, result=None, scores=None, stats=None,
                duration_ms=None, mode=None):
    """Append a user query + optional assistant result to a session.

    If the session doesn't exist, it is auto-created.

    Args:
        session_id: Session ID (must exist or will be created)
        query: The user's query
        result: Full processing result dict
        scores: Dict with reasonableness/executability/satisfaction/overall
        stats: System stats dict
        duration_ms: Total processing time in ms
        mode: 'closed' or 'open'

    Returns:
        Updated session dict, or None on failure.
    """
    with _lock:
        session = _load_session(session_id)
        if session is None:
            # Auto-create session
            now = time.time()
            session = {
                'id': session_id,
                'name': query[:50],
                'created_at': now,
                'updated_at': now,
                'query_count': 0,
                'messages': [],
            }

        now = time.time()
        msg_id = f"msg_{uuid.uuid4().hex[:8]}"

        # User message
        session['messages'].append({
            'id': f"{msg_id}_user",
            'role': 'user',
            'query': query,
            'timestamp': now,
        })

        # Assistant message (if result exists)
        if result is not None:
            assistant_msg = {
                'id': f"{msg_id}_assistant",
                'role': 'assistant',
                'query': query,
                'result': result,
                'timestamp': now,
            }
            if scores:
                assistant_msg['scores'] = scores
            if stats:
                assistant_msg['stats'] = stats
            if duration_ms is not None:
                assistant_msg['duration_ms'] = duration_ms
            if mode:
                assistant_msg['mode'] = mode
            session['messages'].append(assistant_msg)

        # Update metadata
        session['query_count'] = len([m for m in session['messages'] if m['role'] == 'user'])
        session['updated_at'] = now
        if not session.get('name') or session['name'] == 'New Session':
            session['name'] = query[:50]

        _save_session(session)
        return session


# Keep backward-compatible list_sessions for any existing references
def list_sessions(limit=50):
    """Return list of session summaries (no messages), newest first.
    Legacy — prefer list_sessions_grouped for grouped view.
    """
    ensure_dir()
    pattern = os.path.join(SESSIONS_DIR, '*.json')
    files = glob.glob(pattern)
    files.sort(key=os.path.getmtime, reverse=True)
    files = files[:limit]
    results = []
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            results.append(_session_summary(data))
        except (json.JSONDecodeError, KeyError, IOError):
            continue
    return results
