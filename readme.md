# System Genesis

A lightweight, high-performance system monitoring tool written in C that tracks CPU, memory, disk, and process activity in real-time.

## Features

- **Real-time CPU Usage** - Monitor overall and per-core CPU utilization
- **Memory Tracking** - View RAM usage, swap, and available memory
- **Disk Monitoring** - Check disk space
- **Temperature Monitoring** - Read available Linux hardware and thermal sensors
- **Process Management** - List and monitor running processes
- **Low Overhead** - Minimal resource consumption, perfect for embedded systems
- **Fast Updates** - Efficient data collection and display

## Quick Start

```bash
cd daemon/
make
./sys_monitor
cd ../web
docker compose up --build
```

The web stack uses `sys_monitor` as a development database user, password, and
database name when the corresponding variables are not set. Override these
values in `web/.env` for local customization; set a strong `POSTGRES_PASSWORD`
before deploying anywhere beyond development.
Database schema migrations are applied automatically when the server starts.
The web server and dashboard are TypeScript; Docker compiles both during
`docker compose up --build`. To build them locally, run `npm ci && npm run build`
from `web/`.

## Requirements

- GCC or Clang
- Linux kernel 2.6+
- Standard C library
- Docker
- Docker Compose

## Project Structure

```
System Genesis
├── daemon
│   ├── Makefile
│   └── sys_monitor.c
├── LICENSE
├── readme.md
└── web
    ├── compose.yml
    ├── package.json
    ├── tsconfig.client.json
    ├── tsconfig.server.json
    ├── nginx
    │   ├── Dockerfile
    │   └── www
    │       ├── index.html
    │       ├── js
    │       │   └── index.ts
    │       └── styles
    │           └── index.css
    ├── server
    │   ├── db.ts
    │   ├── Dockerfile
    │   ├── index.ts
    │   └── migrations
    │       └── 001_add_temperature_metrics.sql
    └── sql
        └── init.sql
```

## Performance

- Optimized for minimal CPU usage
- Efficient memory footprint
- Fast system call execution

## License

MIT License
