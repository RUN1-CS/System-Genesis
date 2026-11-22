import { createHash, timingSafeEqual } from "node:crypto";
import { promises as fs } from "node:fs";
import path from "node:path";
import type { IncomingMessage } from "http";
import { PoolClient } from "pg";
import WebSocket, { RawData, WebSocketServer } from "ws";
import { pool } from "./db";

const PORT = Number(process.env.PORT) || 3000;
const HOST = process.env.HOST || "0.0.0.0";
const AUTH_TOKEN = process.env.SYSMON_TOKEN || "";
const MAX_PAYLOAD_BYTES = 64 * 1024;
const MAX_LOGS_PER_REQUEST = 1000;
const MAX_CORES = 1024;
const MAX_DISKS = 256;
const MAX_TEMPERATURES = 256;
const HEARTBEAT_MS = 30_000;

type JsonObject = Record<string, unknown>;

interface ValidatedMetrics {
  hostname: string;
  time: Date;
  ram_free: number;
  ram_total: number;
  swap_free: number;
  swap_total: number;
  cpu_cores: string;
  disk_metrics: string;
  temperature_metrics: string;
}

class ValidationError extends Error {}

const isNonNegNumber = (value: unknown): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;

function isPlainObject(value: unknown): value is JsonObject {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function requireNumber(value: unknown, name: string): number {
  if (!isNonNegNumber(value)) throw new ValidationError(`Invalid ${name}`);
  return value;
}

function safeEqual(a: string, b: string): boolean {
  const hashA = createHash("sha256").update(a).digest();
  const hashB = createHash("sha256").update(b).digest();
  return timingSafeEqual(hashA, hashB);
}

function isAuthorized(request: IncomingMessage): boolean {
  if (!AUTH_TOKEN) return true;

  let supplied = "";
  try {
    supplied =
      new URL(request.url ?? "/", "http://localhost").searchParams.get(
        "token",
      ) ?? "";
  } catch {
    // Malformed URLs are unauthorized.
  }
  if (!supplied) {
    supplied = (request.headers.authorization ?? "").replace(/^Bearer\s+/i, "");
  }
  return supplied !== "" && safeEqual(supplied, AUTH_TOKEN);
}

function send(socket: WebSocket, payload: unknown): void {
  if (socket.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(payload));
  }
}

function sendError(socket: WebSocket, message: string): void {
  send(socket, { error: message });
}

function validateMetrics(message: JsonObject): ValidatedMetrics {
  if (
    typeof message.hostname !== "string" ||
    message.hostname.length === 0 ||
    message.hostname.length > 255
  ) {
    throw new ValidationError("Invalid hostname");
  }
  if (!isPlainObject(message.ram) || !isPlainObject(message.swap)) {
    throw new ValidationError("Missing ram/swap");
  }
  if (
    !Array.isArray(message.cpu_cores) ||
    message.cpu_cores.length > MAX_CORES ||
    !message.cpu_cores.every(isNonNegNumber)
  ) {
    throw new ValidationError("Invalid cpu_cores");
  }
  if (!isPlainObject(message.disk_metrics)) {
    throw new ValidationError("Invalid disk_metrics");
  }

  const disks = Object.values(message.disk_metrics);
  if (
    disks.length > MAX_DISKS ||
    !disks.every(
      (disk) =>
        isPlainObject(disk) &&
        isNonNegNumber(disk.free) &&
        isNonNegNumber(disk.total),
    )
  ) {
    throw new ValidationError("Invalid disk_metrics entry");
  }

  const temperatures = message.temperature_metrics ?? [];
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

  const time = new Date(
    typeof message.time === "string" || typeof message.time === "number"
      ? message.time
      : Number.NaN,
  );
  if (Number.isNaN(time.getTime())) time.setTime(Date.now());

  return {
    hostname: message.hostname,
    time,
    ram_free: requireNumber(message.ram.free, "ram.free"),
    ram_total: requireNumber(message.ram.total, "ram.total"),
    swap_free: requireNumber(message.swap.free, "swap.free"),
    swap_total: requireNumber(message.swap.total, "swap.total"),
    cpu_cores: JSON.stringify(message.cpu_cores),
    disk_metrics: JSON.stringify(message.disk_metrics),
    temperature_metrics: JSON.stringify(temperatures),
  };
}

