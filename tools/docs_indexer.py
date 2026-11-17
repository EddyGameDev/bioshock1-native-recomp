#!/usr/bin/env python3
"""Index every docs/*.md file into a SQLite database for the docs website.

Creates web/docs.db and web/docs.sql (portable dump). No third-party deps.
The database stores structure (headings, code blocks, links), extracted facts,
verification checks and worklog sessions — so the website can show more than
plain text: search, charts, a link graph and a timeline.

Usage:
    python tools/docs_indexer.py
"""

from __future__ import annotations

import html
import json
import re
import sqlite3
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DOCS = PROJECT / "docs"
WEB = PROJECT / "web"
DB = WEB / "docs.db"
DUMP = WEB / "docs.sql"
REPORT = WEB / "verify_report.json"

CATEGORY_NAMES = {
    "00": "START", "01": "Architecture", "02": "Toolchain", "03": "Analysis",
    "04": "Pipeline", "05": "Build", "06": "HLE", "07": "GPU", "08": "Launcher",
    "09": "Reverse engineering", "10": "Status", "11": "Roadmap",
    "12": "Troubleshooting", "13": "Legal", "14": "Glossary", "15": "References",
    "16": "BIOS/XDK", "17": "Testing", "18": "Manifest", "19": "Work log",
    "20": "Toolchain v0.11", "21": "Import matrix", "22": "Link milestone",
    "23": "Verification", "24": "Device test", "25": "Phase 3 renderer",
    "26": "Docs website",
}

FACT_PATTERNS = [
    ("title_id", r"0x545407D8"),
    ("xdk_build", r"\b5426\b"),
    ("image_base", r"0x82000000"),
    ("import_records", r"\b427\b"),
    ("hle_imports", r"\b216\b"),
    ("runtime_symbols", r"\b245\b"),
    ("generated_files", r"\b308\b"),
    ("functions", r"36,?625"),
    ("linked_so_mb", r"139\.?\d*\s*MB"),
    ("apk_mb", r"9\.\d\s*MB"),
    ("guest_entry", r"0x8219D698"),
    ("device_model", r"SM-A366B"),
    ("checks_pass", r"12/12"),
]

SCHEMA = """
DROP TABLE IF EXISTS documents;
DROP TABLE IF EXISTS headings;
DROP TABLE IF EXISTS code_blocks;
DROP TABLE IF EXISTS links;
DROP TABLE IF EXISTS facts;
DROP TABLE IF EXISTS checks;
DROP TABLE IF EXISTS sessions;

CREATE TABLE documents (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  slug TEXT UNIQUE NOT NULL,
  title TEXT NOT NULL,
  category TEXT NOT NULL,
  path TEXT NOT NULL,
  words INTEGER NOT NULL,
  lines INTEGER NOT NULL,
  summary TEXT NOT NULL,
  html TEXT NOT NULL
);
CREATE TABLE headings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  doc_id INTEGER NOT NULL REFERENCES documents(id),
  level INTEGER NOT NULL,
  text TEXT NOT NULL
);
CREATE TABLE code_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  doc_id INTEGER NOT NULL REFERENCES documents(id),
  lang TEXT NOT NULL,
  code TEXT NOT NULL
);
CREATE TABLE links (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  doc_id INTEGER NOT NULL REFERENCES documents(id),
  target TEXT NOT NULL,
  label TEXT NOT NULL
);
CREATE TABLE facts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  doc_id INTEGER NOT NULL REFERENCES documents(id),
  kind TEXT NOT NULL,
  value TEXT NOT NULL
);
CREATE TABLE checks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  passed INTEGER NOT NULL,
  detail TEXT NOT NULL
);
CREATE TABLE sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  title TEXT NOT NULL,
  date TEXT NOT NULL,
  body TEXT NOT NULL
);
"""


def inline_md(text: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
                     r'<a href="\2">\1</a>', escaped)
    return escaped


