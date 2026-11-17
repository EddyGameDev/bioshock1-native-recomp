# Docs intelligence website (SQLite + local SPA)

All 26 docs are now indexed into a real SQL database and visualized by a
dependency-free local website. It gives us search, extracted facts, a
verification dashboard, a worklog timeline and a document cross-reference graph
instead of plain text files.

## 1. Components

| File | Purpose |
|---|---|
| `tools/docs_indexer.py` | Parses every `docs/*.md` into SQLite: title, category, word/line counts, rendered HTML, headings, code blocks, links, facts, worklog sessions, verification checks |
| `scripts/verify-all.py` | Writes `web/verify_report.json` (machine-readable check results) consumed by the indexer |
| `web/docs.db` | The SQLite database (304 KB) — tables: `documents`, `headings`, `code_blocks`, `links`, `facts`, `checks`, `sessions` |
| `web/docs.sql` | Portable SQL dump (253 KB) of the entire database |
| `web/server.py` | Python stdlib HTTP server + JSON API (no pip packages) |
| `web/index.html`, `web/app.js`, `web/style.css` | Single-page dashboard (vanilla JS/CSS, no CDN) |
| `scripts/serve-docs.ps1` | One-command refresh + launch |

## 2. Run it

```powershell
.\scripts\serve-docs.ps1          # then open http://127.0.0.1:8765
# or custom port:
.\scripts\serve-docs.ps1 -Port 9000
```

The launcher script:
1. runs `scripts/verify-all.py` (refreshing `web/verify_report.json`),
2. runs `tools/docs_indexer.py` (rebuilding `web/docs.db` and `web/docs.sql`),
3. starts `web/server.py`.

## 3. What you can see

| Tab | Content |
|---|---|
| **Overview** | KPI cards (documents, words, headings, code blocks, facts, checks), horizontal bar chart of words per topic, donut chart of documents per category |
| **Documents** | Sidebar grouped by topic; click any doc to read rendered Markdown with syntax-styled code blocks and tables |
| **Facts** | 76 extracted facts grouped by kind (title ID, XDK build, import counts, device model, etc.), with source docs |
| **Checks** | Latest `verify-all.py` report: PASS/FAIL per check with detail |
| **Timeline** | Worklog sessions 1–4 rendered as a vertical timeline |
| **Graph** | SVG force-free circular layout of all 26 docs and 98 cross-reference edges |

Search (top bar) queries titles, summaries and rendered HTML across all docs,
with ranked results; clicking a result opens that doc.

## 4. JSON API

| Endpoint | Returns |
|---|---|
| `/api/summary` | counts + per-category totals |
| `/api/docs` | document list |
| `/api/doc?id=N` | one document including rendered HTML |
| `/api/search?q=…` | ranked search results |
| `/api/facts` | all extracted facts |
| `/api/checks` | verification checks |
| `/api/sessions` | worklog sessions |
| `/api/graph` | `{nodes, edges}` for the doc graph |

## 5. Design decisions

* **SQLite, not a service** — zero setup, portable, inspectable with any SQL
  client; the SQL dump makes the whole index reviewable as text.
* **No external JS/CSS** — works offline; no CDN outages or npm dependency.
* **Generated from docs** — the database is a derived artefact; never edit it
  by hand. Re-run the indexer after any documentation change.
* **Facts are regex-extracted from real doc content**, not hardcoded, so they
  stay honest when the project facts change.
* **Cross-references** include both Markdown links and filename mentions in
  backticks (our docs mostly use the latter).

## 6. Verification

* `python tools/docs_indexer.py` → "indexed 26 documents", DB + SQL dump written.
* API smoke test: summary reports 26 docs, 20,422 words, 76 facts, 12/12 checks,
  5 sessions; graph returns 26 nodes / 98 edges; search for `PM4` returns 11 hits.
* `node --check web/app.js` passes; static files serve HTTP 200.

Browser automation was unavailable in this environment (`patchright` not
installed), so the frontend was verified statically and over HTTP rather than
visually. If you run the site and see a rendering issue, report it and I will
fix it.
