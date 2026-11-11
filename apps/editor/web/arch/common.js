"use strict";
// Shared registry and helpers of the Architecture panel's tabs. Each tab is a file in this folder that calls Arch.register().
window.Arch = window.Arch || {};
Object.assign(Arch, {
  views: {}, current: null, plain: false, data: null,
  LAYER_ORDER: ["source", "medium", "weather", "cloud", "control", "terrain", "lighting", "output"],
  LAYER_TITLES: { source: "Sources", medium: "Atmosphere & medium", weather: "Weather", cloud: "Clouds", control: "Scheduling & control",
                  terrain: "Terrain", lighting: "Lighting", output: "Output" },
  LAYER_COLORS: { source: "#e8c25c", medium: "#6fb5e8", weather: "#6fd6c0", cloud: "#b9a7f2", control: "#f09a7a",
                  terrain: "#9bcf6a", lighting: "#f4e27a", output: "#ef7fb0" },
  RES_COLORS: { cpu: "#e0a84a", gpu: "#5aa9ff", "cpu->gpu": "#d46bd0", "cpu+gpu": "#7fd29a", offline: "#8b93a3" },
  RES_LABEL: { cpu: "CPU", gpu: "GPU", "cpu->gpu": "CPU→GPU", "cpu+gpu": "CPU+GPU", offline: "OFFLINE" },
  RES_PLAIN: { cpu: "Planner (CPU)", gpu: "Helpers (GPU)", "cpu->gpu": "Prepared by the planner, sent to the helpers", "cpu+gpu": "Shared by both", offline: "Offline" },

  $: (id) => document.getElementById(id),
  esc: (s) => String(s ?? "").replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c])),
  register(id, view) { this.views[id] = view; view.dirty = true; },
  hasData() { return !!(this.data && this.data.nodes && this.data.nodes.size); },

  // Display name of a node: everyday words when the toggle is on (or always, for the Start tab), else the technical label.
  name(n, forcePlain) {
    const p = window.ARCH_PLAIN && window.ARCH_PLAIN[n.id];
    return (forcePlain || this.plain) && p ? p[0] : (n.tech || n.label);
  },
  blurb(n) { const p = window.ARCH_PLAIN && window.ARCH_PLAIN[n.id]; return p ? p[1] : n.summary; },

  layerOf(n) { return this.LAYER_ORDER.includes(n.layer) ? n.layer : "other"; },
  edgeColor(e) {
    const t = e.transport || "";
    if (t.startsWith("gpu->cpu")) return "#ee6b6b";
    if (t.startsWith("cpu->gpu")) return this.RES_COLORS["cpu->gpu"];
    if (t.startsWith("gpu")) return this.RES_COLORS.gpu;
    if (t.startsWith("cpu")) return this.RES_COLORS.cpu;
    return "#8a93a6";
  },

  // The data model was rebuilt (or its names changed): re-render the visible tab, mark the others stale.
  notify() {
    for (const v of Object.values(this.views)) v.dirty = true;
    const cur = this.views[this.current];
    if (cur && cur.render) { cur.dirty = false; cur.render(); }
  },
  show(id) {
    if (!this.views[id] && id !== "map") id = "start";
    this.current = id;
    document.querySelectorAll("#tabs .tab").forEach(b => b.classList.toggle("on", b.dataset.tab === id));
    document.querySelectorAll(".view").forEach(v => v.classList.toggle("active", v.id === "v-" + id));
    try { localStorage.setItem("architecture.tab", id); } catch {}
    const v = this.views[id];
    if (v && v.render && this.hasData() && v.dirty) { v.dirty = false; v.render(); }
    if (v && v.shown) v.shown();
  },

  // A cubic curve with an arrow head, drawn into an <svg>.
  arrow(svg, p, color, o = {}) {
    const NS = "http://www.w3.org/2000/svg";
    const g = document.createElementNS(NS, "g");
    const path = document.createElementNS(NS, "path");
    path.setAttribute("d", `M${p.p0[0]},${p.p0[1]}C${p.c1[0]},${p.c1[1]} ${p.c2[0]},${p.c2[1]} ${p.p1[0]},${p.p1[1]}`);
    path.setAttribute("fill", "none"); path.setAttribute("stroke", color);
    path.setAttribute("stroke-width", o.width || 1.6); path.setAttribute("opacity", o.alpha ?? 0.5);
    if (o.dash) path.setAttribute("stroke-dasharray", o.dash);
    g.appendChild(path);
    const ang = Math.atan2(p.p1[1] - p.c2[1], p.p1[0] - p.c2[0]), s = o.head || 8;
    const tip = p.p1, a1 = [tip[0] - s * Math.cos(ang - .42), tip[1] - s * Math.sin(ang - .42)], a2 = [tip[0] - s * Math.cos(ang + .42), tip[1] - s * Math.sin(ang + .42)];
    const head = document.createElementNS(NS, "polygon");
    head.setAttribute("points", `${tip[0]},${tip[1]} ${a1[0]},${a1[1]} ${a2[0]},${a2[1]}`);
    head.setAttribute("fill", color); head.setAttribute("opacity", o.alpha ?? 0.5);
    g.appendChild(head);
    svg.appendChild(g);
    return g;
  },

  // Rectangle of an element relative to a container (for arrows between DOM cards).
  rectIn(el, box) {
    const r = el.getBoundingClientRect(), b = box.getBoundingClientRect();
    return { x: r.left - b.left, y: r.top - b.top, w: r.width, h: r.height };
  },
  portY(list, edge, r) {
    const i = list.indexOf(edge), c = list.length, span = Math.min(r.h - 14, 9 * (c - 1));
    return r.y + r.h / 2 + (c > 1 ? (i / (c - 1) - 0.5) * span : 0);
  },
  // Weighted ordering inside columns: sort by the average position of neighbours (barycentre sweeps) to shorten arrows.
  orderColumns(cols, neighbours, sweeps = 8) {
    const pos = new Map();
    const reindex = () => { for (const c of cols) c.forEach((n, i, a) => pos.set(n.id, a.length > 1 ? i / (a.length - 1) : 0.5)); };
    reindex();
    for (let s = 0; s < sweeps; s++) {
      for (const c of (s % 2 ? [...cols].reverse() : cols)) {
        const key = new Map(c.map(n => {
          const ns = neighbours(n); let sum = 0;
          for (const m of ns) sum += pos.get(m) ?? 0.5;
          return [n.id, ns.length ? sum / ns.length : pos.get(n.id)];
        }));
        c.sort((a, b) => key.get(a.id) - key.get(b.id));
        reindex();
      }
    }
  }
});