def render_markdown(src: str) -> str:
    """Small dependency-free Markdown renderer covering what our docs use."""
    out: list[str] = []
    lines = src.splitlines()
    i = 0
    in_code = False
    code_lang = ""
    code_buf: list[str] = []
    in_table = False
    table_rows: list[list[str]] = []

    def flush_table() -> None:
        nonlocal in_table, table_rows
        if not table_rows:
            return
        out.append("<table><thead><tr>" +
                   "".join(f"<th>{inline_md(c)}</th>" for c in table_rows[0]) +
                   "</tr></thead><tbody>")
        for row in table_rows[2:]:
            out.append("<tr>" + "".join(f"<td>{inline_md(c)}</td>" for c in row) + "</tr>")
        out.append("</tbody></table>")
        in_table = False
        table_rows = []

    while i < len(lines):
        line = lines[i]
        if line.startswith("```"):
            if in_code:
                out.append(f'<pre><code class="lang-{html.escape(code_lang)}">' +
                           html.escape("\n".join(code_buf)) + "</code></pre>")
                in_code = False
                code_buf = []
            else:
                flush_table()
                in_code = True
                code_lang = line[3:].strip().lower()
            i += 1
            continue
        if in_code:
            code_buf.append(line)
            i += 1
            continue

        if "|" in line and line.strip().startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not in_table:
                in_table = True
                table_rows = []
            table_rows.append(cells)
            i += 1
            continue
        flush_table()

        m = re.match(r"^(#{1,6})\s+(.*)$", line)
        if m:
            level = len(m.group(1))
            out.append(f"<h{level}>{inline_md(m.group(2))}</h{level}>")
            i += 1
            continue
        if line.startswith("> "):
            out.append(f"<blockquote>{inline_md(line[2:])}</blockquote>")
            i += 1
            continue
        if re.match(r"^\s*[-*]\s+", line):
            items = []
            while i < len(lines) and re.match(r"^\s*[-*]\s+", lines[i]):
                items.append(re.sub(r"^\s*[-*]\s+", "", lines[i]))
                i += 1
            out.append("<ul>" + "".join(f"<li>{inline_md(x)}</li>" for x in items) + "</ul>")
            continue
        if not line.strip():
            i += 1
            continue
        para = [line]
        i += 1
        while i < len(lines) and lines[i].strip() and not lines[i].startswith(("#", "```", "> ", "|")):
            para.append(lines[i])
            i += 1
        out.append(f"<p>{inline_md(' '.join(para))}</p>")
    flush_table()
    return "\n".join(out)


def main() -> int:
    WEB.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(DB)
    con.executescript(SCHEMA)

    for path in sorted(DOCS.glob("*.md")):
        src = path.read_text(encoding="utf-8")
        slug = path.stem
        title_match = re.search(r"^#\s+(.+)$", src, re.MULTILINE)
        title = title_match.group(1).strip() if title_match else slug
        category = CATEGORY_NAMES.get(slug[:2], "Other")
        words = len(src.split())
        lines = len(src.splitlines())
        first_para = ""
        for line in src.splitlines():
            if line.startswith("#") or not line.strip() or line.startswith("```"):
                continue
            first_para = re.sub(r"[*`\[\]]", "", line).strip()
            if first_para:
                break

        cur = con.execute(
            "INSERT INTO documents(slug,title,category,path,words,lines,summary,html) "
            "VALUES(?,?,?,?,?,?,?,?)",
            (slug, title, category, str(path.relative_to(PROJECT)), words, lines,
             first_para[:300], render_markdown(src)))
        doc_id = cur.lastrowid

        for line in src.splitlines():
            m = re.match(r"^(#{1,6})\s+(.*)$", line)
            if m:
                con.execute("INSERT INTO headings(doc_id,level,text) VALUES(?,?,?)",
                            (doc_id, len(m.group(1)), m.group(2).strip()))

        for block in re.findall(r"```(\w*)\n(.*?)```", src, re.DOTALL):
            con.execute("INSERT INTO code_blocks(doc_id,lang,code) VALUES(?,?,?)",
                        (doc_id, block[0] or "text", block[1][:8000]))

        for label, target in re.findall(r"\[([^\]]+)\]\(([^)]+)\)", src):
            con.execute("INSERT INTO links(doc_id,target,label) VALUES(?,?,?)",
                        (doc_id, target, label))

        # Docs mostly cross-reference each other by filename in backticks
        # rather than with Markdown links. Treat any mentioned doc slug as a
        # graph edge so the visualization reflects real relationships.
        for other in DOCS.glob("*.md"):
            if other.stem == slug:
                continue
            if other.stem in src or other.name in src:
                con.execute("INSERT INTO links(doc_id,target,label) VALUES(?,?,?)",
                            (doc_id, other.name, other.stem))

        for kind, pattern in FACT_PATTERNS:
            if re.search(pattern, src, re.IGNORECASE):
                con.execute("INSERT INTO facts(doc_id,kind,value) VALUES(?,?,?)",
                            (doc_id, kind, pattern))

    if REPORT.is_file():
        report = json.loads(REPORT.read_text(encoding="utf-8"))
        for c in report["checks"]:
            con.execute("INSERT INTO checks(name,passed,detail) VALUES(?,?,?)",
                        (c["name"], int(c["passed"]), c["detail"]))

    # Worklog sessions from 19-WORKLOG.md.
    worklog = DOCS / "19-WORKLOG.md"
    if worklog.is_file():
        text = worklog.read_text(encoding="utf-8")
        sessions = re.split(r"(?m)^## Session ", text)[1:]
        for session in sessions:
            head, _, body = session.partition("\n")
            date_match = re.search(r"(\d{4}-\d{2}-\d{2})", head)
            con.execute("INSERT INTO sessions(title,date,body) VALUES(?,?,?)",
                        ("Session " + head.strip(),
                         date_match.group(1) if date_match else "",
                         render_markdown(body)[:6000]))

    con.commit()

    with DUMP.open("w", encoding="utf-8") as fh:
        for line in con.iterdump():
            fh.write(line + "\n")
    con.close()

    docs = len(list(DOCS.glob("*.md")))
    print(f"indexed {docs} documents into {DB.relative_to(PROJECT)}")
    print(f"SQL dump: {DUMP.relative_to(PROJECT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
