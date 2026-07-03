<?php
require_once __DIR__ . '/config.php';

$raw = file_get_contents('php://input');
$data = json_decode($raw, true);

if (!$data) {
    json_response(['ok' => false, 'message' => 'invalid json'], 400);
}

$device_id = trim($data['device_id'] ?? '');
$event_type = trim($data['event_type'] ?? '');
$captured_at = trim($data['captured_at'] ?? '');
$labels = $data['labels'] ?? [];
$snapshot_url = trim($data['snapshot_url'] ?? '');
$stream_url = trim($data['stream_url'] ?? '');

if ($device_id === '' || $event_type === '' || $captured_at === '' || $snapshot_url === '') {
    json_response([
        'ok' => false,
        'message' => 'missing required fields'
    ], 400);
}

if (!is_array($labels)) {
    $labels = [$labels];
}

$saveDir = UPLOAD_DIR;
ensure_dir($saveDir);

list($ok, $imgContent) = http_get_binary($snapshot_url, 6);
if (!$ok) {
    json_response([
        'ok' => false,
        'message' => 'failed to fetch snapshot',
        'snapshot_url' => $snapshot_url,
        'error' => $imgContent
    ], 500);
}

if (strlen($imgContent) < 100) {
    json_response([
        'ok' => false,
        'message' => 'snapshot content too small'
    ], 500);
}

$safeDevice = preg_replace('/[^a-zA-Z0-9_-]/', '_', $device_id);
$ts = date('Ymd_His') . '_' . substr((string)microtime(true), -6);
$fileName = $ts . '_' . $safeDevice . '.jpg';
$filePath = $saveDir . '/' . $fileName;

$saveOk = file_put_contents($filePath, $imgContent);
if ($saveOk === false) {
    json_response([
        'ok' => false,
        'message' => 'failed to save image'
    ], 500);
}

$imageUrl = UPLOAD_URL_BASE . '/' . $fileName;
$createdAt = date('Y-m-d H:i:s');

try {
    $pdo = get_pdo();
    $stmt = $pdo->prepare("
        INSERT INTO helmet_events
        (device_id, event_type, labels_json, image_path, image_url, captured_at, snapshot_url, stream_url, created_at)
        VALUES
        (:device_id, :event_type, :labels_json, :image_path, :image_url, :captured_at, :snapshot_url, :stream_url, :created_at)
    ");

    $stmt->execute([
        ':device_id' => $device_id,
        ':event_type' => $event_type,
        ':labels_json' => json_encode($labels, JSON_UNESCAPED_UNICODE),
        ':image_path' => $filePath,
        ':image_url' => $imageUrl,
        ':captured_at' => $captured_at,
        ':snapshot_url' => $snapshot_url,
        ':stream_url' => $stream_url,
        ':created_at' => $createdAt,
    ]);

    $eventId = $pdo->lastInsertId();

    json_response([
        'ok' => true,
        'event_id' => (int)$eventId,
        'image_url' => $imageUrl,
        'captured_at' => $captured_at
    ]);
} catch (Exception $e) {
    json_response([
        'ok' => false,
        'message' => 'db insert failed',
        'error' => $e->getMessage()
    ], 500);
}