async function handleMetrics(message: JsonObject): Promise<void> {
  const log = validateMetrics(message);
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

async function handleGetLogs(
  socket: WebSocket,
  message: JsonObject,
): Promise<void> {
  const requested = message.amount ?? message.ammount ?? 1;
  const limit = Math.min(
    Math.max(Math.trunc(Number(requested)) || 1, 1),
    MAX_LOGS_PER_REQUEST,
  );

  const params: (string | number)[] = [];
  let where = "";
  if (message.hostname !== undefined) {
    if (typeof message.hostname !== "string") {
      throw new ValidationError("Invalid hostname filter");
    }
    params.push(message.hostname);
    where = `WHERE hostname = $${params.length}`;
  }
  params.push(limit);

  const { rows } = await pool.query(
    `SELECT * FROM logs ${where} ORDER BY time DESC LIMIT $${params.length}`,
    params,
  );
  send(socket, limit === 1 ? (rows[0] ?? {}) : rows);
}

async function runMigrations(): Promise<void> {
  const client: PoolClient = await pool.connect();
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
    const files = (await fs.readdir(directory))
      .filter((filename) => /^\d+_.+\.sql$/.test(filename))
      .sort();

    for (const filename of files) {
      const { rows } = await client.query(
        "SELECT 1 FROM schema_migrations WHERE filename = $1",
        [filename],
      );
      if (rows.length > 0) continue;

      const sql = await fs.readFile(path.join(directory, filename), "utf8");
      await client.query(sql);
      await client.query(
        "INSERT INTO schema_migrations (filename) VALUES ($1)",
        [filename],
      );
    }

    await client.query("COMMIT");
  } catch (error) {
    try {
      await client.query("ROLLBACK");
    } catch (rollbackError) {
      console.error("Database migration rollback failed:", rollbackError);
    }
    throw error;
  } finally {
    client.release();
  }
}

async function startServer(): Promise<void> {
  try {
    await runMigrations();
  } catch (error) {
    console.error("Database schema migration failed:", error);
    await pool.end();
    process.exitCode = 1;
    return;
  }

  const server = new WebSocketServer({
    port: PORT,
    host: HOST,
    maxPayload: MAX_PAYLOAD_BYTES,
  });
  const socketAlive = new WeakMap<WebSocket, boolean>();

  server.on("listening", () => {
    console.log(
      `WebSocket server listening on ${HOST}:${PORT}` +
        (AUTH_TOKEN ? " (token auth enabled)" : " (WARNING: auth disabled)"),
    );
  });
  server.on("error", (error: Error) => {
    console.error("Server error:", error);
    process.exit(1);
  });
  server.on("connection", (socket, request) => {
    if (!isAuthorized(request)) {
      socket.close(1008, "Unauthorized");
      return;
    }

    socketAlive.set(socket, true);
    socket.on("pong", () => socketAlive.set(socket, true));
    socket.on("error", (error: Error) => {
      console.error("WebSocket error:", error.message || error);
    });
    socket.on("message", async (data: RawData, isBinary: boolean) => {
      if (isBinary) {
        sendError(socket, "Binary frames are not supported");
        return;
      }

      let message: unknown;
      try {
        message = JSON.parse(data.toString());
      } catch {
        sendError(socket, "Invalid JSON");
        return;
      }
      if (!isPlainObject(message)) {
        sendError(socket, "Message must be a JSON object");
        return;
      }

      try {
        if (message.action === undefined) {
          await handleMetrics(message);
        } else if (message.action === "get_logs") {
          await handleGetLogs(socket, message);
        } else {
          throw new ValidationError("Unknown action");
        }
      } catch (error) {
        if (error instanceof ValidationError) {
          sendError(socket, error.message);
        } else {
          console.error("Error handling message:", error);
          sendError(socket, "Internal server error");
        }
      }
    });
  });

  const heartbeat = setInterval(() => {
    for (const socket of server.clients) {
      if (!socketAlive.get(socket)) {
        socket.terminate();
        continue;
      }
      socketAlive.set(socket, false);
      socket.ping();
    }
  }, HEARTBEAT_MS);
  heartbeat.unref();

  let shuttingDown = false;
  const shutdown = async (signal: NodeJS.Signals): Promise<void> => {
    if (shuttingDown) return;
    shuttingDown = true;
    console.log(`${signal} received, shutting down...`);
    clearInterval(heartbeat);
    for (const socket of server.clients) {
      socket.close(1001, "Server shutting down");
    }
    await new Promise<void>((resolve) => server.close(() => resolve()));
    try {
      await pool.end();
    } catch (error) {
      console.error("Error closing database pool:", error);
      process.exitCode = 1;
    }
  };

  process.on("SIGINT", () => void shutdown("SIGINT"));
  process.on("SIGTERM", () => void shutdown("SIGTERM"));
}

void startServer();

process.on("unhandledRejection", (reason: unknown) => {
  console.error("Unhandled rejection:", reason);
});
