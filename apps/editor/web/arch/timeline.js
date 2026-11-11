"use strict";
// Timeline: how often each system runs, what one frame looks like step by step, and what wakes up when something happens.
(function () {
  const root = Arch.$("v-timeline");
  let eventId = null, focus = null, layout = null;

  const BUCKETS = [
    { id: "once", title: "Once, then never", hz: "Done at start-up and kept.", w: "3%" },
    { id: "edit", title: "When something is edited", hz: "Only after you change the world, a setting or a source file.", w: "10%" },
    { id: "move", title: "When you or the sun move", hz: "Redone when the sun, your height or your view changes.", w: "25%" },
    { id: "ask", title: "Only when asked", hz: "Runs on demand when another system needs an answer.", w: "35%" },
    { id: "second", title: "About once a second", hz: "Slow background work such as the weather.", w: "50%" },
    { id: "trickle", title: "A little every frame", hz: "A small fixed budget each frame, so it never stutters.", w: "75%" },
    { id: "frame", title: "Every single frame", hz: "About 60 times per second.", w: "100%" },
    { id: "derived", title: "Follows its inputs", hz: "No schedule of its own: refreshes when what feeds it does.", w: "40%" }
  ];
  function bucket(n) {
    const r = (n.update_rate || "").toLowerCase();
    if (!r) return "derived";
    if (/static/.test(r)) return "once";
    if (/budget|few voxels|~64/.test(r)) return "trickle";
    if (/per frame/.test(r)) return "frame";
    if (/time quantum|sim step/.test(r)) return "second";
    if (/query/.test(r)) return "ask";
    if (/altitude|sun|per view|hashed|scroll/.test(r)) return "move";
    return "edit";
  }
  const EVENTS = [
    { id: "start", icon: "🚀", label: "The program starts", test: n => bucket(n) === "once" },
    { id: "edit", icon: "✏️", label: "You edit something", test: n => bucket(n) === "edit" },
    { id: "move", icon: "🧭", label: "You move, or the sun moves", test: n => bucket(n) === "move" },
    { id: "second", icon: "⏱️", label: "A second of weather passes", test: n => bucket(n) === "second" },
    { id: "frame", icon: "🎞️", label: "A new frame is drawn", test: n => bucket(n) === "frame" || bucket(n) === "trickle" }
  ];

  // Systems woken by the chosen event, plus everything downstream of them (distance = how many hand-offs later).
  function cascade() {
    const d = Arch.data, ev = EVENTS.find(e => e.id === eventId); if (!ev) return null;
    const level = new Map(), queue = [];
    for (const n of d.nodes.values()) if (ev.test(n)) { level.set(n.id, 0); queue.push(n.id); }
    for (let i = 0; i < queue.length; i++) {
      const id = queue[i];
      for (const e of d.out.get(id)) if (!level.has(e.target)) { level.set(e.target, level.get(id) + 1); queue.push(e.target); }
    }
    return level;
  }

  const chipHtml = (n, cls = "") => {
    const rc = Arch.RES_COLORS[n.residency] || "#888";
    return `<span class="tchip ${cls}" data-id="${Arch.esc(n.id)}" style="--rc:${rc}" title="${Arch.esc(Arch.blurb(n))}">${Arch.esc(Arch.name(n))}<small>${Arch.esc(Arch.RES_LABEL[n.residency] || "")}${n.update_rate ? " · " + Arch.esc(n.update_rate) : ""}</small></span>`;
  };

  // ---- one frame, in order ----
  function frameModel() {
    const d = Arch.data, set = new Set([...d.nodes.values()].filter(n => ["frame", "trickle"].includes(bucket(n))).map(n => n.id));
    const level = new Map(), visiting = new Set();
    const depth = (id) => {
      if (level.has(id)) return level.get(id);
      if (visiting.has(id)) return 0;
      visiting.add(id);
      let best = 0;
      for (const e of d.inc.get(id)) if (set.has(e.source)) best = Math.max(best, depth(e.source) + 1);
      visiting.delete(id); level.set(id, best); return best;
    };
    for (const id of set) depth(id);
    const columns = Math.max(0, ...level.values()) + 1;
    const laneOf = (n) => n.residency === "cpu" ? 0 : n.residency === "gpu" ? 2 : 1;
    return { set, level, columns, laneOf };
  }

  function render() {
    const d = Arch.data; if (!d) return;
    const groups = new Map(BUCKETS.map(b => [b.id, []]));
    for (const n of d.nodes.values()) groups.get(bucket(n)).push(n);
    const lvl = cascade();
    const rungs = BUCKETS.filter(b => groups.get(b.id).length).map(b => `<div class="rung"><h4>${b.title}</h4><div class="hz">${b.hz}</div><div class="bar" style="--w:${b.w}"></div>
      ${groups.get(b.id).map(n => chipHtml(n, lvl ? (lvl.get(n.id) === 0 ? "lit" : lvl.has(n.id) ? "" : "dim") : "")).join("")}</div>`).join("");

    const evButtons = EVENTS.map(e => `<div class="ev ${e.id === eventId ? "on" : ""}" data-ev="${e.id}">${e.icon} ${e.label}</div>`).join("");
    let cascadeHtml = `<p class="lead">Pick something that can happen and see which systems wake up, in the order the work flows.</p>`;
    if (lvl) {
      const maxL = Math.max(...lvl.values()), steps = [];
      for (let l = 0; l <= maxL; l++) {
        const ns = [...lvl].filter(([, v]) => v === l).map(([id]) => d.nodes.get(id));
        if (ns.length) steps.push(`<div class="cstep ${l === 0 ? "first" : ""}"><h5>${l === 0 ? "Wakes up first" : "Then, " + l + " hand-off" + (l > 1 ? "s" : "") + " later"}</h5>${ns.map(n => chipHtml(n)).join("")}</div>`);
      }
      cascadeHtml = `<div class="cascade">${steps.join("")}</div>
        <p class="lead" style="margin-top:8px">${lvl.size} of ${d.nodes.size} systems are touched by this. Systems that never appear here stay asleep.</p>`;
    }

    const fm = frameModel(); layout = fm;
    const lanes = [["CPU — planner", Arch.RES_COLORS.cpu], ["Bridge — carried across", Arch.RES_COLORS["cpu->gpu"]], ["GPU — helpers", Arch.RES_COLORS.gpu]];
    const cells = [];
    for (let l = 0; l < 3; l++) {
      cells.push(`<div class="flane" style="color:${lanes[l][1]};background:${lanes[l][1]}14">${lanes[l][0]}</div>`);
      for (let c = 0; c < fm.columns; c++) {
        const here = [...fm.set].map(id => d.nodes.get(id)).filter(n => fm.level.get(n.id) === c && fm.laneOf(n) === l);
        cells.push(`<div class="fcell" style="background:${lanes[l][1]}0a">${here.map(n => `<div class="fchip" data-id="${Arch.esc(n.id)}" style="--rc:${Arch.RES_COLORS[n.residency] || "#888"}" title="${Arch.esc(Arch.blurb(n))}">${Arch.esc(Arch.name(n))}<small>${Arch.esc(n.update_rate || "")}</small></div>`).join("")}</div>`);
      }
    }
    const heads = `<div></div>` + Array.from({ length: fm.columns }, (_, c) => `<div class="fhead">${c === 0 ? "start of frame" : c === fm.columns - 1 ? "end of frame" : "then"}</div>`).join("");

    root.innerHTML = `<div class="vw">
      <h1>What happens, and when</h1>
      <p class="lead">Not everything runs all the time. Some work is done once, some only when you edit, some every one of the 60 frames each second. Knowing the rhythm tells you what is cheap and what is not.</p>
      <h3>How often does each thing run?</h3>
      <div class="ladder">${rungs}</div>
      <h3>If this happens… what wakes up?</h3>
      <div class="events">${evButtons}</div>
      ${cascadeHtml}
      <h3>One frame, step by step <span style="text-transform:none;letter-spacing:0;font-weight:400"> — everything that runs each frame, left to right, on the side it runs on (about 16 milliseconds in total)</span></h3>
      <div class="frame" id="frameBoard" style="grid-template-columns:150px repeat(${fm.columns}, minmax(150px, 1fr))"><svg id="frameSvg"></svg>${heads}${cells.join("")}</div>
    </div>`;
    root.querySelectorAll("[data-ev]").forEach(el => el.onclick = () => { eventId = eventId === el.dataset.ev ? null : el.dataset.ev; render(); });
    root.querySelectorAll(".fchip").forEach(el => {
      el.onmouseenter = () => { focus = el.dataset.id; drawFrame(); };
      el.onmouseleave = () => { focus = null; drawFrame(); };
    });
    drawFrame();
  }

  function drawFrame() {
    const d = Arch.data, board = Arch.$("frameBoard"), svg = Arch.$("frameSvg"); if (!d || !board || !svg || !layout) return;
    const br = board.getBoundingClientRect(); svg.setAttribute("width", br.width); svg.setAttribute("height", br.height); svg.innerHTML = "";
    const R = new Map();
    board.querySelectorAll(".fchip").forEach(el => {
      R.set(el.dataset.id, Arch.rectIn(el, board));
      el.classList.toggle("focus", el.dataset.id === focus);
      const rel = focus && (el.dataset.id === focus || d.out.get(focus).some(e => e.target === el.dataset.id) || d.inc.get(focus).some(e => e.source === el.dataset.id));
      el.classList.toggle("dim", !!focus && !rel);
    });
    const list = d.edges.filter(e => layout.set.has(e.source) && layout.set.has(e.target) && R.has(e.source) && R.has(e.target));
    const outs = new Map(), ins = new Map(), add = (m, id, e) => { if (!m.has(id)) m.set(id, []); m.get(id).push(e); };
    for (const e of list) { add(outs, e.source, e); add(ins, e.target, e); }
    const cy = (id) => { const r = R.get(id); return r.y + r.h / 2; };
    for (const [id, a] of outs) a.sort((p, q) => cy(p.target) - cy(q.target));
    for (const [id, a] of ins) a.sort((p, q) => cy(p.source) - cy(q.source));
    for (const e of list) {
      const a = R.get(e.source), b = R.get(e.target), hot = focus && (e.source === focus || e.target === focus);
      const x1 = a.x + a.w, x2 = b.x, y1 = Arch.portY(outs.get(e.source), e, a), y2 = Arch.portY(ins.get(e.target), e, b);
      const forward = x2 > x1, dx = Math.max(30, Math.abs(x2 - x1) * 0.45);
      const p = forward ? { p0: [x1, y1], c1: [x1 + dx, y1], c2: [x2 - dx, y2], p1: [x2, y2] }
                        : { p0: [a.x + a.w / 2, y1], c1: [a.x + a.w / 2, y1 + 40], c2: [b.x + b.w / 2, y2 - 40], p1: [b.x + b.w / 2, y2] };
      Arch.arrow(svg, p, Arch.edgeColor(e), { alpha: focus ? (hot ? 1 : 0.06) : 0.5, width: hot ? 2.6 : 1.5, head: hot ? 10 : 7, dash: e.kind === "data" ? "" : "6 5" });
    }
  }

  new ResizeObserver(() => { if (Arch.current === "timeline") drawFrame(); }).observe(root);
  Arch.register("timeline", { render, shown() { drawFrame(); } });
})();
