const pg = require("pg"); // Import the 'pg' module for PostgreSQL database interaction

// Define database credentials, using environment variables if available, otherwise defaulting to specified values
const creds = {
  user: process.env.POSTGRES_USER || "sys_monitor",
  host: process.env.POSTGRES_HOST || "db",
  database: process.env.POSTGRES_DB || "sys_monitor",
  password: process.env.POSTGRES_PASSWORD || "sys_monitor",
  port: process.env.POSTGRES_PORT || 5432,
};

// Create a new PostgreSQL connection pool using the defined credentials
const pool = new pg.Pool({
  user: creds.user,
  host: creds.host,
  database: creds.database,
  password: creds.password,
  port: creds.port,
});

pool.on("connect", () => {});

// Handle unexpected errors on idle clients in the connection pool
pool.on("error", (err) => {
  console.error("Unexpected error on idle client", err);
  process.exit(-1);
});

module.exports = {
  pool,
};
