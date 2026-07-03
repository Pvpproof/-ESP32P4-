<?php
require_once __DIR__ . '/config.php';

ensure_data_files();

$body = read_json_body();
$deviceId = $body['device_id'] ?? DEVICE_ID;
$command = trim(strval($body['command'] ?? ''));

if ($deviceId !== DEVICE_ID) {
    json_response([
        'ok' => false,
        'message' => 'unknown device_id'
    ], 400);
}

$allow = ['set_plan', 'start', 'stop'];
if (!in_array($command, $allow, true)) {
    json_response([
        'ok' => false,
        'message' => 'bad command'
    ], 400);
}

$current = read_data_file(COMMAND_FILE, default_command());
$nextId = intval($current['command_id'] ?? 0) + 1;

$plan = default_command()['plan'];
$planBody = is_array($body['plan'] ?? null) ? $body['plan'] : null;
if (is_array($planBody)) {
    $plan = [
        'A' => !empty($planBody['A']),
        'B' => !empty($planBody['B']),
        'C' => !empty($planBody['C']),
        'D' => !empty($planBody['D']),
    ];
} else {
    $plan = is_array($current['plan'] ?? null) ? $current['plan'] : $plan;
}

$cmd = [
    'device_id' => DEVICE_ID,
    'token' => DEVICE_TOKEN,
    'has_command' => true,
    'command_id' => $nextId,
    'command' => $command,
    'plan' => $plan,
    'issued_at' => date('Y-m-d H:i:s'),
    'acked_at' => '',
    'consumed' => 0,
];

write_data_file(COMMAND_FILE, $cmd);

json_response([
    'ok' => true,
    'message' => 'command queued',
    'command_id' => $nextId,
    'command' => $command,
    'plan' => $plan,
]);
