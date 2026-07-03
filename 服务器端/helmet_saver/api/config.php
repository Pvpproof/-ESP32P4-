<?php
date_default_timezone_set('Asia/Shanghai');

define('DATA_DIR', dirname(__DIR__) . '/data');
define('DB_PATH', DATA_DIR . '/helmet_monitor.sqlite');
define('UPLOAD_DIR', dirname(__DIR__) . '/uploads');
define('UPLOAD_URL_BASE', '/helmet_saver/uploads');

function json_response($data, $code = 200) {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function ensure_dir($dir) {
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }
}

function get_pdo() {
    static $pdo = null;
    if ($pdo === null) {
        ensure_dir(DATA_DIR);
        ensure_dir(UPLOAD_DIR);

        $pdo = new PDO('sqlite:' . DB_PATH, null, null, [
            PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        ]);

        $pdo->exec("CREATE TABLE IF NOT EXISTS helmet_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            event_type TEXT NOT NULL,
            labels_json TEXT,
            image_path TEXT NOT NULL,
            image_url TEXT NOT NULL,
            captured_at TEXT NOT NULL,
            snapshot_url TEXT NOT NULL,
            stream_url TEXT DEFAULT '',
            created_at TEXT NOT NULL
        )");

        $pdo->exec("CREATE INDEX IF NOT EXISTS idx_helmet_events_created_at ON helmet_events(created_at)");
        $pdo->exec("CREATE INDEX IF NOT EXISTS idx_helmet_events_captured_at ON helmet_events(captured_at)");
    }
    return $pdo;
}

function http_get_binary($url, $timeout = 3) {
    $ch = curl_init($url);
    curl_setopt_array($ch, [
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_FOLLOWLOCATION => true,
        CURLOPT_CONNECTTIMEOUT => $timeout,
        CURLOPT_TIMEOUT => $timeout,
        CURLOPT_SSL_VERIFYPEER => false,
        CURLOPT_SSL_VERIFYHOST => false,
        CURLOPT_USERAGENT => 'helmet-saver/1.0',
    ]);

    $data = curl_exec($ch);
    $err = curl_error($ch);
    $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);

    if ($data === false || $code >= 400) {
        return [false, $err ?: ('http status ' . $code)];
    }

    return [true, $data];
}
