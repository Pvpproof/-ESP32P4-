<?php
require_once __DIR__ . '/config.php';

ini_set('display_errors', '0');

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    json_response(['ok' => false, 'message' => 'method not allowed'], 405);
}

$raw = file_get_contents('php://input');
$data = json_decode($raw, true);
if (!$data) {
    json_response(['ok' => false, 'message' => 'invalid json'], 400);
}

$filename = trim($data['filename'] ?? '');
if ($filename === '' || preg_match('/[\\\/]/', $filename)) {
    json_response(['ok' => false, 'message' => 'invalid filename'], 400);
}

$filePath = UPLOAD_DIR . '/' . $filename;
if (!is_file($filePath)) {
    json_response(['ok' => false, 'message' => 'file not found'], 404);
}

$deleted = @unlink($filePath);
if (!$deleted) {
    json_response(['ok' => false, 'message' => 'delete failed'], 500);
}

json_response([
    'ok' => true,
    'filename' => $filename
]);
