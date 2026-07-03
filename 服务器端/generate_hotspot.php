<?php
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');

$thermalPath = __DIR__ . '/latest_thermal.json';
$cameraPath = __DIR__ . '/latest_camera.jpg';
$outPath = __DIR__ . '/latest_hotspot.jpg';
$outMetaPath = __DIR__ . '/latest_hotspot.json';

if (!file_exists($thermalPath)) {
    http_response_code(404);
    echo json_encode(['ok' => false, 'error' => 'latest_thermal.json not found'], JSON_UNESCAPED_UNICODE);
    exit;
}
if (!file_exists($cameraPath)) {
    http_response_code(404);
    echo json_encode(['ok' => false, 'error' => 'latest_camera.jpg not found'], JSON_UNESCAPED_UNICODE);
    exit;
}

$thermal = json_decode(file_get_contents($thermalPath), true);
if (!is_array($thermal)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid thermal json'], JSON_UNESCAPED_UNICODE);
    exit;
}

$img = @imagecreatefromstring(file_get_contents($cameraPath));
if (!$img) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid camera jpg'], JSON_UNESCAPED_UNICODE);
    exit;
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

if (!imagejpeg($img, $outPath, 88)) {
    imagedestroy($img);
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'write latest_hotspot.jpg failed'], JSON_UNESCAPED_UNICODE);
    exit;
}

$meta = [
    'ok' => true,
    'saved' => 'latest_hotspot.jpg',
    'hot_x' => $hotX,
    'hot_y' => $hotY,
    'hot_w' => $hotW,
    'hot_h' => $hotH,
    't_max' => $tMax,
    't_min' => $tMin,
    't_avg' => $tAvg,
    'server_generated_at' => date('Y-m-d H:i:s')
];
file_put_contents($outMetaPath, json_encode($meta, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);

imagedestroy($img);
echo json_encode($meta, JSON_UNESCAPED_UNICODE);
