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
if ($raw === false || strlen($raw) < 100) {
    http_response_code(400);
    echo json_encode([
        'ok' => false,
        'error' => 'empty or too small image body'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

if (substr($raw, 0, 2) !== "\xFF\xD8") {
    http_response_code(400);
    echo json_encode([
        'ok' => false,
        'error' => 'not a jpeg payload'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

$savePath = __DIR__ . '/latest_camera.jpg';
if (file_put_contents($savePath, $raw, LOCK_EX) === false) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'error' => 'write latest_camera.jpg failed'
    ], JSON_UNESCAPED_UNICODE);
    exit;
}

function build_thermal_snapshot_image(array $thermal, string $cameraPath, string $outPath): bool
{
    if (!file_exists($cameraPath)) {
        return false;
    }

    $img = @imagecreatefromstring(file_get_contents($cameraPath));
    if (!$img) {
        return false;
    }

    $imgW = imagesx($img);
    $imgH = imagesy($img);

    $hotX = isset($thermal['hot_x']) ? intval($thermal['hot_x']) : 0;
    $hotY = isset($thermal['hot_y']) ? intval($thermal['hot_y']) : 0;
    $hotW = isset($thermal['hot_w']) ? intval($thermal['hot_w']) : 32;
    $hotH = isset($thermal['hot_h']) ? intval($thermal['hot_h']) : 32;
    $tMax = isset($thermal['t_max']) ? floatval($thermal['t_max']) : 0.0;
    $tMin = isset($thermal['t_min']) ? floatval($thermal['t_min']) : 0.0;
    $tAvg = isset($thermal['t_avg']) ? floatval($thermal['t_avg']) : 0.0;

    $hotX = max(0, min($imgW - 1, $hotX));
    $hotY = max(0, min($imgH - 1, $hotY));
    $hotW = max(12, min($imgW - $hotX, $hotW));
    $hotH = max(12, min($imgH - $hotY, $hotH));

    $red = imagecolorallocate($img, 255, 48, 48);
    $yellow = imagecolorallocate($img, 255, 224, 64);
    $black = imagecolorallocate($img, 0, 0, 0);
    $white = imagecolorallocate($img, 255, 255, 255);

    imagesetthickness($img, 3);
    imagerectangle($img, $hotX, $hotY, $hotX + $hotW, $hotY + $hotH, $red);

    $label1 = 'HOT ' . number_format($tMax, 1) . 'C';
    $label2 = 'MIN ' . number_format($tMin, 1) . 'C  AVG ' . number_format($tAvg, 1) . 'C';

    $font = 5;
    $textPad = 6;
    $textH = imagefontheight($font);
    $textW1 = imagefontwidth($font) * strlen($label1);
    $textW2 = imagefontwidth($font) * strlen($label2);
    $boxW = max($textW1, $textW2) + $textPad * 2;
    $boxH = $textH * 2 + $textPad * 3;

    $boxX = $hotX;
    $boxY = $hotY - $boxH - 8;
    if ($boxY < 0) {
        $boxY = min($imgH - $boxH, $hotY + $hotH + 8);
    }
    if ($boxX + $boxW > $imgW) {
        $boxX = max(0, $imgW - $boxW);
    }

    imagefilledrectangle($img, $boxX, $boxY, $boxX + $boxW, $boxY + $boxH, $black);
    imagerectangle($img, $boxX, $boxY, $boxX + $boxW, $boxY + $boxH, $yellow);
    imagestring($img, $font, $boxX + $textPad, $boxY + $textPad, $label1, $yellow);
    imagestring($img, $font, $boxX + $textPad, $boxY + $textPad + $textH + 4, $label2, $white);

    $ok = imagejpeg($img, $outPath, 88);
    imagedestroy($img);
    return $ok;
}

$meta = [
    'ok' => true,
    'device' => $_GET['device'] ?? 'unknown',
    'sensor' => $_GET['sensor'] ?? 'unknown',
    'trigger' => $_GET['trigger'] ?? 'manual',
    't_max' => $_GET['t_max'] ?? null,
    't_min' => $_GET['t_min'] ?? null,
    't_avg' => $_GET['t_avg'] ?? null,
    'size' => strlen($raw),
    'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? '',
    'server_received_at' => date('Y-m-d H:i:s')
];

file_put_contents(__DIR__ . '/latest_camera.json', json_encode($meta, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);

$hotspotTriggered = false;
$hotspotResult = null;
$hotspotScript = __DIR__ . '/generate_hotspot.php';
if (file_exists($hotspotScript) && file_exists(__DIR__ . '/latest_thermal.json')) {
    ob_start();
    include $hotspotScript;
    $hotspotOutput = trim(ob_get_clean());
    $hotspotResult = json_decode($hotspotOutput, true);
    $hotspotTriggered = is_array($hotspotResult) && !empty($hotspotResult['ok']);
}

$thermalEventWritten = false;
$thermalEventImageUrl = '';
$thermalThreshold = 60.0;
$thermalDataPath = __DIR__ . '/latest_thermal.json';
if (file_exists($thermalDataPath)) {
    $thermalData = json_decode(file_get_contents($thermalDataPath), true);
    $tMax = isset($thermalData['t_max']) ? floatval($thermalData['t_max']) : null;

    if ($tMax !== null && $tMax >= $thermalThreshold) {
        $dataDir = __DIR__ . '/data';
        $eventFile = $dataDir . '/local-events.json';
        $snapshotDir = __DIR__ . '/uploads/thermal-events';

        if (!is_dir($dataDir)) {
            mkdir($dataDir, 0755, true);
        }
        if (!is_dir($snapshotDir)) {
            mkdir($snapshotDir, 0755, true);
        }

        $events = [];
        if (file_exists($eventFile)) {
            $decoded = json_decode(file_get_contents($eventFile), true);
            if (is_array($decoded)) {
                $events = $decoded;
            }
        }

        $eventId = 'thermal_' . time() . '_' . substr(md5((string)microtime(true)), 0, 6);
        $snapshotName = $eventId . '.jpg';
        $snapshotAbsPath = $snapshotDir . '/' . $snapshotName;
        $hotspotSource = __DIR__ . '/latest_hotspot.jpg';

        if ((is_file($hotspotSource) && @copy($hotspotSource, $snapshotAbsPath)) || build_thermal_snapshot_image($thermalData, $savePath, $snapshotAbsPath)) {
            $thermalEventImageUrl = '/uploads/thermal-events/' . $snapshotName;
        }

        $events[] = [
            'event_id' => $eventId,
            'captured_at' => date('Y-m-d H:i:s'),
            'event_type' => 'thermal_overheat',
            'labels' => ['温度异常'],
            'image_url' => $thermalEventImageUrl,
            'image_path' => $thermalEventImageUrl ? ltrim($thermalEventImageUrl, '/') : '',
            'meta' => [
                't_max' => $thermalData['t_max'] ?? null,
                't_min' => $thermalData['t_min'] ?? null,
                't_avg' => $thermalData['t_avg'] ?? null,
                'device' => $thermalData['device'] ?? '',
                'sensor' => $thermalData['sensor'] ?? '',
            ],
        ];

        $events = array_slice($events, -100);
        file_put_contents(
            $eventFile,
            json_encode($events, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES),
            LOCK_EX
        );
        $thermalEventWritten = true;
    }
}

http_response_code(200);
echo json_encode([
    'ok' => true,
    'saved' => 'latest_camera.jpg',
    'meta' => 'latest_camera.json',
    'size' => strlen($raw),
    'hotspot_triggered' => $hotspotTriggered,
    'hotspot_result' => $hotspotResult,
    'thermal_threshold' => $thermalThreshold,
    'thermal_event_written' => $thermalEventWritten,
    'thermal_event_image_url' => $thermalEventImageUrl,
], JSON_UNESCAPED_UNICODE);
