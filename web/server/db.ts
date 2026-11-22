import { Pool } from "pg";

const port = Number(process.env.POSTGRES_PORT) || 5432;

export const pool = new Pool({
  user: process.env.POSTGRES_USER || "sys_monitor",
  host: process.env.POSTGRES_HOST || "db",
  database: process.env.POSTGRES_DB || "sys_monitor",
  password: process.env.POSTGRES_PASSWORD || "sys_monitor",
  port,
});

pool.on("error", (error: Error) => {
  console.error("Unexpected error on idle client", error);
  process.exitCode = 1;
});
