#!/usr/bin/env python3
"""Rebuild the local Chroma DB for UCX, OBMM, and OMPI with Ollama embeddings."""

from __future__ import annotations

import json
import shutil
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import chromadb


REPO_ROOT = Path(__file__).resolve().parents[3]
DB_DIR = REPO_ROOT / ".artifacts" / "chromadb"
OLLAMA_URL = "http://127.0.0.1:11434/api/embed"
OLLAMA_MODEL = "bge-m3:latest"
MAX_CHARS = 1800
OVERLAP_LINES = 6
UPSERT_BATCH_SIZE = 64
EMBED_BATCH_SIZE = 16
SOURCES = (
    ("ucx_code", REPO_ROOT / "ucx"),
    ("obmm_code", REPO_ROOT / "obmm"),
    ("ompi_code", REPO_ROOT / "ompi"),
)
EXCLUDED_PATH_PREFIXES = (
    "ucx/src/uct/obmm/",
)
SKIP_EXTENSIONS = {
    ".7z",
    ".a",
    ".bin",
    ".bmp",
    ".class",
    ".db",
    ".dll",
    ".dylib",
    ".exe",
    ".gif",
    ".gz",
    ".ico",
    ".jar",
    ".jpeg",
    ".jpg",
    ".o",
    ".obj",
    ".pdf",
    ".png",
    ".pyc",
    ".pyd",
    ".pyo",
    ".so",
    ".sqlite",
    ".sqlite3",
    ".tar",
    ".tgz",
    ".xz",
    ".zip",
}


@dataclass
class Chunk:
    chunk_id: str
    document: str
    metadata: dict


class OllamaBatchEmbeddingFunction:
    def __init__(self, url: str, model_name: str, timeout: int = 300) -> None:
        self._url = url
        self._model_name = model_name
        self._timeout = timeout

    def __call__(self, input: list[str]) -> list[list[float]]:
        texts = list(input)
        if not texts:
            return []

        all_embeddings: list[list[float]] = []
        for start in range(0, len(texts), EMBED_BATCH_SIZE):
            batch = texts[start : start + EMBED_BATCH_SIZE]
            payload = json.dumps({"model": self._model_name, "input": batch}).encode()
            request = urllib.request.Request(
                self._url,
                data=payload,
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(request, timeout=self._timeout) as response:
                data = json.loads(response.read().decode("utf-8"))

            embeddings = data.get("embeddings")
            if not embeddings:
                raise RuntimeError(f"ollama returned no embeddings for batch at {start}")

            all_embeddings.extend(embeddings)

        return all_embeddings


def decode_text(path: Path) -> str | None:
    if path.suffix.lower() in SKIP_EXTENSIONS:
        return None

    raw = path.read_bytes()
    if b"\x00" in raw[:8192]:
        return None

    for encoding in ("utf-8", "utf-8-sig"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            pass

    text = raw.decode("latin-1")
    printable = sum(ch.isprintable() or ch in "\r\n\t" for ch in text)
    if text and printable / len(text) < 0.95:
        return None

    return text


def chunk_text(root_name: str, rel_path: str, text: str) -> Iterable[Chunk]:
    lines = text.splitlines()
    if not lines:
        return

    start = 0
    index = 0
    while start < len(lines):
        end = start
        current_chars = 0
        while end < len(lines):
            line = lines[end]
            line_size = len(line) + 1
            if end > start and current_chars + line_size > MAX_CHARS:
                break
            current_chars += line_size
            end += 1

        if end == start:
            end += 1

        start_line = start + 1
        end_line = end
        body = "\n".join(lines[start:end])
        document = (
            f"path: {rel_path}\n"
            f"root: {root_name}\n"
            f"lines: {start_line}-{end_line}\n\n"
            f"{body}"
        )
        chunk_id = f"{root_name}:{rel_path}:{start_line}-{end_line}:{index}"
        metadata = {
            "path": rel_path,
            "root": root_name,
            "start_line": start_line,
            "end_line": end_line,
        }
        yield Chunk(chunk_id=chunk_id, document=document, metadata=metadata)

        if end >= len(lines):
            break

        start = max(start + 1, end - OVERLAP_LINES)
        index += 1


def batched(items: list[Chunk], size: int) -> Iterable[list[Chunk]]:
    for start in range(0, len(items), size):
        yield items[start : start + size]


def is_excluded(rel_path: str) -> bool:
    return any(rel_path.startswith(prefix) for prefix in EXCLUDED_PATH_PREFIXES)


def build_collection(
    client: chromadb.ClientAPI, name: str, root: Path
) -> tuple[int, int, int]:
    if not root.exists():
        print(f"[{name}] source root missing: {root}", file=sys.stderr)
        collection = client.get_or_create_collection(
            name=name,
            metadata={"hnsw:space": "cosine"},
            embedding_function=OllamaBatchEmbeddingFunction(
                url=OLLAMA_URL, model_name=OLLAMA_MODEL
            ),
        )
        return 0, 0, collection.count()

    embedding_function = OllamaBatchEmbeddingFunction(
        url=OLLAMA_URL, model_name=OLLAMA_MODEL
    )
    collection = client.get_or_create_collection(
        name=name,
        metadata={"hnsw:space": "cosine"},
        embedding_function=embedding_function,
    )

    chunks: list[Chunk] = []
    file_count = 0
    skipped_files = 0

    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue

        rel_path = str(path.relative_to(REPO_ROOT)).replace("\\", "/")
        if is_excluded(rel_path):
            skipped_files += 1
            continue

        text = decode_text(path)
        if text is None:
            skipped_files += 1
            continue

        file_count += 1
        chunks.extend(chunk_text(root.name, rel_path, text))

    print(f"[{name}] files={file_count} skipped={skipped_files} chunks={len(chunks)}")

    for batch_index, batch in enumerate(batched(chunks, UPSERT_BATCH_SIZE), start=1):
        collection.upsert(
            ids=[chunk.chunk_id for chunk in batch],
            documents=[chunk.document for chunk in batch],
            metadatas=[chunk.metadata for chunk in batch],
        )
        if batch_index % 25 == 0:
            current = min(batch_index * UPSERT_BATCH_SIZE, len(chunks))
            print(f"[{name}] upserted {current}/{len(chunks)}")

    return file_count, skipped_files, len(chunks)


def main() -> None:
    if DB_DIR.exists():
        shutil.rmtree(DB_DIR)
    DB_DIR.mkdir(parents=True, exist_ok=True)

    client = chromadb.PersistentClient(path=str(DB_DIR))
    summaries = {}
    for name, root in SOURCES:
        summaries[name] = build_collection(client, name, root)

    print("\nrebuild complete")
    for name, (files_indexed, skipped_files, chunk_count) in summaries.items():
        collection = client.get_collection(name)
        print(
            f"{name}: files_indexed={files_indexed}, skipped_files={skipped_files}, "
            f"chunks={chunk_count}, stored={collection.count()}"
        )


if __name__ == "__main__":
    main()
