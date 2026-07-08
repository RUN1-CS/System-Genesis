let ws;
let logTimer; // Timer for log timeout

// We can't interact with DOM elements before the DOM is loaded
addEventListener("DOMContentLoaded", () => {
  // Choose ws or wss to match page protocol and connect to backend on port 3000
  const proto = location.protocol === "https:" ? "wss" : "ws";
  // Connect to the WebSocket host
  ws = new WebSocket(`${proto}://${location.hostname}:3000`);

  // Catch WebSocket errors
  ws.onerror = (err) => {
    console.error("WebSocket error:", err);
  };

  // Start the update loop once connected
  ws.onopen = () => {
    updateLogContainer();
  };

  ws.onmessage = (event) => {
    let data;
    try {
      // Data arrives in JSON format so we parse it
      data = JSON.parse(event.data);
    } catch {
      return;
    }

    // Each piece of data has it's failsafe
    // We write the data to the dashboard
    if (data.hostname !== undefined) {
      const el = document.getElementById("hostname");
      if (el) el.textContent = data.hostname;
    }

    if (data.time !== undefined) {
      const el = document.getElementById("time");
      if (el) el.textContent = data.time;
    }

    if (
      data.ram !== undefined ||
      (data.ram_free !== undefined && data.ram_total !== undefined)
    ) {
      const el = document.getElementById("ram");
      if (el) {
        if (data.ram !== undefined) {
          el.textContent = `${Number(data.ram_total - data.ram_free).toFixed(2)} / ${Number(data.ram_total).toFixed(2)} GiB`;
        }
      }
    }

    if (
      data.swap !== undefined ||
      (data.swap_free !== undefined && data.swap_total !== undefined)
    ) {
      const el = document.getElementById("swap");
      if (el) {
        if (data.swap !== undefined) {
          el.textContent = `${Number(data.swap_total - data.swap_free).toFixed(2)} / ${Number(data.swap_total).toFixed(2)} GiB`;
        }
      }
    }

    if (Array.isArray(data.cpu) || Array.isArray(data.cpu_cores)) {
      const cpuData = Array.isArray(data.cpu) ? data.cpu : data.cpu_cores;
      let cpuTable = document.getElementById("cpu-table");
      // Remove old rows
      while (cpuTable.rows.length > 1) cpuTable.deleteRow(1);
      cpuData.forEach((usage, idx) => {
        const row = cpuTable.insertRow();
        const c0 = row.insertCell();
        const c1 = row.insertCell();
        c0.textContent = String(idx);
        c1.textContent = String(usage);
      });
    }

    if (
      Array.isArray(data.disks) ||
      (data.disk_metrics && typeof data.disk_metrics === "object")
    ) {
      const diskData = Array.isArray(data.disks)
        ? data.disks
        : Object.entries(data.disk_metrics).map(([device, m]) => ({
            device,
            usage: `${Number(m.total - m.free).toFixed(2)} / ${Number(m.total).toFixed(2)} GiB${m.label ? ` (${m.label})` : ""}`,
          }));
      let diskTable = document.getElementById("disk-table");
      while (diskTable.rows.length > 1) diskTable.deleteRow(1);
      diskData.forEach((d) => {
        const row = diskTable.insertRow();
        const c0 = row.insertCell();
        const c1 = row.insertCell();
        c0.textContent = d.device || "";
        c1.textContent = d.usage || "";
      });
    }
    updateLogContainer();
  };

  ws.onclose = (ev) => {
    console.warn("WebSocket closed", ev.code, ev.reason);
    if (logTimer) clearTimeout(logTimer);
    logTimer = setTimeout(() => {
      location.reload();
    }, 1000);
  };
});

// Update log loop
async function updateLogContainer() {
  await new Promise((resolve) => setTimeout(resolve, 1000));
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    // Try again later
    logTimer = setTimeout(updateLogContainer, 1000);
    return;
  }
  const msg = {
    action: "get_logs",
  };
  ws.send(JSON.stringify(msg));
}
