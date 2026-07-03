<?php
date_default_timezone_set('Asia/Shanghai');

header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Headers: Content-Type');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');

if (($_SERVER['REQUEST_METHOD'] ?? '') === 'OPTIONS') {
    http_response_code(204);
    exit;
}

define('DEVICE_ID', 'smartcar-p4-01');
define('DEVICE_TOKEN', 'smartcar_p4_cloud_001');

define('STATUS_FILE', __DIR__ . '/status.json');
define('COMMAND_FILE', __DIR__ . '/command.json');

define('ONLINE_TIMEOUT_SECONDS', 8);

function json_response($data, $code = 200)
{
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRETTY_PRINT);
    exit;
}

function read_json_body()
{
    $raw = file_get_contents('php://input');
    if (!$raw) {
        return [];
    }
    $data = json_decode($raw, true);
    return is_array($data) ? $data : [];
}

function require_device_auth($deviceId, $token)
{
    if ($deviceId !== DEVICE_ID || $token !== DEVICE_TOKEN) {
        json_response([
            'ok' => false,
            'message' => 'device auth failed'
        ], 403);
    }
}

function default_status()
{
    return [
        'device_id' => DEVICE_ID,
        'last_seen' => '',
        'ip' => '',
        'wifi_connected' => 0,
        'mode' => 'IDLE',
        'plan_running' => 0,
        'route_progress' => 0,
        'route_task_count' => 5,
        'active_task' => 'NONE',
        'line_seen' => 0,
        'line_error' => 0,
        't_node_seen' => 0,
        'uptime_ms' => 0,
        'cloud_push_ok' => 0,
        'cloud_pull_ok' => 0,
        'raw' => []
    ];
}

function default_command()
{
    return [
        'device_id' => DEVICE_ID,
        'token' => DEVICE_TOKEN,
        'has_command' => false,
        'command_id' => 0,
        'command' => '',
        'plan' => [
            'A' => true,
            'B' => true,
            'C' => true,
            'D' => true,
        ],
        'issued_at' => '',
        'acked_at' => '',
        'consumed' => 1
    ];
}

function ensure_data_files()
{
    if (!is_dir(__DIR__)) {
        mkdir(__DIR__, 0777, true);
    }

    if (!file_exists(STATUS_FILE)) {
        file_put_contents(STATUS_FILE, json_encode(default_status(), JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
    }
    if (!file_exists(COMMAND_FILE)) {
        file_put_contents(COMMAND_FILE, json_encode(default_command(), JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
    }
}

function read_data_file($path, $default)
{
    if (!file_exists($path)) {
        return $default;
    }
    $content = file_get_contents($path);
    $data = json_decode($content, true);
    return is_array($data) ? $data : $default;
}

function write_data_file($path, $data)
{
    file_put_contents($path, json_encode($data, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), LOCK_EX);
}

function is_device_online($status)
{
    if (empty($status['last_seen'])) {
        return false;
    }
    $lastSeen = strtotime($status['last_seen']);
    return $lastSeen && (time() - $lastSeen <= ONLINE_TIMEOUT_SECONDS);
}
