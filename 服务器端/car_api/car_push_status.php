<?php
require_once __DIR__ . '/config.php';

ensure_data_files();

$body = read_json_body();
$deviceId = $body['device_id'] ?? '';
$token = $body['token'] ?? '';

require_device_auth($deviceId, $token);

$status = default_status();
$status['device_id'] = DEVICE_ID;
$status['last_seen'] = date('Y-m-d H:i:s');
$status['ip'] = strval($body['ip'] ?? '');
$status['wifi_connected'] = !empty($body['wifi_connected']) ? 1 : 0;
$status['mode'] = strval($body['mode'] ?? 'IDLE');
$status['plan_running'] = !empty($body['plan_running']) ? 1 : 0;
$status['route_progress'] = intval($body['route_progress'] ?? 0);
$status['route_task_count'] = intval($body['route_task_count'] ?? 5);
$status['active_task'] = strval($body['active_task'] ?? 'NONE');
$status['line_seen'] = !empty($body['line_seen']) ? 1 : 0;
$status['line_error'] = intval($body['line_error'] ?? 0);
$status['t_node_seen'] = !empty($body['t_node_seen']) ? 1 : 0;
$status['uptime_ms'] = intval($body['uptime_ms'] ?? 0);
$status['cloud_push_ok'] = 1;
$status['cloud_pull_ok'] = !empty($body['cloud_pull_ok']) ? 1 : 0;
$status['raw'] = $body;

write_data_file(STATUS_FILE, $status);

json_response([
    'ok' => true,
    'message' => 'status saved',
    'server_time' => date('Y-m-d H:i:s')
]);
