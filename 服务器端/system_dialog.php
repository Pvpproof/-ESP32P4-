<?php
date_default_timezone_set('Asia/Shanghai');
header('Content-Type: application/json; charset=utf-8');

$token = 'jarvis_dialog_token_001';
$dataDir = __DIR__ . '/data';
$dialogFile = $dataDir . '/system-dialog.json';
$maxItems = 200;

if (!is_dir($dataDir)) {
    mkdir($dataDir, 0755, true);
}
if (!file_exists($dialogFile)) {
    file_put_contents($dialogFile, json_encode([], JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
}

function read_dialog($file) {
    $raw = @file_get_contents($file);
    $data = json_decode($raw ?: '[]', true);
    return is_array($data) ? $data : [];
}

function write_dialog($file, $items) {
    file_put_contents($file, json_encode($items, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);
}

function append_dialog_item($file, $role, $text, $source = 'web', $meta = []) {
    global $maxItems;
    $items = read_dialog($file);
    $item = [
        'id' => uniqid('dlg_', true),
        'role' => $role,
        'text' => $text,
        'source' => $source,
        'meta' => $meta,
        'created_at' => date('Y-m-d H:i:s'),
        'timestamp' => time()
    ];
    $items[] = $item;
    if (count($items) > $maxItems) {
        $items = array_slice($items, -$maxItems);
    }
    write_dialog($file, $items);
    return $item;
}

$action = $_GET['action'] ?? ($_POST['action'] ?? 'list');

if ($action === 'append') {
    $reqToken = $_GET['token'] ?? ($_POST['token'] ?? '');
    if ($reqToken !== $token) {
        http_response_code(403);
        echo json_encode(['ok' => false, 'error' => 'invalid token'], JSON_UNESCAPED_UNICODE);
        exit;
    }

    $raw = file_get_contents('php://input');
    $json = json_decode($raw ?: 'null', true);
    $payload = is_array($json) ? $json : $_POST;

    $role = trim((string)($payload['role'] ?? 'user'));
    $text = trim((string)($payload['text'] ?? ''));
    $source = trim((string)($payload['source'] ?? 'web'));
    $meta = is_array($payload['meta'] ?? null) ? $payload['meta'] : [];

    if ($text === '') {
        http_response_code(400);
        echo json_encode(['ok' => false, 'error' => 'text is required'], JSON_UNESCAPED_UNICODE);
        exit;
    }

    $item = append_dialog_item($dialogFile, $role, $text, $source, $meta);
    echo json_encode(['ok' => true, 'item' => $item], JSON_UNESCAPED_UNICODE);
    exit;
}

if ($action === 'clear') {
    $reqToken = $_GET['token'] ?? ($_POST['token'] ?? '');
    if ($reqToken !== $token) {
        http_response_code(403);
        echo json_encode(['ok' => false, 'error' => 'invalid token'], JSON_UNESCAPED_UNICODE);
        exit;
    }
    write_dialog($dialogFile, []);
    echo json_encode(['ok' => true], JSON_UNESCAPED_UNICODE);
    exit;
}

$items = read_dialog($dialogFile);
$limit = intval($_GET['limit'] ?? 100);
if ($limit <= 0 || $limit > 200) {
    $limit = 100;
}
$items = array_slice($items, -$limit);

echo json_encode([
    'ok' => true,
    'count' => count($items),
    'items' => $items,
    'server_time' => date('Y-m-d H:i:s')
], JSON_UNESCAPED_UNICODE);
