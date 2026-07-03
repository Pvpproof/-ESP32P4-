<?php
header('Content-Type: application/json; charset=utf-8');

$file = __DIR__ . '/data/local-events.json';
$items = [];

if (file_exists($file)) {
    $decoded = json_decode(file_get_contents($file), true);
    if (is_array($decoded)) {
        $items = $decoded;
    }
}

usort($items, function ($a, $b) {
    return strcmp((string)($b['captured_at'] ?? ''), (string)($a['captured_at'] ?? ''));
});

echo json_encode([
    'ok' => true,
    'items' => $items,
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
