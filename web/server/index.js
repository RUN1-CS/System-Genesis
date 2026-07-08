const WebSocket = require("ws"); // Import the 'ws' module for WebSocket server functionality
const pool = require("./db").pool; // Import the PostgreSQL connection pool from the 'db.js' module

// Create a new WebSocket server that listens on port 3000
const wss = new WebSocket.Server({ port: 3000 });

wss.on("connection", (ws) => {
  // Handle errors on the WebSocket connection
  ws.on("error", (err) => {
    console.error("WebSocket error:", err.message || err);
  });

  // Handle incoming messages from the WebSocket client
  ws.on("message", (message) => {
    if (!message) return;

    try {
      try {
        // Attempt to parse the incoming message as JSON, replacing single quotes with double quotes for valid JSON formatting
        const raw = message.toString();
        const jsonReady = raw.replace(/'/g, '"');
        const parsed = JSON.parse(jsonReady);
        // If the parsed message is an object and does not contain an 'action' key, treat it as log data to be inserted into the database
        if (parsed && typeof parsed === "object") {
          const keys = Object.keys(parsed);
          if (keys.length > 0 && keys[0] !== "action") {
            // Prepare the log data for insertion into the database
            const logData = {
              hostname: parsed.hostname,
              // ensure time is a proper JS Date (Postgres accepts JS Date as timestamp)
              time: parsed.time ? new Date(parsed.time) : new Date(),
              ram_free: parsed.ram.free,
              ram_total: parsed.ram.total,
              swap_free: parsed.swap.free,
              swap_total: parsed.swap.total,
              cpu_cores: JSON.stringify(parsed.cpu_cores),
              disk_metrics: JSON.stringify(parsed.disk_metrics),
            };

            // Insert logData into the database
            pool
              .query(
                "INSERT INTO logs (hostname, time, ram_free, ram_total, swap_free, swap_total, cpu_cores, disk_metrics) VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                [
                  logData.hostname,
                  logData.time,
                  logData.ram_free,
                  logData.ram_total,
                  logData.swap_free,
                  logData.swap_total,
                  logData.cpu_cores,
                  logData.disk_metrics,
                ],
              )
              .catch((err) => {
                console.error("Error inserting log data into database", err);
              });
            return;
          } else {
            switch (parsed.action) {
              // Handle the "get_logs" action to fetch logs from the database
              case "get_logs":
                let ammount = 1; // Default value
                if (parsed.ammount && typeof parsed.ammount === "number") {
                  ammount = parsed.ammount;
                }
                try {
                  pool
                    .query("SELECT * FROM logs ORDER BY time DESC LIMIT $1", [
                      ammount,
                    ])
                    .then((result) => {
                      ws.send(JSON.stringify(result.rows[0] || {}));
                    })
                    .catch((err) => {
                      console.error("Error fetching logs from database", err);
                    });
                } catch (err) {
                  console.error("Error in get_logs action:", err);
                }
                break;
              default:
                // Log a warning for unknown actions received from the WebSocket client
                console.warn("Unknown action:", parsed.action);
            }
          }
        }
      } catch (parseError) {
        // If parsing fails, log the error and the raw message for debugging purposes
        const raw = message.toString();
        const jsonReady = raw.replace(/'/g, '"');
        console.error(
          "Error parsing message:",
          parseError,
          "Raw message:",
          raw,
          "JSON-ready message:",
          jsonReady,
        );
        return;
      }
    } catch (e) {
      // Not JSON, ignore or handle as needed
    }
  });

  ws.on("close", () => {});
});
