<?php
require_once __DIR__ . '/config.php';

$limit = intval($_GET['limit'] ?? 50);
if ($limit <= 0) $limit = 50;
if ($limit > 200) $limit = 200;

$items = [];

if (is_dir(UPLOAD_DIR)) {
    $files = array_filter(scandir(UPLOAD_DIR), function ($name) {
        return $name !== '.' && $name !== '..' && preg_match('/\.(jpg|jpeg|png)$/i', $name);
    });

    foreach ($files as $fileName) {
        $filePath = UPLOAD_DIR . '/' . $fileName;
        if (!is_file($filePath)) continue;

        $mtime = filemtime($filePath) ?: time();
        $items[] = [
            'id' => md5($filePath),
            'filename' => $fileName,
            'captured_at' => date('Y-m-d H:i:s', $mtime),
            'event_type' => 'no_helmet',
            'labels' => ['No-Helmet'],
            'image_url' => UPLOAD_URL_BASE . '/' . $fileName,
            'image_path' => $filePath,
        ];
    }
}

usort($items, function ($a, $b) {
    return strcmp($b['captured_at'], $a['captured_at']);
});

$items = array_slice($items, 0, $limit);

json_response([
    'ok' => true,
    'count' => count($items),
    'items' => $items,
]);
