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

# Sessions directory (one JSON file per session)
BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS_DIR = os.path.join(BASE_DIR, 'config', 'sessions')

_lock = threading.Lock()


def ensure_dir():
    """Create sessions directory if it doesn't exist."""
    os.makedirs(SESSIONS_DIR, exist_ok=True)


def _safe_path(session_id):
    """Return safe file path for a session ID; raises ValueError on invalid ID."""
    # Only allow alphanumeric, hyphens, underscores
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


def list_sessions(limit=50):
    """Return list of session summaries (no messages), newest first.

    Each summary: {id, name, created_at, updated_at, query_count, preview}
    """
    ensure_dir()
    pattern = os.path.join(SESSIONS_DIR, '*.json')
    files = glob.glob(pattern)
    # Sort by modification time (newest first)
    files.sort(key=os.path.getmtime, reverse=True)
    files = files[:limit]

    results = []
    for path in files:
        try:
            with open(path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            msgs = data.get('messages', [])
            # Build preview from last assistant message or first user message
            preview = ''
            for msg in reversed(msgs):
                if msg.get('role') == 'assistant' and msg.get('query'):
                    preview = msg['query'][:100]
                    break
                if msg.get('role') == 'user':
                    preview = msg['query'][:100]
                    break
            results.append({
                'id': data['id'],
                'name': data.get('name', 'Untitled'),
                'created_at': data.get('created_at', 0),
                'updated_at': data.get('updated_at', 0),
                'query_count': data.get('query_count', len(msgs) // 2),
                'preview': preview,
            })
        except (json.JSONDecodeError, KeyError, IOError):
            continue
    return results


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
