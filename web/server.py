#!/usr/bin/env python3
"""Local docs-intelligence web server (standard library only).

Serves the SPA in web/ and a JSON API backed by web/docs.db.

    python web/server.py            # http://127.0.0.1:8765
    python web/server.py 9000       # custom port
"""

from __future__ import annotations

import json
import sqlite3
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent
DB = ROOT / "docs.db"


def query(sql: str, params: tuple = ()) -> list[dict]:
    con = sqlite3.connect(DB)
    con.row_factory = sqlite3.Row
    try:
        return [dict(r) for r in con.execute(sql, params).fetchall()]
    finally:
        con.close()


def one(sql: str, params: tuple = ()):
    rows = query(sql, params)
    return rows[0] if rows else None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args) -> None:  # quieter logs
        if self.path.startswith("/api/"):
            return
        super().log_message(fmt, *args)

    def _json(self, payload, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, name: str) -> None:
        path = ROOT / name
        if not path.is_file():
            self.send_error(404)
            return
        body = path.read_bytes()
        ctype = {"html": "text/html", "js": "text/javascript",
                 "css": "text/css"}.get(path.suffix.lstrip("."), "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        q = parse_qs(parsed.query)

        if not path.startswith("/api/"):
            self._file("index.html" if path in ("/", "") else path.lstrip("/"))
            return

        if not DB.is_file():
            self._json({"error": "docs.db missing — run python tools/docs_indexer.py"}, 500)
            return

        if path == "/api/summary":
            self._json({
                "documents": one("SELECT COUNT(*) n FROM documents")["n"],
                "words": one("SELECT COALESCE(SUM(words),0) n FROM documents")["n"],
                "headings": one("SELECT COUNT(*) n FROM headings")["n"],
                "code_blocks": one("SELECT COUNT(*) n FROM code_blocks")["n"],
                "links": one("SELECT COUNT(*) n FROM links")["n"],
                "facts": one("SELECT COUNT(*) n FROM facts")["n"],
                "sessions": one("SELECT COUNT(*) n FROM sessions")["n"],
                "checks_pass": one("SELECT COUNT(*) n FROM checks WHERE passed=1")["n"],
                "checks_total": one("SELECT COUNT(*) n FROM checks")["n"],
                "categories": query(
                    "SELECT category, COUNT(*) n, SUM(words) words FROM documents "
                    "GROUP BY category ORDER BY words DESC"),
            })
        elif path == "/api/docs":
            self._json(query("SELECT id,slug,title,category,words,lines,summary "
                             "FROM documents ORDER BY slug"))
        elif path == "/api/doc":
            doc_id = q.get("id", [""])[0]
            self._json(one("SELECT * FROM documents WHERE id=?", (doc_id,)) or {})
        elif path == "/api/search":
            term = q.get("q", [""])[0].strip()
            if len(term) < 2:
                self._json([])
                return
            like = f"%{term}%"
            rows = query(
                "SELECT d.id,d.slug,d.title,d.category,d.summary, "
                "CASE WHEN d.title LIKE ? THEN 5 WHEN d.summary LIKE ? THEN 3 "
                "WHEN d.html LIKE ? THEN 2 ELSE 1 END score "
                "FROM documents d WHERE d.title LIKE ? OR d.summary LIKE ? "
                "OR d.html LIKE ? ORDER BY score DESC, d.slug LIMIT 30",
                (like, like, like, like, like, like))
            self._json(rows)
        elif path == "/api/facts":
            self._json(query(
                "SELECT f.kind,f.value,d.slug,d.title FROM facts f "
                "JOIN documents d ON d.id=f.doc_id ORDER BY f.kind,d.slug"))
        elif path == "/api/checks":
            self._json(query("SELECT name,passed,detail FROM checks ORDER BY id"))
        elif path == "/api/sessions":
            self._json(query("SELECT title,date,body FROM sessions ORDER BY id"))
        elif path == "/api/graph":
            docs = query("SELECT id,slug,title,category FROM documents")
            slugs = {d["slug"]: d for d in docs}
            edges = []
            for row in query("SELECT doc_id,target FROM links"):
                slug = row["target"].split("/")[-1].removesuffix(".md")
                if slug in slugs:
                    edges.append({"source": row["doc_id"], "target": slugs[slug]["id"]})
            self._json({"nodes": docs, "edges": edges})
        else:
            self._json({"error": "unknown endpoint"}, 404)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"BioShock docs intelligence: http://127.0.0.1:{port}")
    print(f"SQLite database: {DB}")
    server.serve_forever()
