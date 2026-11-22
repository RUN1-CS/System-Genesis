"use strict";

(() => {
  // ---------------------------------------------------------------------
  // Config
  // ---------------------------------------------------------------------
  const WS_PORT = 3000;
  const POLL_MS = 5000; // the agent reports every ~5s, so polling faster is wasted
  const STALE_AFTER_MS = 30000; // sample older than this => agent probably down
  const RECONNECT_MIN_MS = 1000;
  const RECONNECT_MAX_MS = 30000;

  // ---------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------
  let ws = null;
  let pollTimer = null;
  let reconnectTimer = null;
  let retries = 0;
  let lastSample = null; // Date of the newest sample shown

  const $ = (id) => document.getElementById(id);

  // ---------------------------------------------------------------------
  // Small DOM / formatting helpers (textContent only: no HTML injection)
  // ---------------------------------------------------------------------
  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  const num = (v) => {
    if (v === null || v === undefined || v === "") return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
  };

  // Postgres json columns arrive parsed, text columns arrive as strings.
  function maybeJson(v) {
    if (typeof v !== "string") return v;
    try {
      return JSON.parse(v);
    } catch {
      return null;
    }
  }

  const gib = (n) => `${n.toFixed(2)} GiB`;
  const clampPct = (p) => Math.min(100, Math.max(0, p));
  const level = (p) => (p >= 90 ? "high" : p >= 70 ? "warn" : "ok");

  function makeBar(label) {
    const bar = el("div", "bar");
    bar.setAttribute("role", "progressbar");
    bar.setAttribute("aria-label", label);
    bar.setAttribute("aria-valuemin", "0");
    bar.setAttribute("aria-valuemax", "100");
    bar.append(el("div", "bar-fill"));
    return bar;
  }

  function setBar(bar, pct) {
    const p = pct === null ? 0 : clampPct(pct);
    const fill = bar.firstElementChild;
    bar.setAttribute("aria-valuenow", p.toFixed(0));
    fill.style.width = `${p}%`;
    fill.dataset.level = level(p);
  }

  // ---------------------------------------------------------------------
  // Data normalisation: the server returns a DB row
  // (ram_free, ram_total, cpu_cores, disk_metrics, ...)
  // ---------------------------------------------------------------------
  function usage(obj, totalKey, freeKey, row) {
    const src =
      obj && typeof obj === "object"
        ? obj
        : { total: row[totalKey], free: row[freeKey] };
    const total = num(src.total);
    const free = num(src.free);
    if (total === null || free === null) return null;
    const used = Math.max(total - free, 0);
    return { total, used, pct: total > 0 ? (used / total) * 100 : 0 };
  }

  function normalize(row) {
    const cores = maybeJson(row.cpu_cores ?? row.cpu);
    const disks = maybeJson(row.disk_metrics);

    return {
      hostname: typeof row.hostname === "string" ? row.hostname : null,
      time: row.time ? new Date(row.time) : null,
      ram: usage(row.ram, "ram_total", "ram_free", row),
      swap: usage(row.swap, "swap_total", "swap_free", row),
      cores: Array.isArray(cores) ? cores.map(num) : [],
      disks:
        disks && typeof disks === "object" && !Array.isArray(disks)
          ? Object.entries(disks).map(([device, m]) => ({
              device,
              label: m && m.label ? String(m.label) : "",
              ...(usage(null, "total", "free", m || {}) || {
                total: 0,
                used: 0,
                pct: 0,
              }),
            }))
          : [],
    };
  }

  // ---------------------------------------------------------------------
  // Rendering
  // ---------------------------------------------------------------------
  function renderMemory(prefix, u, emptyText) {
    const text = $(prefix);
    const bar = $(`${prefix}-bar`);
    if (!u) {
      text.textContent = "—";
      setBar(bar, null);
    } else if (u.total === 0) {
      text.textContent = emptyText;
      setBar(bar, null);
    } else {
      text.textContent = `${gib(u.used)} / ${gib(u.total)} (${u.pct.toFixed(0)}%)`;
      setBar(bar, u.pct);
    }
  }

  function renderCores(cores) {
    const list = $("cpu-list");
    if (cores.length === 0) {
      list.replaceChildren(el("li", "empty", "No CPU data"));
      return;
    }
    list.replaceChildren(
      ...cores.map((pct, i) => {
        const li = el("li", "core");
        const bar = makeBar(`Core ${i} usage`);
        setBar(bar, pct);
        li.append(
          el("span", "core-id", `Core ${i}`),
          bar,
          el("span", "core-pct", pct === null ? "—" : `${pct.toFixed(1)}%`),
        );
        return li;
      }),
    );
  }

  function renderDisks(disks) {
    const body = $("disk-body");
    if (disks.length === 0) {
      const tr = el("tr");
      const td = el("td", "empty", "No disk data");
      td.colSpan = 3;
      tr.append(td);
      body.replaceChildren(tr);
      return;
    }
    body.replaceChildren(
      ...disks.map((d) => {
        const tr = el("tr");
        const cell = el("td", "usage-cell");
        const bar = makeBar(`${d.device} used`);
        setBar(bar, d.pct);
        cell.append(
          bar,
          el(
            "span",
            "usage-text",
            `${gib(d.used)} / ${gib(d.total)} (${d.pct.toFixed(0)}%)`,
          ),
        );
        tr.append(el("td", "", d.device), el("td", "", d.label || "—"), cell);
        return tr;
      }),
    );
  }

  function showAlert(message) {
    const box = $("alert");
    box.hidden = !message;
    box.textContent = message || "";
  }

  function render(row) {
    // Server sends {} when the logs table is still empty.
    if (!row || typeof row !== "object" || Object.keys(row).length === 0) {
      showAlert("No samples recorded yet. Is the agent running?");
      return;
    }
    showAlert("");

    const m = normalize(row);
    $("hostname").textContent = m.hostname ?? "Unknown";

    lastSample = m.time && !Number.isNaN(m.time.getTime()) ? m.time : null;
    $("time").textContent = lastSample
      ? lastSample.toLocaleString()
      : "Unknown";
    renderAge();

    renderMemory("ram", m.ram, "n/a");
    renderMemory("swap", m.swap, "No swap configured");
    renderCores(m.cores);
    renderDisks(m.disks);
  }

  function renderAge() {
    const age = $("age");
    if (!lastSample) {
      age.textContent = "—";
      age.dataset.stale = "false";
      return;
    }
    const ms = Math.max(Date.now() - lastSample.getTime(), 0);
    const s = Math.round(ms / 1000);
    const text =
      s < 60
        ? `${s}s ago`
        : s < 3600
          ? `${Math.floor(s / 60)}m ago`
          : `${Math.floor(s / 3600)}h ago`;
    const stale = ms > STALE_AFTER_MS;
    age.textContent = stale ? `${text} (stale, agent may be down)` : text;
    age.dataset.stale = String(stale);
  }

  function setStatus(state) {
    const labels = {
      connecting: "Connecting…",
      online: "Live",
      offline: "Disconnected, retrying…",
    };
    $("status").dataset.state = state;
    $("status-text").textContent = labels[state];
  }

  // ---------------------------------------------------------------------
  // WebSocket: connect, poll, reconnect with backoff (no page reloads)
  // ---------------------------------------------------------------------
  function wsUrl() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    // Browsers can't set headers on WebSockets, so a token (if the server
    // requires one) is taken from this page's URL: /index.php?token=...
    const token = new URLSearchParams(location.search).get("token");
    const query = token ? `/?token=${encodeURIComponent(token)}` : "";
    return `${proto}://${location.hostname}:${WS_PORT}${query}`;
  }

  function poll() {
    clearTimeout(pollTimer);
    if (ws && ws.readyState === WebSocket.OPEN && !document.hidden) {
      ws.send(JSON.stringify({ action: "get_logs" }));
    }
    pollTimer = setTimeout(poll, POLL_MS);
  }

  function scheduleReconnect() {
    clearTimeout(reconnectTimer);
    const base = Math.min(RECONNECT_MAX_MS, RECONNECT_MIN_MS * 2 ** retries);
    retries += 1;
    reconnectTimer = setTimeout(connect, base / 2 + Math.random() * (base / 2));
  }

  function onMessage(event) {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch {
      return;
    }
    if (data && typeof data.error === "string") {
      showAlert(`Server error: ${data.error}`);
      return;
    }
    render(data);
  }

  function connect() {
    setStatus("connecting");
    ws = new WebSocket(wsUrl());

    ws.onopen = () => {
      retries = 0;
      setStatus("online");
      poll();
    };
    ws.onmessage = onMessage;
    ws.onerror = () => {}; // onclose always follows; handled there
    ws.onclose = (ev) => {
      console.warn("WebSocket closed", ev.code, ev.reason);
      clearTimeout(pollTimer);
      setStatus("offline");
      if (ev.code === 1008) showAlert("Unauthorized: missing or wrong ?token=");
      scheduleReconnect();
    };
  }

  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && ws && ws.readyState === WebSocket.OPEN) poll();
  });

  addEventListener("DOMContentLoaded", () => {
    connect();
    setInterval(renderAge, 1000);
  });
})();
