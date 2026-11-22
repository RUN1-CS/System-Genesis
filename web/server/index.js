"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const WebSocket = require("ws");
const { pool } = require("./db");

// ---------------------------------------------------------------------------
// Config (all overridable through environment variables)
// ---------------------------------------------------------------------------
const PORT = Number(process.env.PORT) || 3000;
const HOST = process.env.HOST || "0.0.0.0";
const AUTH_TOKEN = process.env.SYSMON_TOKEN || ""; // empty = auth disabled
const MAX_PAYLOAD_BYTES = 64 * 1024;
const MAX_LOGS_PER_REQUEST = 1000;
const MAX_CORES = 1024;
const MAX_DISKS = 256;
const MAX_TEMPERATURES = 256;
const HEARTBEAT_MS = 30_000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
class ValidationError extends Error {}

const isNonNegNumber = (v) =>
  typeof v === "number" && Number.isFinite(v) && v >= 0;
const isPlainObject = (v) =>
  v !== null && typeof v === "object" && !Array.isArray(v);

function requireNumber(value, name) {
  if (!isNonNegNumber(value)) throw new ValidationError(`Invalid ${name}`);
  return value;
}

function safeEqual(a, b) {
  // Hash first so the comparison is constant-time even for different lengths.
  const ha = crypto.createHash("sha256").update(String(a)).digest();
  const hb = crypto.createHash("sha256").update(String(b)).digest();
  return crypto.timingSafeEqual(ha, hb);
}

// Token may come as "Authorization: Bearer <t>" or "?token=<t>" (browsers
// can't set headers on WebSocket connections).
function isAuthorized(req) {
  if (!AUTH_TOKEN) return true;
  let supplied = "";
  try {
    supplied = new URL(req.url, "http://localhost").searchParams.get("token");
  } catch {
    /* malformed URL: treated as unauthorized below */
  }
  if (!supplied) {
    supplied = (req.headers.authorization || "").replace(/^Bearer\s+/i, "");
  }
  return supplied !== "" && safeEqual(supplied, AUTH_TOKEN);
}

function send(ws, payload) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(payload));
}

function sendError(ws, message) {
  send(ws, { error: message });
}

// ---------------------------------------------------------------------------
// Metrics ingestion
// ---------------------------------------------------------------------------
function validateMetrics(m) {
  if (
    typeof m.hostname !== "string" ||
    m.hostname.length === 0 ||
    m.hostname.length > 255
  ) {
    throw new ValidationError("Invalid hostname");
  }
  if (!isPlainObject(m.ram) || !isPlainObject(m.swap)) {
    throw new ValidationError("Missing ram/swap");
  }

  if (
    !Array.isArray(m.cpu_cores) ||
    m.cpu_cores.length > MAX_CORES ||
    !m.cpu_cores.every(isNonNegNumber)
  ) {
    throw new ValidationError("Invalid cpu_cores");
  }

  if (!isPlainObject(m.disk_metrics)) {
    throw new ValidationError("Invalid disk_metrics");
  }
  const disks = Object.values(m.disk_metrics);
  if (
    disks.length > MAX_DISKS ||
    !disks.every(
      (d) =>
        isPlainObject(d) && isNonNegNumber(d.free) && isNonNegNumber(d.total),
    )
  ) {
    throw new ValidationError("Invalid disk_metrics entry");
  }

  const temperatures = m.temperature_metrics ?? [];
  if (
    !Array.isArray(temperatures) ||
    temperatures.length > MAX_TEMPERATURES ||
    !temperatures.every(
      (sensor) =>
        isPlainObject(sensor) &&
        typeof sensor.label === "string" &&
        sensor.label.length > 0 &&
        sensor.label.length <= 255 &&
        typeof sensor.celsius === "number" &&
        Number.isFinite(sensor.celsius) &&
        sensor.celsius >= -273.15 &&
        sensor.celsius <= 1000,
    )
  ) {
    throw new ValidationError("Invalid temperature_metrics");
  }

  // Prefer the agent's timestamp, fall back to receive time if unusable.
  let time = new Date(m.time);
  if (Number.isNaN(time.getTime())) time = new Date();

  return {
    hostname: m.hostname,
    time,
    ram_free: requireNumber(m.ram.free, "ram.free"),
    ram_total: requireNumber(m.ram.total, "ram.total"),
    swap_free: requireNumber(m.swap.free, "swap.free"),
    swap_total: requireNumber(m.swap.total, "swap.total"),
    cpu_cores: JSON.stringify(m.cpu_cores),
    disk_metrics: JSON.stringify(m.disk_metrics),
    temperature_metrics: JSON.stringify(temperatures),
  };
}

