const state = { tab: 'overview', docs: [], selected: null, summary: null };

async function api(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(path + ' -> ' + r.status);
  return r.json();
}

function fmt(n) { return Number(n || 0).toLocaleString(); }
function esc(s) { return String(s || '').replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

async function init() {
  state.summary = await api('/api/summary');
  state.docs = await api('/api/docs');
  renderSidebar();
  renderTab();
}

function renderSidebar() {
  const body = document.getElementById('sidebar-body');
  if (state.tab === 'documents') {
    const groups = {};
    state.docs.forEach(d => (groups[d.category] ||= []).push(d));
    body.innerHTML = Object.entries(groups).map(([cat, docs]) =>
      `<div class="cat-title">${esc(cat)}</div>` +
      docs.map(d => `<button class="doc-item" data-id="${d.id}">
        <b>${esc(d.title)}</b><small>${fmt(d.words)} words · ${d.slug}.md</small></button>`).join('')
    ).join('');
    body.querySelectorAll('.doc-item').forEach(b => b.onclick = async () => {
      state.selected = b.dataset.id;
      body.querySelectorAll('.doc-item').forEach(x => x.style.background = '');
      b.style.background = 'var(--panel2)';
      const doc = await api('/api/doc?id=' + state.selected);
      document.getElementById('content').innerHTML =
        `<article class="reader">${doc.html}</article>`;
    });
  } else {
    body.innerHTML = `<div class="cat-title">Database</div>
      <p style="color:var(--muted);font-size:12px;margin:0">
      ${fmt(state.summary.documents)} documents · ${fmt(state.summary.words)} words<br>
      ${fmt(state.summary.headings)} headings · ${fmt(state.summary.code_blocks)} code blocks<br>
      ${fmt(state.summary.links)} links · ${fmt(state.summary.facts)} facts<br>
      ${fmt(state.summary.sessions)} worklog sessions</p>`;
  }
}

async function renderTab() {
  const c = document.getElementById('content');
  c.innerHTML = '<div class="loading">Loading…</div>';
  switch (state.tab) {
    case 'overview': return renderOverview(c);
    case 'documents':
      if (state.selected) {
        const doc = await api('/api/doc?id=' + state.selected);
        c.innerHTML = `<article class="reader">${doc.html}</article>`;
      } else {
        c.innerHTML = `<div class="card"><h2>Document browser</h2>
          <p style="color:var(--muted)">Select a document on the left, or use the search box above
          to query every heading, code block and fact in the SQLite index.</p></div>`;
      }
      return;
    case 'facts': return renderFacts(c);
    case 'checks': return renderChecks(c);
    case 'timeline': return renderTimeline(c);
    case 'graph': return renderGraph(c);
  }
}

async function renderOverview(c) {
  const s = state.summary;
  const kpis = [
    ['Documents', s.documents], ['Words indexed', s.words],
    ['Headings', s.headings], ['Code blocks', s.code_blocks],
    ['Facts extracted', s.facts],
    ['Checks', `${s.checks_pass}/${s.checks_total}`],
  ];
  const max = Math.max(...s.categories.map(x => x.words), 1);
  const palette = ['#4da3ff','#3ddc97','#ffd166','#c792ea','#ff8f70','#80cbc4','#f48fb1'];
  const bars = s.categories.map((x,i) =>
    `<div class="bar-row"><span>${esc(x.category)}</span>
      <div class="bar-track"><div class="bar-fill" style="width:${(x.words/max*100).toFixed(1)}%"></div></div>
      <span>${fmt(x.words)}</span></div>`).join('');
  const donutStops = s.categories.map((x,i) => {
    const start = s.categories.slice(0,i).reduce((a,y) => a + y.n, 0) / s.documents * 360;
    const end = (s.categories.slice(0,i).reduce((a,y) => a + y.n, 0) + x.n) / s.documents * 360;
    return `${palette[i%palette.length]} ${start}deg ${end}deg`;
  }).join(',');
  const legend = s.categories.map((x,i) =>
    `<div><i style="background:${palette[i%palette.length]}"></i>${esc(x.category)} (${x.n})</div>`).join('');
  c.innerHTML = `<div class="kpis">${kpis.map(([l,v]) =>
      `<div class="kpi"><div class="v">${fmt(v)}</div><div class="l">${l}</div></div>`).join('')}</div>
    <div class="card"><h2>Words by topic</h2>${bars}</div>
    <div class="card"><h2>Documents by category</h2>
      <div class="donut-wrap"><div class="donut" style="background:conic-gradient(${donutStops})"></div>
      <div class="legend">${legend}</div></div></div>
    <div class="card"><h2>Verified project state</h2>
      <p style="color:var(--muted)">${s.checks_pass} of ${s.checks_total} automated checks pass.
      The game is not playable yet: linking succeeded, HLE/runtime work is in progress.</p></div>`;
}

async function renderFacts(c) {
  const facts = await api('/api/facts');
  const groups = {};
  facts.forEach(f => (groups[f.kind] ||= []).push(f));
  c.innerHTML = `<div class="card"><h2>Facts extracted from every document</h2>
    <div class="fact-grid">${Object.entries(groups).map(([kind, rows]) =>
      `<div class="fact"><b>${esc(kind)}</b>
       <small>${rows.length} docs: ${rows.slice(0,4).map(r => esc(r.slug)).join(', ')}${rows.length>4?'…':''}</small></div>`
    ).join('')}</div></div>`;
}

async function renderChecks(c) {
  const checks = await api('/api/checks');
  c.innerHTML = `<div class="card"><h2>scripts/verify-all.py — latest report</h2>
    <table><thead><tr><th></th><th>Check</th><th>Detail</th></tr></thead><tbody>
    ${checks.map(x => `<tr><td class="${x.passed?'pass':'fail'}">${x.passed?'PASS':'FAIL'}</td>
      <td>${esc(x.name)}</td><td style="color:var(--muted)">${esc(x.detail)}</td></tr>`).join('')}
    </tbody></table></div>`;
}

async function renderTimeline(c) {
  const sessions = await api('/api/sessions');
  c.innerHTML = `<div class="card"><h2>Work log timeline</h2><div class="timeline">
    ${sessions.map(s => `<div class="event"><div class="date">${esc(s.date)}</div>
      <h3>${esc(s.title)}</h3><div style="font-size:12px">${s.body}</div></div>`).join('')}
    </div></div>`;
}

async function renderGraph(c) {
  const { nodes, edges } = await api('/api/graph');
  const W = 900, H = 560, cx = W/2, cy = H/2, R = 220;
  const pos = {};
  nodes.forEach((n,i) => {
    const a = (i / nodes.length) * Math.PI * 2 - Math.PI/2;
    pos[n.id] = { x: cx + R*Math.cos(a), y: cy + R*Math.sin(a) };
  });
  const lines = edges.map(e => {
    const a = pos[e.source], b = pos[e.target];
    return a && b ? `<line class="edge" x1="${a.x}" y1="${a.y}" x2="${b.x}" y2="${b.y}"/>` : '';
  }).join('');
  const circles = nodes.map(n => {
    const p = pos[n.id];
    return `<circle cx="${p.x}" cy="${p.y}" r="5" fill="#4da3ff"/>
      <text class="node-label" x="${p.x+8}" y="${p.y+3}">${esc(n.slug)}</text>`;
  }).join('');
  c.innerHTML = `<div class="card"><h2>Document link graph (${nodes.length} nodes, ${edges.length} links)</h2>
    <svg viewBox="0 0 ${W} ${H}">${lines}${circles}</svg></div>`;
}

document.getElementById('tabs').addEventListener('click', e => {
  const btn = e.target.closest('button');
  if (!btn) return;
  state.tab = btn.dataset.tab;
  document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b === btn));
  renderSidebar();
  renderTab();
});

let searchTimer;
document.getElementById('search').addEventListener('input', e => {
  clearTimeout(searchTimer);
  const q = e.target.value.trim();
  searchTimer = setTimeout(async () => {
    const c = document.getElementById('content');
    if (q.length < 2) { renderTab(); return; }
    const results = await api('/api/search?q=' + encodeURIComponent(q));
    c.innerHTML = `<div class="card"><h2>Search results for “${esc(q)}” (${results.length})</h2>
      ${results.map(r => `<div class="result" data-id="${r.id}"><b>${esc(r.title)}</b>
        <span class="pill">${esc(r.category)}</span><div style="color:var(--muted);font-size:12px">${esc(r.summary)}</div></div>`).join('') || '<p style="color:var(--muted)">No matches.</p>'}
      </div>`;
    c.querySelectorAll('.result').forEach(el => el.onclick = async () => {
      state.tab = 'documents'; state.selected = el.dataset.id;
      document.querySelectorAll('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === 'documents'));
      renderSidebar(); renderTab();
    });
  }, 180);
});

init().catch(e => document.getElementById('content').innerHTML =
  '<div class="card">Error: ' + esc(e.message) + '</div>');
