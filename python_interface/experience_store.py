"""ExperienceStore — Persistent memory for successful query-solution pairs.

Provides:
- Char n-gram based embedding (zero external dependencies)
- Cosine similarity search
- JSONL-based persistence
- Pruning by score to manage size

Used by CLMAFramework to cache and recall successful solutions.
"""

import json
import math
import os
import time
from dataclasses import dataclass, field
from typing import Optional


# Embedding dimensionality — 256 floats per vector (~2KB each)
S = 256


def _embed(text: str) -> list[float]:
    """Convert a text string to an L2-normalized char n-gram hash vector.

    Uses 3-gram character hashing into an S-dimensional vector.
    Zero external dependencies — pure Python stdlib.

    Args:
        text: Input text (query or solution).

    Returns:
        A list of S floats, L2-normalized (or all zeros if text is empty).
    """
    vec = [0.0] * S

    if not text:
        return vec

    text = text.lower()
    n = 3
    for i in range(len(text) - n + 1):
        gram = text[i : i + n]
        idx = hash(gram) % S
        vec[idx] += 1.0

    # L2 normalize
    norm = math.sqrt(sum(v * v for v in vec))
    if norm > 0:
        vec = [v / norm for v in vec]

    return vec


def cosine_sim(a: list[float], b: list[float]) -> float:
    """Cosine similarity between two vectors.

    Args:
        a, b: Two vectors of equal length.

    Returns:
        Cosine similarity in [-1, 1].
    """
    if len(a) != len(b):
        raise ValueError(f"Vector size mismatch: {len(a)} vs {len(b)}")
    dot = sum(ai * bi for ai, bi in zip(a, b))
    norm_a = math.sqrt(sum(ai * ai for ai in a))
    norm_b = math.sqrt(sum(bi * bi for bi in b))
    if norm_a == 0 or norm_b == 0:
        return 0.0
    return dot / (norm_a * norm_b)


@dataclass
class ExperienceEntry:
    """A single experience record stored in the knowledge base."""

    query: str
    solution: str
    verification_report: dict = field(default_factory=dict)
    scores: dict = field(default_factory=dict)
    iterations_used: int = 0
    total_tokens: int = 0
    timestamp: float = 0.0
    embedding: Optional[list[float]] = None


class ExperienceStore:
    """Persistent experience store with embedding-based similarity search.

    Stores successful (above-threshold) query-solution pairs and retrieves
    them by similarity to new queries — avoiding redundant LLM calls.

    Storage format: one JSON object per line in `store_dir/entries.jsonl`.
    """

    def __init__(self, store_dir: str = "config/experience"):
        self.store_dir = store_dir
        self.entries: list[ExperienceEntry] = []
        self._loaded = False
        self._load()

    def _jsonl_path(self) -> str:
        return os.path.join(self.store_dir, "entries.jsonl")

    def _load(self):
        """Load all entries from JSONL on disk."""
        self.entries = []
        path = self._jsonl_path()
        if not os.path.exists(path):
            os.makedirs(self.store_dir, exist_ok=True)
            self._loaded = True
            return

        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                data = json.loads(line)
                entry = ExperienceEntry(
                    query=data["query"],
                    solution=data["solution"],
                    verification_report=data.get("verification_report", {}),
                    scores=data.get("scores", {}),
                    iterations_used=data.get("iterations_used", 0),
                    total_tokens=data.get("total_tokens", 0),
                    timestamp=data.get("timestamp", 0.0),
                )
                # Recompute embedding on load (avoids storing large vectors)
                entry.embedding = _embed(entry.query)
                self.entries.append(entry)
        self._loaded = True

    def _persist(self):
        """Append a single entry to the JSONL file."""
        os.makedirs(self.store_dir, exist_ok=True)
        path = self._jsonl_path()
        # Re-write all entries for consistency (pruning may remove entries)
        with open(path, "w", encoding="utf-8") as f:
            for entry in self.entries:
                data = {
                    "query": entry.query,
                    "solution": entry.solution,
                    "verification_report": entry.verification_report,
                    "scores": entry.scores,
                    "iterations_used": entry.iterations_used,
                    "total_tokens": entry.total_tokens,
                    "timestamp": entry.timestamp,
                }
                f.write(json.dumps(data, ensure_ascii=False) + "\n")

    def add(
        self,
        query: str,
        solution: str,
        verification_report: dict,
        scores: dict,
        iterations_used: int = 0,
        total_tokens: int = 0,
    ):
        """Store a new experience entry.

        The caller (core.py) decides whether to store based on score threshold.
        This just persists the entry.
        """
        entry = ExperienceEntry(
            query=query,
            solution=solution,
            verification_report=verification_report,
            scores=scores,
            iterations_used=iterations_used,
            total_tokens=total_tokens,
            timestamp=time.time(),
            embedding=_embed(query),
        )
        self.entries.append(entry)
        self._persist()

    def search(
        self, query: str, top_k: int = 3, threshold: float = 0.7
    ) -> list[ExperienceEntry]:
        """Search for the most similar entries by cosine similarity.

        Args:
            query: The user query to match against.
            top_k: Maximum number of results.
            threshold: Minimum cosine similarity to include.

        Returns:
            List of matched ExperienceEntry objects, sorted by relevance.
        """
        if not self.entries:
            return []

        q_emb = _embed(query)
        scored = []
        for entry in self.entries:
            if entry.embedding is None:
                entry.embedding = _embed(entry.query)
            sim = cosine_sim(q_emb, entry.embedding)
            scored.append((sim, entry))

        scored.sort(key=lambda x: x[0], reverse=True)
        return [entry for sim, entry in scored[:top_k] if sim >= threshold]

    def prune(self, max_entries: int = 1000):
        """Remove entries beyond max_entries, keeping the highest-scoring ones.

        Sorting by average score (overall + reasonableness + satisfaction / 3).
        """
        if len(self.entries) <= max_entries:
            return

        def avg_score(entry: ExperienceEntry) -> float:
            s = entry.scores
            vals = [
                s.get("overall", 0),
                s.get("reasonableness", 0),
                s.get("executability", 0),
                s.get("satisfaction", 0),
            ]
            return sum(v for v in vals if isinstance(v, (int, float))) / max(len(vals), 1)

        self.entries.sort(key=avg_score, reverse=True)
        self.entries = self.entries[:max_entries]
        self._persist()
