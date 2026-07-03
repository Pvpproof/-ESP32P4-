<?php
require_once __DIR__ . '/config.php';

ensure_data_files();

$status = read_data_file(STATUS_FILE, default_status());
$command = read_data_file(COMMAND_FILE, default_command());

json_response([
    'ok' => true,
    'online' => is_device_online($status),
    'status' => $status,
    'pending_command' => [
        'has_command' => !empty($command['has_command']) && empty($command['consumed']),
        'command_id' => intval($command['command_id'] ?? 0),
        'command' => strval($command['command'] ?? ''),
        'issued_at' => strval($command['issued_at'] ?? ''),
    ]
]);
