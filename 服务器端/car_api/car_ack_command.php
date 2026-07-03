<?php
require_once __DIR__ . '/config.php';

ensure_data_files();

$body = read_json_body();
$deviceId = $body['device_id'] ?? '';
$token = $body['token'] ?? '';
$commandId = intval($body['command_id'] ?? 0);

require_device_auth($deviceId, $token);

$cmd = read_data_file(COMMAND_FILE, default_command());
if ($commandId > 0 && intval($cmd['command_id'] ?? 0) === $commandId) {
    $cmd['consumed'] = 1;
    $cmd['acked_at'] = date('Y-m-d H:i:s');
    write_data_file(COMMAND_FILE, $cmd);
}

json_response([
    'ok' => true,
    'message' => 'command acked'
]);
