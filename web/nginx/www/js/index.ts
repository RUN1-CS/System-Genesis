"use strict";

(() => {
  const WS_PORT = 3000;
  const POLL_MS = 5000;
  const STALE_AFTER_MS = 30_000;
  const RECONNECT_MIN_MS = 1000;
  const RECONNECT_MAX_MS = 30_000;

  type Status = "connecting" | "online" | "offline";
  type Usage = { total: number; used: number; pct: number };
  type Disk = Usage & { device: string; label: string };
  type Temperature = { label: string; celsius: number };
  type NormalizedMetrics = {
    hostname: string | null;
    time: Date | null;
    ram: Usage | null;
    swap: Usage | null;
    cores: (number | null)[];
    disks: Disk[];
    temperatures: Temperature[];
  };
  type JsonObject = Record<string, unknown>;

  let ws: WebSocket | null = null;
  let pollTimer: ReturnType<typeof setTimeout> | null = null;
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let retries = 0;
  let lastSample: Date | null = null;

  function $(id: string): HTMLElement {
    const element = document.getElementById(id);
    if (!element) throw new Error(`Missing required page element: ${id}`);
    return element;
  }

  function isRecord(value: unknown): value is JsonObject {
    return value !== null && typeof value === "object" && !Array.isArray(value);
  }

  function el<K extends keyof HTMLElementTagNameMap>(
    tag: K,
    className?: string,
    text?: string,
  ): HTMLElementTagNameMap[K] {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function num(value: unknown): number | null {
    if (value === null || value === undefined || value === "") return null;
    const parsed = Number(value);
    return Number.isFinite(parsed) ? parsed : null;
  }

  function maybeJson(value: unknown): unknown {
    if (typeof value !== "string") return value;
    try {
      return JSON.parse(value) as unknown;
    } catch {
      return null;
    }
  }

  const gib = (value: number): string => `${value.toFixed(2)} GiB`;
  const clampPct = (pct: number): number => Math.min(100, Math.max(0, pct));
  const level = (pct: number): "ok" | "warn" | "high" =>
    pct >= 90 ? "high" : pct >= 70 ? "warn" : "ok";

  function makeBar(label: string): HTMLDivElement {
    const bar = el("div", "bar");
    bar.setAttribute("role", "progressbar");
    bar.setAttribute("aria-label", label);
    bar.setAttribute("aria-valuemin", "0");
    bar.setAttribute("aria-valuemax", "100");
    bar.append(el("div", "bar-fill"));
    return bar;
  }

  function setBar(bar: HTMLElement, pct: number | null): void {
    const percent = pct === null ? 0 : clampPct(pct);
    const fill = bar.querySelector<HTMLElement>(".bar-fill");
    if (!fill) throw new Error("Progress bar is missing its fill element");
    bar.setAttribute("aria-valuenow", percent.toFixed(0));
    fill.style.width = `${percent}%`;
    fill.dataset.level = level(percent);
  }

  function usage(
    object: unknown,
    totalKey: string,
    freeKey: string,
    row: JsonObject,
  ): Usage | null {
    const source = isRecord(object)
      ? object
      : { total: row[totalKey], free: row[freeKey] };
    const total = num(source.total);
    const free = num(source.free);
    if (total === null || free === null) return null;
    const used = Math.max(total - free, 0);
    return { total, used, pct: total > 0 ? (used / total) * 100 : 0 };
  }

  function normalize(row: JsonObject): NormalizedMetrics {
    const cores = maybeJson(row.cpu_cores ?? row.cpu);
    const rawDisks = maybeJson(row.disk_metrics);
    const rawTemperatures = maybeJson(row.temperature_metrics);

    const disks: Disk[] = isRecord(rawDisks)
      ? Object.entries(rawDisks).map(([device, value]) => {
          const details = isRecord(value) ? value : {};
          return {
            device,
            label: typeof details.label === "string" ? details.label : "",
            ...(usage(null, "total", "free", details) ?? {
              total: 0,
              used: 0,
              pct: 0,
            }),
          };
        })
      : [];

    const temperatures: Temperature[] = Array.isArray(rawTemperatures)
      ? rawTemperatures.flatMap((sensor): Temperature[] => {
          if (!isRecord(sensor) || typeof sensor.label !== "string") return [];
          const celsius = num(sensor.celsius);
          return celsius === null ? [] : [{ label: sensor.label, celsius }];
        })
      : [];

    return {
      hostname: typeof row.hostname === "string" ? row.hostname : null,
      time:
        typeof row.time === "string" || typeof row.time === "number"
          ? new Date(row.time)
          : null,
      ram: usage(row.ram, "ram_total", "ram_free", row),
      swap: usage(row.swap, "swap_total", "swap_free", row),
      cores: Array.isArray(cores) ? cores.map(num) : [],
      disks,
      temperatures,
    };
  }

  function renderMemory(
    prefix: string,
    value: Usage | null,
    emptyText: string,
  ) {
    const text = $(prefix);
    const bar = $(`${prefix}-bar`);
    if (!value) {
      text.textContent = "—";
      setBar(bar, null);
    } else if (value.total === 0) {
      text.textContent = emptyText;
      setBar(bar, null);
    } else {
      text.textContent = `${gib(value.used)} / ${gib(value.total)} (${value.pct.toFixed(0)}%)`;
      setBar(bar, value.pct);
    }
  }

  function renderCores(cores: (number | null)[]): void {
    const list = $("cpu-list");
    if (!(list instanceof HTMLUListElement)) {
      throw new Error("CPU list must be an unordered list");
    }
    if (cores.length === 0) {
      list.replaceChildren(el("li", "empty", "No CPU data"));
      return;
    }
    list.replaceChildren(
      ...cores.map((pct, index) => {
        const item = el("li", "core");
        const bar = makeBar(`Core ${index} usage`);
        setBar(bar, pct);
        item.append(
          el("span", "core-id", `Core ${index}`),
          bar,
          el("span", "core-pct", pct === null ? "—" : `${pct.toFixed(1)}%`),
        );
        return item;
      }),
    );
  }

  function renderDisks(disks: Disk[]): void {
    const body = $("disk-body");
    if (!(body instanceof HTMLTableSectionElement)) {
      throw new Error("Disk table body is missing");
    }
    if (disks.length === 0) {
      const row = el("tr");
      const cell = el("td", "empty", "No disk data");
      cell.colSpan = 3;
      row.append(cell);
      body.replaceChildren(row);
      return;
    }
    body.replaceChildren(
      ...disks.map((disk) => {
        const row = el("tr");
        const cell = el("td", "usage-cell");
        const bar = makeBar(`${disk.device} used`);
        setBar(bar, disk.pct);
        cell.append(
          bar,
          el(
            "span",
            "usage-text",
            `${gib(disk.used)} / ${gib(disk.total)} (${disk.pct.toFixed(0)}%)`,
          ),
        );
        row.append(
          el("td", "", disk.device),
          el("td", "", disk.label || "—"),
          cell,
        );
        return row;
      }),
    );
  }

  function renderTemperatures(temperatures: Temperature[]): void {
    const body = $("temperature-body");
    if (!(body instanceof HTMLTableSectionElement)) {
      throw new Error("Temperature table body is missing");
    }
    if (temperatures.length === 0) {
      const row = el("tr");
      const cell = el("td", "empty", "No temperature sensors available");
      cell.colSpan = 2;
      row.append(cell);
      body.replaceChildren(row);
      return;
    }
    body.replaceChildren(
      ...temperatures.map((sensor) => {
        const row = el("tr");
        row.append(
          el("td", "", sensor.label),
          el("td", "temperature-value", `${sensor.celsius.toFixed(1)} °C`),
        );
        return row;
      }),
    );
  }

  function showAlert(message: string): void {
    const box = $("alert");
    box.hidden = !message;
    box.textContent = message;
  }

  function render(value: unknown): void {
    if (!isRecord(value) || Object.keys(value).length === 0) {
      showAlert("No samples recorded yet. Is the agent running?");
      return;
    }
    showAlert("");

    const metrics = normalize(value);
    $("hostname").textContent = metrics.hostname ?? "Unknown";
    lastSample =
      metrics.time && !Number.isNaN(metrics.time.getTime())
        ? metrics.time
        : null;
    $("time").textContent = lastSample
      ? lastSample.toLocaleString()
      : "Unknown";
    renderAge();

    renderMemory("ram", metrics.ram, "n/a");
    renderMemory("swap", metrics.swap, "No swap configured");
    renderCores(metrics.cores);
    renderDisks(metrics.disks);
    renderTemperatures(metrics.temperatures);
  }

  function renderAge(): void {
    const age = $("age");
    if (!lastSample) {
      age.textContent = "—";
      age.dataset.stale = "false";
      return;
    }
    const milliseconds = Math.max(Date.now() - lastSample.getTime(), 0);
    const seconds = Math.round(milliseconds / 1000);
    const text =
      seconds < 60
        ? `${seconds}s ago`
        : seconds < 3600
          ? `${Math.floor(seconds / 60)}m ago`
          : `${Math.floor(seconds / 3600)}h ago`;
    const stale = milliseconds > STALE_AFTER_MS;
    age.textContent = stale ? `${text} (stale, agent may be down)` : text;
    age.dataset.stale = String(stale);
  }

  function setStatus(state: Status): void {
    const labels: Record<Status, string> = {
      connecting: "Connecting…",
      online: "Live",
      offline: "Disconnected, retrying…",
    };
    $("status").dataset.state = state;
    $("status-text").textContent = labels[state];
  }

  function wsUrl(): string {
    const protocol = location.protocol === "https:" ? "wss" : "ws";
    const token = new URLSearchParams(location.search).get("token");
    const query = token ? `/?token=${encodeURIComponent(token)}` : "";
    return `${protocol}://${location.hostname}:${WS_PORT}${query}`;
  }

  function poll(): void {
    if (pollTimer) clearTimeout(pollTimer);
    if (ws?.readyState === WebSocket.OPEN && !document.hidden) {
      ws.send(JSON.stringify({ action: "get_logs" }));
    }
    pollTimer = setTimeout(poll, POLL_MS);
  }

  function scheduleReconnect(): void {
    if (reconnectTimer) clearTimeout(reconnectTimer);
    const base = Math.min(RECONNECT_MAX_MS, RECONNECT_MIN_MS * 2 ** retries);
    retries += 1;
    reconnectTimer = setTimeout(connect, base / 2 + Math.random() * (base / 2));
  }

  function onMessage(event: MessageEvent): void {
    if (typeof event.data !== "string") return;
    let data: unknown;
    try {
      data = JSON.parse(event.data) as unknown;
    } catch {
      return;
    }
    if (isRecord(data) && typeof data.error === "string") {
      showAlert(`Server error: ${data.error}`);
      return;
    }
    render(data);
  }

  function connect(): void {
    setStatus("connecting");
    ws = new WebSocket(wsUrl());
    ws.onopen = () => {
      retries = 0;
      setStatus("online");
      poll();
    };
    ws.onmessage = onMessage;
    ws.onerror = () => {};
    ws.onclose = (event: CloseEvent) => {
      console.warn("WebSocket closed", event.code, event.reason);
      if (pollTimer) clearTimeout(pollTimer);
      setStatus("offline");
      if (event.code === 1008) {
        showAlert("Unauthorized: missing or wrong ?token=");
      }
      scheduleReconnect();
    };
  }

  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && ws?.readyState === WebSocket.OPEN) poll();
  });

  document.addEventListener("DOMContentLoaded", () => {
    connect();
    setInterval(renderAge, 1000);
  });
})();
