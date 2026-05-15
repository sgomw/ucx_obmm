#!/usr/bin/env python3
"""Query the local Chroma SQLite store with FTS and repo-specific path bias."""

from __future__ import annotations

import argparse
import json
import re
import sqlite3
import sys
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[3]
DB_PATH = REPO_ROOT / ".artifacts" / "chromadb" / "chroma.sqlite3"
COLLECTIONS = ("ucx_code", "obmm_code", "ompi_code")

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


def existing_collections(con: sqlite3.Connection) -> set[str]:
    return {
        row["name"]
        for row in con.execute("select name from collections")
    }


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
    if not DB_PATH.exists():
        print(f"Chroma DB not found: {DB_PATH}", file=sys.stderr)
        return 1

    con = sqlite3.connect(DB_PATH)
    con.row_factory = sqlite3.Row

    collections = args.collections or list(COLLECTIONS)
    present = existing_collections(con)
    missing = [name for name in collections if name not in present]
    if missing:
        print(
            "Missing Chroma collection(s): "
            + ", ".join(missing)
            + ". Rebuild with "
            + r"python .\.github\skills\vector-db-retrieval\rebuild_chroma.py",
            file=sys.stderr,
        )
        return 2

    candidate_limit = max(args.k * args.candidate_multiplier, args.k)
    payload = []

    for collection in collections:
        candidates = fetch_candidates(con, collection, args.query, candidate_limit)
        ranked = rerank(candidates, args.query, args.path_hint)[: args.k]
        payload.append(
            {
                "collection": collection,
                "query": args.query,
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
