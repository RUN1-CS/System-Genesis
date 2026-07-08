<?php
// Index page for the web interface
?>

<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Index</title>
    <script src="./js/index.js" defer></script>
    <link rel="stylesheet" href="./styles/index.css">
</head>
<body>
    <main>
        <h1>System Monitor</h1>
        <div class="card">
            <p>Hostname: <span id="hostname">Pending...</span></p>
            <p>Time: <span id="time">Pending...</span></p>
            <p>RAM: <span id="ram">Pending...</span></p>
            <p>Swap: <span id="swap">Pending...</span></p>
            <p>CPU Cores: <table id="cpu-table"><tr><th>Core</th><th>Usage</th></tr><tr><td>0</td><td>Pending...</td></tr></table></p>
            <p>Disk Metrics: <table id="disk-table"><tr><th>Device</th><th>Usage</th></tr><tr><td>/dev/sda1</td><td>Pending...</td></tr></table></p>
        </div>
    </main>
</body>
</html>