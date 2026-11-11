"use strict";
// Residency: which side each system lives on (CPU left, GPU right, things that cross the bridge in the middle) and every
// hand-off that crosses between the sides, drawn as arrows. Backward (GPU -> CPU) trips are red: they are the slow ones.
(function () {
  const root = Arch.$("v-residency");
  let pinned = null, hover = null, hoverEdge = -1, model = null;

  const colOf = (n) => ({ cpu: 0, "cpu->gpu": 1, "cpu+gpu": 1, gpu: 2 }[n.residency] ?? 0);

  function build() {
    const d = Arch.data, cols = [[], [], []];
    for (const n of d.nodes.values()) cols[colOf(n)].push(n);
    // start from layer order, then pull connected cards next to each other
    for (const c of cols) c.sort((a, b) => Arch.LAYER_ORDER.indexOf(a.layer) - Arch.LAYER_ORDER.indexOf(b.layer));
    Arch.orderColumns(cols, (n) => [...d.out.get(n.id).map(e => e.target), ...d.inc.get(n.id).map(e => e.source)]);
    return { cols };
  }

  function stats() {
    const d = Arch.data, N = [...d.nodes.values()], cnt = (r) => N.filter(n => n.residency === r).length;
    let cross = 0, back = 0;
    for (const e of d.edges) {
      const a = colOf(d.nodes.get(e.source)), b = colOf(d.nodes.get(e.target));
      if (a !== b) { cross++; if (a > b) back++; }
    }
    const S = (n, t, c) => `<div class="stat"><b style="color:${c}">${n}</b>${t}</div>`;
    return S(cnt("cpu"), "planner jobs", Arch.RES_COLORS.cpu) + S(cnt("gpu"), "helper jobs", Arch.RES_COLORS.gpu) +
      S(cnt("cpu->gpu"), "prepared by the planner, then sent", Arch.RES_COLORS["cpu->gpu"]) + S(cnt("cpu+gpu"), "kept on both sides", Arch.RES_COLORS["cpu+gpu"]) +
      S(cross, "hand-offs between sides", "#fff") + S(back, "slow trips back to the planner", "#ee6b6b");
  }

  function card(n) {
    const lc = Arch.LAYER_COLORS[Arch.layerOf(n)] || "#888";
    return `<div class="rcard ${n.residency === "cpu+gpu" ? "both" : ""}" data-id="${Arch.esc(n.id)}" style="--lc:${lc}">
      <div class="t">${Arch.esc(Arch.name(n))}</div>
      <div class="m">${Arch.esc(Arch.LAYER_TITLES[Arch.layerOf(n)] || n.layer)}${n.update_rate ? " · " + Arch.esc(n.update_rate) : ""}</div></div>`;
  }

  function render() {
    const d = Arch.data; if (!d) return;
    model = build();
    root.innerHTML = `<div class="vw">
      <h1>Who does the work, and what crosses the bridge</h1>
      <p class="lead">Read it left to right. The <b style="color:#e8b866">planner</b> prepares, the <b style="color:#de86da">bridge</b> carries, the <b style="color:#7bbaff">helpers</b> paint.
        Arrows show data being handed over. <b style="color:#ee6b6b">Red arrows going backwards</b> are the rare, expensive trips. Hover or click any box to follow its connections.</p>
      <div class="stats">${stats()}</div>
      <div class="detail" id="resDetail">Hover or click a box to see what it does and what it exchanges with the rest.</div>
      <div class="board" id="resBoard"><svg id="resSvg"></svg>
        <section class="col cpu"><h2>🧠 Planner — CPU</h2><p class="expl">One clever worker, one job at a time. Decides, schedules, prepares. Notes kept in main memory.</p><div class="cards">${model.cols[0].map(card).join("")}</div></section>
        <div></div>
        <section class="col bridge"><h2>🌉 The bridge</h2><p class="expl">Data the planner prepares and copies across, or that both sides keep a copy of. Every copy costs time.</p><div class="cards">${model.cols[1].map(card).join("")}</div></section>
        <div></div>
        <section class="col gpu"><h2>🎨 Helpers — GPU</h2><p class="expl">Thousands of simple workers colouring at once. Fast at pixels. Notes kept in video memory.</p><div class="cards">${model.cols[2].map(card).join("")}</div></section>
      </div>
      <h3>Everything that crosses between sides</h3><div id="resTable"></div>
    </div>`;
    root.querySelectorAll(".rcard").forEach(el => {
      el.onmouseenter = () => { hover = el.dataset.id; refresh(); };
      el.onmouseleave = () => { hover = null; refresh(); };
      el.onclick = () => { pinned = pinned === el.dataset.id ? null : el.dataset.id; refresh(); };
    });
    table(); refresh();
  }

  function table() {
    const d = Arch.data, rows = [];
    d.edges.forEach((e, i) => {
      const a = d.nodes.get(e.source), b = d.nodes.get(e.target), ca = colOf(a), cb = colOf(b);
      if (ca !== cb) rows.push({ e, i, a, b, back: ca > cb });
    });
    rows.sort((x, y) => (y.back - x.back) || (x.i - y.i));
    Arch.$("resTable").innerHTML = `<table class="tbl"><tr><th>From</th><th></th><th>To</th><th>What travels</th><th>How</th><th>Size</th><th>How often</th></tr>` +
      rows.map(r => `<tr class="row" data-edge="${r.i}"><td>${Arch.esc(Arch.name(r.a))}</td><td class="arrow" style="color:${Arch.edgeColor(r.e)}">${r.back ? "⟵" : "⟶"}</td>
        <td>${Arch.esc(Arch.name(r.b))}</td><td>${Arch.esc(r.e.label || "")}</td><td style="color:${Arch.edgeColor(r.e)}">${Arch.esc(r.e.transport || r.e.kind || "")}</td>
        <td>${Arch.esc(r.e.size || "")}</td><td>${Arch.esc(r.e.update_rate || "")}</td></tr>`).join("") + `</table>`;
    root.querySelectorAll("tr.row").forEach(tr => {
      tr.onmouseenter = () => { hoverEdge = +tr.dataset.edge; draw(); };
      tr.onmouseleave = () => { hoverEdge = -1; draw(); };
    });
  }

  function focus() { return pinned || hover; }

  function refresh() {
    const d = Arch.data, f = focus();
    const related = new Set();
    if (f) { related.add(f); d.out.get(f).forEach(e => related.add(e.target)); d.inc.get(f).forEach(e => related.add(e.source)); }
    root.querySelectorAll(".rcard").forEach(el => {
      const id = el.dataset.id;
      el.classList.toggle("focus", id === f);
      el.classList.toggle("rel", !!f && id !== f && related.has(id));
      el.classList.toggle("dim", !!f && !related.has(id));
    });
    const box = Arch.$("resDetail");
    if (f) {
      const n = d.nodes.get(f), list = (arr, other) => arr.length ? arr.map(e => `${Arch.esc(Arch.name(d.nodes.get(other(e))))} <span style="color:${Arch.edgeColor(e)}">(${Arch.esc(e.transport || e.kind || "")}${e.label ? ", " + Arch.esc(e.label) : ""})</span>`).join("; ") : "nothing";
      box.innerHTML = `<b>${Arch.esc(Arch.name(n))}</b> <span class="pill" style="background:${Arch.RES_COLORS[n.residency]}">${Arch.RES_LABEL[n.residency] || n.residency}</span> — ${Arch.esc(Arch.blurb(n))}
        <div style="margin-top:4px">Receives from: ${list(d.inc.get(f), e => e.source)}</div><div>Sends to: ${list(d.out.get(f), e => e.target)}</div>`;
    } else box.textContent = "Hover or click a box to see what it does and what it exchanges with the rest.";
    draw();
  }

  function draw() {
    const d = Arch.data, board = Arch.$("resBoard"), svg = Arch.$("resSvg"); if (!d || !board || !svg) return;
    const br = board.getBoundingClientRect();
    svg.setAttribute("width", br.width); svg.setAttribute("height", br.height); svg.innerHTML = "";
    const R = new Map();
    board.querySelectorAll(".rcard").forEach(el => R.set(el.dataset.id, Arch.rectIn(el, board)));
    const f = focus();
    const show = [];
    d.edges.forEach((e, i) => {
      const ca = colOf(d.nodes.get(e.source)), cb = colOf(d.nodes.get(e.target));
      const involved = f && (e.source === f || e.target === f);
      if (ca !== cb || involved) show.push({ e, i, ca, cb });
    });
    // spread ports along each card's sides
    const left = new Map(), right = new Map(), add = (m, id, s) => { if (!m.has(id)) m.set(id, []); m.get(id).push(s); };
    for (const s of show) {
      const { e, ca, cb } = s;
      if (ca < cb) { add(right, e.source, s); add(left, e.target, s); }
      else if (ca > cb) { add(left, e.source, s); add(right, e.target, s); }
      else { add(right, e.source, s); add(right, e.target, s); }
    }
    const yc = (id) => { const r = R.get(id); return r ? r.y + r.h / 2 : 0; };
    for (const [id, arr] of left) arr.sort((p, q) => yc(p.e.source === id ? p.e.target : p.e.source) - yc(q.e.source === id ? q.e.target : q.e.source));
    for (const [id, arr] of right) arr.sort((p, q) => yc(p.e.source === id ? p.e.target : p.e.source) - yc(q.e.source === id ? q.e.target : q.e.source));
    const py = (m, id, s) => Arch.portY(m.get(id), s, R.get(id));

    for (const s of show) {
      const { e, i, ca, cb } = s, a = R.get(e.source), b = R.get(e.target); if (!a || !b) continue;
      const hot = f && (e.source === f || e.target === f), tableHot = hoverEdge === i;
      let p;
      if (ca < cb) {
        const x1 = a.x + a.w, x2 = b.x, y1 = py(right, e.source, s), y2 = py(left, e.target, s), dx = Math.max(50, (x2 - x1) * 0.45);
        p = { p0: [x1, y1], c1: [x1 + dx, y1], c2: [x2 - dx, y2], p1: [x2, y2] };
      } else if (ca > cb) {
        const x1 = a.x, x2 = b.x + b.w, y1 = py(left, e.source, s), y2 = py(right, e.target, s), lift = 70 + Math.abs(y1 - y2) * 0.1;
        p = { p0: [x1, y1], c1: [x1 - 110, y1 - lift], c2: [x2 + 110, y2 - lift], p1: [x2, y2] };
      } else {
        const x1 = a.x + a.w, x2 = b.x + b.w, y1 = py(right, e.source, s), y2 = py(right, e.target, s), bulge = ca === 2 ? -1 : 1;
        // same column: loop out into the gap on the side that faces the centre
        const xa = ca === 2 ? a.x : x1, xb = ca === 2 ? b.x : x2;
        p = { p0: [xa, y1], c1: [xa + bulge * 46, y1], c2: [xb + bulge * 46, y2], p1: [xb, y2] };
      }
      const back = ca > cb, color = back ? "#ee6b6b" : Arch.edgeColor(e);
      let alpha = f ? (hot ? 1 : 0.05) : 0.5, width = hot ? 2.8 : back ? 2.2 : 1.5;
      if (tableHot) { alpha = 1; width = 3.4; }
      else if (hoverEdge >= 0) alpha = 0.08;
      Arch.arrow(svg, p, color, { alpha, width, head: hot || tableHot ? 11 : 8, dash: e.kind === "data" ? "" : "6 5" });
    }
  }

  window.addEventListener("resize", () => { if (Arch.current === "residency") draw(); });
  new ResizeObserver(() => { if (Arch.current === "residency") draw(); }).observe(root);
  Arch.register("residency", { render, shown() { draw(); } });
})();
