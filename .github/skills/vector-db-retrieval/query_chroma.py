#!/usr/bin/env python3
"""Query the local Chroma SQLite store with FTS and repo-specific path bias."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_DB_PATH = REPO_ROOT / ".artifacts" / "chromadb" / "chroma.sqlite3"
COLLECTIONS = ("ucx_code", "obmm_code", "ompi_code")
SOURCE_ROOTS = {
    "ucx_code": ("ucx",),
    "obmm_code": ("obmm",),
    "ompi_code": ("ompi",),
}
SOURCE_EXCLUDES = {
    "ucx_code": ("ucx/src/uct/obmm/",),
}
TEXT_SUFFIXES = {
    "",
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
    ".m4",
    ".am",
    ".mk",
    ".md",
    ".txt",
    ".py",
    ".sh",
}
MAX_SOURCE_FILE_BYTES = 1024 * 1024
MAX_RG_OUTPUT_LINES = 4000
SNIPPET_BEFORE = 4
SNIPPET_AFTER = 16

PATH_BIAS_RULES = {
    "ucx_code": [
        ("ucx/src/uct/", -4.0),
        ("ucx/src/ucp/", -3.5),
        ("ucx/src/", -2.5),
        ("ucx/test/", 0.8),
        ("ucx/docs/", 1.0),
        ("ucx/.", 3.0),
    ],
    "obmm_code": [
        ("obmm/src/libobmm/", -3.0),
        ("obmm/doc/", -2.5),
        ("obmm/README", 1.5),
        ("obmm/License", 2.0),
    ],
    "ompi_code": [
        ("ompi/ompi/", -3.0),
        ("ompi/opal/", -2.5),
        ("ompi/oshmem/", -2.5),
        ("ompi/docs/", 0.8),
        ("ompi/test/", 1.0),
        ("ompi/config/", 2.0),
        ("ompi/3rd-party/", 2.5),
        ("ompi/.ci/", 3.0),
        ("ompi/.github/", 3.0),
    ],
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query the local Chroma DB without relying on a mismatched embedding model."
    )
    parser.add_argument("query", help="Symbol, API, concept, or phrase to retrieve.")
    parser.add_argument(
        "--collection",
        action="append",
        choices=COLLECTIONS,
        dest="collections",
        help="Collection(s) to search. Defaults to all collections.",
    )
    parser.add_argument(
        "--k",
        type=int,
        default=8,
        help="Number of final ranked hits to return per collection.",
    )
    parser.add_argument(
        "--candidate-multiplier",
        type=int,
        default=4,
        help="Fetch this many multiples of k before reranking.",
    )
    parser.add_argument(
        "--path-hint",
        action="append",
        default=[],
        help="Prefer paths containing this substring. Repeatable.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit machine-readable JSON.",
    )
    parser.add_argument(
        "--db-path",
        type=Path,
        default=None,
        help="Override Chroma sqlite path. Defaults to .artifacts/chromadb/chroma.sqlite3.",
    )
    parser.add_argument(
        "--no-source-fallback",
        action="store_true",
        help="Fail instead of using source text retrieval when Chroma sqlite is missing.",
    )
    return parser.parse_args()


def tokenize(query: str) -> list[str]:
    return re.findall(r"[A-Za-z_][A-Za-z0-9_]*|\d+", query)


def build_match_expressions(query: str) -> list[str]:
    raw = query.strip()
    if not raw:
        return []

    expressions: list[str] = []
    escaped = raw.replace('"', '""')

    if len(raw.split()) == 1 or "_" in raw or "/" in raw:
        expressions.append(f'"{escaped}"')

    tokens = tokenize(raw)
    if tokens:
        expressions.append(" AND ".join(f'"{token.replace(chr(34), chr(34) * 2)}"' for token in tokens))
        if len(tokens) > 1:
            expressions.append(" OR ".join(f'"{token.replace(chr(34), chr(34) * 2)}"' for token in tokens))

    if f'"{escaped}"' not in expressions:
        expressions.append(f'"{escaped}"')

    deduped: list[str] = []
    for expr in expressions:
        if expr not in deduped:
            deduped.append(expr)
    return deduped


def clean_snippet(document: str, limit: int = 220) -> str:
    lines = document.splitlines()
    if lines[:3] and lines[0].startswith("path: ") and any(line.startswith("root: ") for line in lines[:4]):
        body_start = 0
        blank_seen = False
        for i, line in enumerate(lines):
            if line.strip():
                if blank_seen:
                    body_start = i
                    break
            else:
                blank_seen = True
        body = "\n".join(lines[body_start:]).strip()
    else:
        body = document.strip()
    body = re.sub(r"\s+", " ", body)
    return body[:limit]


def path_bias(collection: str, path: str) -> float:
    for prefix, bias in PATH_BIAS_RULES.get(collection, []):
        if path.startswith(prefix):
            return bias
    return 0.0


def hint_bias(path: str, hints: Iterable[str]) -> float:
    path_lower = path.lower()
    bias = 0.0
    for hint in hints:
        if hint.lower() in path_lower:
            bias -= 1.5
    return bias


def exact_bias(query: str, path: str, document: str) -> float:
    lowered = query.lower()
    bias = 0.0
    if lowered in path.lower():
        bias -= 1.0
    if lowered in document.lower():
        bias -= 1.5
    return bias


def normalize_rel_path(path) -> str:
    path_obj = Path(path)
    if path_obj.is_absolute():
        try:
            path_obj = path_obj.resolve().relative_to(REPO_ROOT)
        except ValueError:
            pass
    return path_obj.as_posix()


def resolve_db_path(override: Optional[Path]) -> Optional[Path]:
    env_path = os.environ.get("UCX_CHROMA_DB_PATH") or os.environ.get("CHROMA_DB_PATH")
    candidates: list[Path] = []

    if override is not None:
        candidates.append(override)
    if env_path:
        candidates.append(Path(env_path))
    candidates.append(DEFAULT_DB_PATH)

    artifacts = REPO_ROOT / ".artifacts"
    if artifacts.exists():
        candidates.extend(artifacts.glob("**/chroma.sqlite3"))

    seen: set[Path] = set()
    for candidate in candidates:
        candidate = candidate if candidate.is_absolute() else REPO_ROOT / candidate
        candidate = candidate.resolve()
        if candidate in seen:
            continue
        seen.add(candidate)
        if candidate.exists():
            return candidate

    return None


def source_path_excluded(collection: str, rel_path: str) -> bool:
    rel_path = rel_path.replace("\\", "/")
    return any(rel_path.startswith(prefix) for prefix in SOURCE_EXCLUDES.get(collection, ()))


def fallback_patterns(query: str) -> list[tuple[str, float]]:
    raw = query.strip()
    patterns: list[tuple[str, float]] = []

    if raw:
        patterns.append((raw, -2.0))

    for token in tokenize(raw):
        patterns.append((token, -0.4))

    deduped: list[tuple[str, float]] = []
    seen: set[str] = set()
    for pattern, weight in patterns:
        key = pattern.lower()
        if key not in seen:
            seen.add(key)
            deduped.append((pattern, weight))

    return deduped


def read_source_snippet(rel_path: str, line_no: int) -> Optional[Tuple[int, int, str]]:
    path = REPO_ROOT / rel_path
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None

    if not lines:
        return None

    start_line = max(1, line_no - SNIPPET_BEFORE)
    end_line = min(len(lines), line_no + SNIPPET_AFTER)
    return start_line, end_line, "\n".join(lines[start_line - 1:end_line])


def source_hit_score(query: str, pattern_weight: float, line_no: int, line: str) -> float:
    score = pattern_weight + (line_no / 100000.0)
    lowered_query = query.lower()
    lowered_line = line.lower()

    if lowered_query and lowered_query in lowered_line:
        score -= 1.0

    for token in tokenize(query):
        if token.lower() in lowered_line:
            score -= 0.15

    return score


def fetch_source_candidates_rg(
    collection: str,
    query: str,
    limit: int,
) -> Optional[list[dict[str, object]]]:
    rg_path = shutil.which("rg")
    if rg_path is None:
        return None

    roots = [root for root in SOURCE_ROOTS[collection] if (REPO_ROOT / root).exists()]
    if not roots:
        return []

    results: dict[tuple[str, int, int], dict[str, object]] = {}
    max_results = max(limit * 8, limit)

    for pattern, pattern_weight in fallback_patterns(query):
        cmd = [
            rg_path,
            "-n",
            "--no-heading",
            "--color",
            "never",
            "--ignore-case",
            "--fixed-strings",
            "--max-filesize",
            "1M",
            "--max-count",
            "20",
        ]
        for exclude in SOURCE_EXCLUDES.get(collection, ()):
            cmd.extend(["--glob", f"!{exclude}**"])
        cmd.extend(["--", pattern])
        cmd.extend(roots)

        proc = subprocess.run(
            cmd,
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if proc.returncode not in (0, 1):
            return None

        for output_line in proc.stdout.splitlines()[:MAX_RG_OUTPUT_LINES]:
            parts = output_line.split(":", 2)
            if len(parts) != 3:
                continue

            rel_path = normalize_rel_path(parts[0])
            if source_path_excluded(collection, rel_path):
                continue

            try:
                line_no = int(parts[1])
            except ValueError:
                continue

            snippet = read_source_snippet(rel_path, line_no)
            if snippet is None:
                continue

            start_line, end_line, document = snippet
            score = source_hit_score(query, pattern_weight, line_no, parts[2])
            key = (rel_path, start_line, end_line)
            candidate = {
                "collection": collection,
                "path": rel_path,
                "start_line": start_line,
                "end_line": end_line,
                "document": document,
                "bm25": score,
                "match_expression": pattern,
            }
            prev = results.get(key)
            if prev is None or candidate["bm25"] < prev["bm25"]:
                results[key] = candidate

            if len(results) >= max_results:
                break

    return list(results.values())


def iter_source_files(collection: str) -> Iterable[Path]:
    for root in SOURCE_ROOTS[collection]:
        root_path = REPO_ROOT / root
        if not root_path.exists():
            continue

        for path in root_path.rglob("*"):
            if not path.is_file():
                continue

            rel_path = normalize_rel_path(path)
            if source_path_excluded(collection, rel_path):
                continue
            if path.suffix not in TEXT_SUFFIXES:
                continue
            try:
                if path.stat().st_size > MAX_SOURCE_FILE_BYTES:
                    continue
            except OSError:
                continue
            yield path


def fetch_source_candidates_scan(
    collection: str,
    query: str,
    limit: int,
) -> list[dict[str, object]]:
    patterns = [(pattern.lower(), weight) for pattern, weight in fallback_patterns(query)]
    results: dict[tuple[str, int, int], dict[str, object]] = {}
    max_results = max(limit * 8, limit)

    if not patterns:
        return []

    for path in iter_source_files(collection):
        rel_path = normalize_rel_path(path)
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue

        for line_index, line in enumerate(lines, 1):
            lowered_line = line.lower()
            matched = [(pattern, weight) for pattern, weight in patterns if pattern in lowered_line]
            if not matched:
                continue

            start_line = max(1, line_index - SNIPPET_BEFORE)
            end_line = min(len(lines), line_index + SNIPPET_AFTER)
            document = "\n".join(lines[start_line - 1:end_line])
            score = source_hit_score(query, min(weight for _, weight in matched), line_index, line)
            key = (rel_path, start_line, end_line)
            candidate = {
                "collection": collection,
                "path": rel_path,
                "start_line": start_line,
                "end_line": end_line,
                "document": document,
                "bm25": score,
                "match_expression": matched[0][0],
            }
            prev = results.get(key)
            if prev is None or candidate["bm25"] < prev["bm25"]:
                results[key] = candidate

            if len(results) >= max_results:
                return list(results.values())

    return list(results.values())


def fetch_source_candidates(
    collection: str,
    query: str,
    limit: int,
) -> list[dict[str, object]]:
    rg_results = fetch_source_candidates_rg(collection, query, limit)
    if rg_results is not None:
        return rg_results

    return fetch_source_candidates_scan(collection, query, limit)


def fetch_candidates(
    con: sqlite3.Connection,
    collection: str,
    query: str,
    limit: int,
) -> list[dict[str, object]]:
    results: dict[tuple[str, int, int], dict[str, object]] = {}
    expressions = build_match_expressions(query)
    if not expressions:
        return []

    sql = """
        select bm25(embedding_fulltext_search) as bm25_score,
               p.string_value as path,
               s.int_value as start_line,
               e.int_value as end_line,
               d.string_value as document
        from embedding_fulltext_search
        join embeddings emb on emb.id = embedding_fulltext_search.rowid
        join segments seg on seg.id = emb.segment_id and seg.scope = 'METADATA'
        join collections c on c.id = seg.collection
        join embedding_metadata d on d.id = emb.id and d.key = 'chroma:document'
        join embedding_metadata p on p.id = emb.id and p.key = 'path'
        join embedding_metadata s on s.id = emb.id and s.key = 'start_line'
        join embedding_metadata e on e.id = emb.id and e.key = 'end_line'
        where c.name = ? and embedding_fulltext_search match ?
        order by bm25_score
        limit ?
    """

    for expr in expressions:
        for row in con.execute(sql, (collection, expr, limit)):
            key = (row["path"], row["start_line"], row["end_line"])
            candidate = {
                "collection": collection,
                "path": row["path"],
                "start_line": row["start_line"],
                "end_line": row["end_line"],
                "document": row["document"],
                "bm25": float(row["bm25_score"]),
                "match_expression": expr,
            }
            prev = results.get(key)
            if prev is None or candidate["bm25"] < prev["bm25"]:
                results[key] = candidate
    return list(results.values())


def rerank(candidates: list[dict[str, object]], query: str, hints: list[str]) -> list[dict[str, object]]:
    reranked = []
    for item in candidates:
        doc = str(item["document"])
        path = str(item["path"])
        collection = str(item["collection"])
        adjusted = (
            float(item["bm25"])
            + path_bias(collection, path)
            + hint_bias(path, hints)
            + exact_bias(query, path, doc)
        )
        reranked.append(
            {
                **item,
                "adjusted_score": adjusted,
                "snippet": clean_snippet(doc),
            }
        )
    reranked.sort(key=lambda item: (item["adjusted_score"], item["bm25"], item["path"], item["start_line"]))
    return reranked


def render_text(collection: str, query: str, results: list[dict[str, object]]) -> str:
    lines = [f"[{collection}] query={query}"]
    if not results:
        lines.append("(no hits)")
        return "\n".join(lines)

    for index, item in enumerate(results, 1):
        score = item.get("adjusted_score", item.get("score"))
        lines.append(
            f"{index}. {item['path']}:{item['start_line']}-{item['end_line']} "
            f"(score={float(score):.3f}, bm25={float(item['bm25']):.3f})"
        )
        lines.append(f"   {item['snippet']}")
    return "\n".join(lines)


def main() -> int:
    args = parse_args()
    db_path = resolve_db_path(args.db_path)
    con = None

    if db_path is None:
        if args.no_source_fallback:
            print(f"Chroma sqlite DB not found under {REPO_ROOT / '.artifacts'}", file=sys.stderr)
            return 1

        print(
            "Chroma sqlite DB not found; falling back to source text retrieval",
            file=sys.stderr,
        )
    else:
        con = sqlite3.connect(db_path)
        con.row_factory = sqlite3.Row

    collections = args.collections or list(COLLECTIONS)
    candidate_limit = max(args.k * args.candidate_multiplier, args.k)
    payload = []

    for collection in collections:
        if con is None:
            candidates = fetch_source_candidates(collection, args.query, candidate_limit)
            source = "source_fallback"
        else:
            candidates = fetch_candidates(con, collection, args.query, candidate_limit)
            source = "chroma_sqlite_fts"

        ranked = rerank(candidates, args.query, args.path_hint)[: args.k]
        payload.append(
            {
                "collection": collection,
                "query": args.query,
                "source": source,
                "results": [
                    {
                        "path": item["path"],
                        "start_line": item["start_line"],
                        "end_line": item["end_line"],
                        "score": round(float(item["adjusted_score"]), 6),
                        "bm25": round(float(item["bm25"]), 6),
                        "snippet": item["snippet"],
                    }
                    for item in ranked
                ],
            }
        )

    if args.json:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    else:
        print("\n\n".join(render_text(item["collection"], item["query"], item["results"]) for item in payload))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
