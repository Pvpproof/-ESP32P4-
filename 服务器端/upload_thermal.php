<?php
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode([
        'ok' => false,
        'error' => 'method not allowed'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

$raw = file_get_contents('php://input');
if ($raw === false || trim($raw) === '') {
    http_response_code(400);
    echo json_encode([
        'ok' => false,
        'error' => 'empty body'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

$data = json_decode($raw, true);
if (!is_array($data)) {
    http_response_code(400);
    echo json_encode([
        'ok' => false,
        'error' => 'invalid json'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

$data['server_received_at'] = date('Y-m-d H:i:s');
$data['remote_addr'] = $_SERVER['REMOTE_ADDR'] ?? '';

$savePath = __DIR__ . '/latest_thermal.json';
$json = json_encode($data, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
if ($json === false) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'error' => 'json encode failed'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

if (file_put_contents($savePath, $json, LOCK_EX) === false) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'error' => 'write latest_thermal.json failed'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

http_response_code(200);
echo json_encode([
    'ok' => true,
    'saved' => 'latest_thermal.json',
    'server_received_at' => $data['server_received_at'],
    'hotspot_triggered' => false,
    'hotspot_result' => null,
], JSON_UNESCAPED_UNICODE);
