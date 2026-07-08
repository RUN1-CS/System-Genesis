CREATE TABLE IF NOT EXISTS logs(
    hostname TEXT NOT NULL,
    time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ram_free REAL NOT NULL,
    ram_total REAL NOT NULL,
    swap_free REAL NOT NULL,
    swap_total REAL NOT NULL,
    cpu_cores JSON NOT NULL,
    disk_metrics JSON NOT NULL
);