async function handleMetrics(message) {
  const log = validateMetrics(message);
  await schemaReady;
  await pool.query(
    `INSERT INTO logs
       (hostname, time, ram_free, ram_total, swap_free, swap_total, cpu_cores, disk_metrics, temperature_metrics)
     VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)`,
    [
      log.hostname,
      log.time,
      log.ram_free,
      log.ram_total,
      log.swap_free,
      log.swap_total,
      log.cpu_cores,
      log.disk_metrics,
      log.temperature_metrics,
    ],
  );
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
async function handleGetLogs(ws, message) {
  // "ammount" (sic) is accepted for backwards compatibility.
  const requested = message.amount ?? message.ammount ?? 1;
  const limit = Math.min(
    Math.max(Math.trunc(Number(requested)) || 1, 1),
    MAX_LOGS_PER_REQUEST,
  );

  const params = [];
  let where = "";
  if (message.hostname !== undefined) {
    if (typeof message.hostname !== "string") {
      throw new ValidationError("Invalid hostname filter");
    }
    params.push(message.hostname);
    where = `WHERE hostname = $${params.length}`;
  }
  params.push(limit);

  await schemaReady;
  const { rows } = await pool.query(
    `SELECT * FROM logs ${where} ORDER BY time DESC LIMIT $${params.length}`,
    params,
  );

  // Legacy clients ask for one row and expect a bare object back;
  // asking for more returns an array.
  send(ws, limit === 1 ? (rows[0] ?? {}) : rows);
}

const actions = {
  get_logs: handleGetLogs,
};

async function handleAction(ws, message) {
  const handler = Object.hasOwn(actions, message.action)
    ? actions[message.action]
    : null;
  if (!handler) throw new ValidationError("Unknown action");
  await handler(ws, message);
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------
async function runMigrations() {
  const client = await pool.connect();
  try {
    await client.query("BEGIN");
    await client.query("SELECT pg_advisory_xact_lock(1947162124)");
    await client.query(
      `CREATE TABLE IF NOT EXISTS schema_migrations (
         filename TEXT PRIMARY KEY,
         applied_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
       )`,
    );

    const directory = path.join(__dirname, "migrations");
    const files = (await fs.promises.readdir(directory))
      .filter((filename) => /^\d+_.+\.sql$/.test(filename))
      .sort();

    for (const filename of files) {
      const { rowCount } = await client.query(
        "SELECT 1 FROM schema_migrations WHERE filename = $1",
        [filename],
      );
      if (rowCount > 0) continue;

      const sql = await fs.promises.readFile(
        path.join(directory, filename),
        "utf8",
      );
      await client.query(sql);
      await client.query(
        "INSERT INTO schema_migrations (filename) VALUES ($1)",
        [filename],
      );
    }

    await client.query("COMMIT");
  } catch (err) {
    try {
      await client.query("ROLLBACK");
    } catch (rollbackError) {
      console.error("Database migration rollback failed:", rollbackError);
    }
    throw err;
  } finally {
    client.release();
  }
}

const schemaReady = runMigrations();
const wss = new WebSocket.Server({
  port: PORT,
  host: HOST,
  maxPayload: MAX_PAYLOAD_BYTES,
});

wss.on("listening", async () => {
  try {
    await schemaReady;
  } catch (err) {
    console.error("Database schema migration failed:", err);
    process.exit(1);
  }
  console.log(
    `WebSocket server listening on ${HOST}:${PORT}` +
      (AUTH_TOKEN ? " (token auth enabled)" : " (WARNING: auth disabled)"),
  );
});

wss.on("error", (err) => {
  console.error("Server error:", err);
  process.exit(1);
});

wss.on("connection", (ws, req) => {
  if (!isAuthorized(req)) {
    ws.close(1008, "Unauthorized");
    return;
  }

  ws.isAlive = true;
  ws.on("pong", () => {
    ws.isAlive = true;
  });

  ws.on("error", (err) => {
    console.error("WebSocket error:", err.message || err);
  });

  ws.on("message", async (data, isBinary) => {
    if (isBinary) return sendError(ws, "Binary frames are not supported");

    let message;
    try {
      message = JSON.parse(data.toString());
    } catch {
      return sendError(ws, "Invalid JSON");
    }
    if (!isPlainObject(message)) {
      return sendError(ws, "Message must be a JSON object");
    }

    try {
      if (message.action === undefined) {
        await handleMetrics(message); // no "action" key => metrics report
      } else {
        await handleAction(ws, message);
      }
    } catch (err) {
      if (err instanceof ValidationError) {
        sendError(ws, err.message);
      } else {
        console.error("Error handling message:", err);
        sendError(ws, "Internal server error");
      }
    }
  });
});

// Drop connections that stop answering pings (half-open TCP, crashed agents).
const heartbeat = setInterval(() => {
  for (const ws of wss.clients) {
    if (!ws.isAlive) {
      ws.terminate();
      continue;
    }
    ws.isAlive = false;
    ws.ping();
  }
}, HEARTBEAT_MS);
heartbeat.unref();

// ---------------------------------------------------------------------------
// Graceful shutdown
// ---------------------------------------------------------------------------
let shuttingDown = false;
async function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`${signal} received, shutting down...`);
  clearInterval(heartbeat);
  for (const ws of wss.clients) ws.close(1001, "Server shutting down");
  wss.close();
  try {
    await pool.end();
  } catch (err) {
    console.error("Error closing database pool:", err);
  }
  process.exit(0);
}
process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));

process.on("unhandledRejection", (reason) => {
  console.error("Unhandled rejection:", reason);
});
