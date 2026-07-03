<?php
require_once __DIR__ . '/config.php';

ensure_data_files();

$deviceId = $_GET['device_id'] ?? '';
$token = $_GET['token'] ?? '';

require_device_auth($deviceId, $token);

$cmd = read_data_file(COMMAND_FILE, default_command());
$hasCommand = !empty($cmd['has_command']) && empty($cmd['consumed']);

if (!$hasCommand) {
    json_response([
        'ok' => true,
        'has_command' => false,
        'server_time' => date('Y-m-d H:i:s')
    ]);
}

json_response([
    'ok' => true,
    'has_command' => true,
    'command_id' => intval($cmd['command_id'] ?? 0),
    'command' => strval($cmd['command'] ?? ''),
    'plan' => is_array($cmd['plan'] ?? null) ? $cmd['plan'] : default_command()['plan'],
    'issued_at' => strval($cmd['issued_at'] ?? ''),
    'server_time' => date('Y-m-d H:i:s')
]);
