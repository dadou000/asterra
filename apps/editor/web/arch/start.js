"use strict";
// "Start here": a guided tour in everyday words, one chapter at a time, plus a small glossary.
(function () {
  const root = Arch.$("v-start"), STOPS = window.ARCH_STOPS || [], GLOSS = window.ARCH_GLOSSARY || [];
  let step = 0;
  try { step = Math.min(STOPS.length - 1, Math.max(0, parseInt(localStorage.getItem("architecture.step") || "0", 10) || 0)); } catch {}

  const who = (r) => ({ cpu: ["planner", "#e0a84a"], gpu: ["helpers", "#5aa9ff"], "cpu->gpu": ["planner → helpers", "#d46bd0"], "cpu+gpu": ["both", "#7fd29a"] }[r] || [r, "#888"]);

  function teamHtml() {
    return `<div class="team">
      <div class="who cpu"><h4>🧠 The planner (CPU)</h4><p>One very clever manager. Does <b>one thing at a time</b>, but is brilliant at deciding, scheduling and handling surprises.</p><p>Keeps its notes in <b>main memory</b>.</p></div>
      <div class="who gpu"><h4>🎨 The helpers (GPU)</h4><p>A stadium of a <b>thousand painters</b>. Each colours one tiny dot, <b>all at the same moment</b>. Hopeless at decisions, astonishing at painting.</p><p>Keep their notes in <b>video memory</b>.</p></div>
    </div>`;
  }
  function exploreHtml() {
    const go = (tab, ic, t, d) => `<div class="go" data-go="${tab}"><b>${ic} ${t}</b><span style="color:#9aa3b4;font-size:13.5px">${d}</span></div>`;
    return `<div class="explore">
      ${go("residency", "↔️", "Who does the work", "Planner on one side, helpers on the other, and everything that has to be carried across.")}
      ${go("timeline", "⏱️", "What happens when", "Which jobs run once, which when you edit, and which every single frame, in order.")}
      ${go("map", "🗺️", "Everything connected", "The full map of all the systems and how they feed each other.")}
    </div>`;
  }
  function partsHtml(s) {
    const d = Arch.data; if (!s.nodes || !s.nodes.length) return "";
    const items = s.nodes.map(id => d && d.nodes.get(id)).filter(Boolean).map(n => {
      const [w, c] = who(n.residency), lc = Arch.LAYER_COLORS[Arch.layerOf(n)] || "#888", p = ARCH_PLAIN[n.id];
      return `<div class="part" style="border-left-color:${lc}" title="${Arch.esc(n.tech || n.label)}">
        <span class="pill who" style="background:${c}">${w}</span><b>${Arch.esc(p ? p[0] : n.label)}</b>
        <span class="d">${Arch.esc(p ? p[1] : n.summary)}</span></div>`;
    }).join("");
    return `<p class="side-h">The parts in this chapter</p><div class="parts">${items}</div>`;
  }

  function render() {
    const s = STOPS[step]; if (!s) return;
    const journey = STOPS.map((t, i) => `<div class="jstep ${i === step ? "on" : i < step ? "done" : ""}" data-step="${i}"><span class="ic">${t.icon}</span>${Arch.esc(t.short)}</div>`).join("");
    const text = s.text.map(t => `<p>${Arch.esc(t)}</p>`).join("");
    const body = s.kind === "team" ? teamHtml() : s.kind === "explore" ? exploreHtml() : "";
    const analogy = s.analogy ? `<div class="analogy">💭 ${Arch.esc(s.analogy)}</div>` : "";
    const side = s.kind === "explore" || s.kind === "welcome" || s.kind === "team" ? "" : partsHtml(s);
    root.innerHTML = `<div class="vw">
      <h1>Start here <span style="font-weight:400;color:var(--muted);font-size:15px">· a three-minute tour, no experience needed</span></h1>
      <div class="journey">${journey}</div>
      <div class="story" style="${side ? "" : "grid-template-columns:minmax(0,1fr)"}">
        <div class="scard"><div class="big">${s.icon}</div><h1>${Arch.esc(s.title)}</h1>${text}${body}${analogy}
          <div class="nav"><button class="btn ghost" id="stBack" ${step === 0 ? "disabled" : ""}>← Back</button>
            <button class="btn" id="stNext" ${step === STOPS.length - 1 ? "disabled" : ""}>Next →</button>
            <span class="count">${step + 1} of ${STOPS.length}</span></div></div>
        ${side ? `<div>${side}</div>` : ""}
      </div>
      <h3>Words you might wonder about</h3>
      <div class="gloss">${GLOSS.map(([w, d]) => `<div><b>${Arch.esc(w)}</b> — ${Arch.esc(d)}</div>`).join("")}</div>
    </div>`;
    root.querySelectorAll("[data-step]").forEach(e => e.onclick = () => go(+e.dataset.step));
    root.querySelectorAll("[data-go]").forEach(e => e.onclick = () => Arch.show(e.dataset.go));
    const b = Arch.$("stBack"), n = Arch.$("stNext");
    if (b) b.onclick = () => go(step - 1);
    if (n) n.onclick = () => go(step + 1);
  }
  function go(i) {
    step = Math.min(STOPS.length - 1, Math.max(0, i));
    try { localStorage.setItem("architecture.step", String(step)); } catch {}
    render(); root.scrollTop = 0;
  }
  Arch.register("start", { render, shown() { if (!Arch.hasData()) render(); } });
})();
