<?php
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

date_default_timezone_set('Asia/Shanghai');

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'OPTIONS') {
    http_response_code(204);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') !== 'POST') {
    http_response_code(405);
    echo json_encode([
        'ok' => false,
        'error' => 'method not allowed',
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function read_json_payload() {
    $raw = file_get_contents('php://input');
    $payload = json_decode($raw ?: 'null', true);
    return is_array($payload) ? $payload : [];
}

function read_json_file_map($file) {
    if (!file_exists($file)) {
        return [];
    }
    $decoded = json_decode(file_get_contents($file), true);
    return is_array($decoded) ? $decoded : [];
}

function write_json_file_map($file, $data) {
    file_put_contents($file, json_encode($data, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES), LOCK_EX);
}

function build_analysis_prompt($item) {
    $eventType = (string)($item['event_type'] ?? 'unknown');
    $labels = is_array($item['labels'] ?? null) ? implode('、', array_map('strval', $item['labels'])) : '';
    $meta = is_array($item['meta'] ?? null) ? $item['meta'] : [];

    $contextParts = [];
    if ($eventType !== '') {
        $contextParts[] = '事件类型：' . $eventType;
    }
    if ($labels !== '') {
        $contextParts[] = '已有标签：' . $labels;
    }
    if (isset($meta['t_max'])) {
        $contextParts[] = '最高温：' . $meta['t_max'] . '°C';
    }
    if (isset($meta['t_avg'])) {
        $contextParts[] = '平均温：' . $meta['t_avg'] . '°C';
    }

    $contextText = $contextParts ? ('已知上下文：' . implode('；', $contextParts) . '。') : '已知上下文较少，请严格基于图片可见内容判断。';

    return "你是施工安全巡检视觉分析助手。请根据图片内容判断是否存在安全隐患、违规操作或危险状态。" .
        $contextText .
        "只允许输出一个 JSON 对象，不要输出 markdown，不要输出解释，不要输出代码块。" .
        "JSON 格式固定为：" .
        '{"has_violation":true,"violation_types":[],"hazards":[],"risk_level":"low|medium|high","summary":"","advice":[]}' .
        "规则：1. 必须基于图片可见内容和已知上下文判断，不要编造看不见的事实。2. 没把握时可以使用“疑似”。3. violation_types、hazards、advice 必须是数组。4. summary 必须是简洁中文结论。";
}

function call_vision_api($config, $model, $imageUrl, $prompt) {
    $payload = [
        'model' => $model,
        'messages' => [
            [
                'role' => 'system',
                'content' => '你是施工安全巡检视觉分析助手。必须基于图片内容回答。只允许输出一个 JSON 对象，不要输出 markdown，不要输出解释，不要输出代码块。',
            ],
            [
                'role' => 'user',
                'content' => [
                    [
                        'type' => 'text',
                        'text' => $prompt,
                    ],
                    [
                        'type' => 'image_url',
                        'image_url' => [
                            'url' => $imageUrl,
                        ],
                    ],
                ],
            ],
        ],
        'temperature' => 0.1,
        'max_tokens' => max(300, (int)($config['max_output_tokens'] ?? 800)),
        'response_format' => ['type' => 'json_object'],
    ];

    $headers = [
        'Accept: application/json',
        'Content-Type: application/json',
        'Authorization: Bearer ' . $config['api_key'],
    ];

    $options = [
        'http' => [
            'method' => 'POST',
            'timeout' => max(20, (int)($config['timeout'] ?? 60)),
            'ignore_errors' => true,
            'header' => implode("\r\n", $headers),
            'content' => json_encode($payload, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES),
        ],
    ];

    $context = stream_context_create($options);
    $response = @file_get_contents($config['base_url'], false, $context);
    $statusCode = 0;
    if (!empty($http_response_header[0]) && preg_match('#HTTP/\\S+\\s+(\\d{3})#', $http_response_header[0], $matches)) {
        $statusCode = intval($matches[1]);
    }

    $decoded = json_decode($response === false ? '' : $response, true);
    $reply = '';
    if (is_array($decoded)) {
        $reply = trim((string)($decoded['choices'][0]['message']['content'] ?? ''));
    }

    return [
        'status_code' => $statusCode,
        'body' => $response === false ? '' : $response,
        'json' => $decoded,
        'reply' => $reply,
        'parsed' => json_decode($reply, true),
    ];
}

function normalize_analysis_result($parsed) {
    if (!is_array($parsed)) {
        return null;
    }

    $riskLevel = trim((string)($parsed['risk_level'] ?? 'medium'));
    if (!in_array($riskLevel, ['low', 'medium', 'high'], true)) {
        $riskLevel = 'medium';
    }

    $toArray = function ($value) {
        if (is_array($value)) {
            return array_values(array_filter(array_map('strval', $value), function ($item) {
                return trim($item) !== '';
            }));
        }
        if (is_string($value) && trim($value) !== '') {
            return [trim($value)];
        }
        return [];
    };

    return [
        'has_violation' => !empty($parsed['has_violation']),
        'violation_types' => $toArray($parsed['violation_types'] ?? []),
        'hazards' => $toArray($parsed['hazards'] ?? []),
        'risk_level' => $riskLevel,
        'summary' => trim((string)($parsed['summary'] ?? '')),
        'advice' => $toArray($parsed['advice'] ?? []),
    ];
}

$payload = read_json_payload();
$eventId = trim((string)($payload['event_id'] ?? ''));
$imageUrl = trim((string)($payload['image_url'] ?? ''));
$force = !empty($payload['force']);

if ($eventId === '' || $imageUrl === '') {
    http_response_code(400);
    echo json_encode([
        'ok' => false,
        'error' => 'event_id and image_url are required',
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

$configFile = __DIR__ . '/ai_config.php';
if (!file_exists($configFile)) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'error' => 'ai_config.php not found',
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

$config = require $configFile;
if (!is_array($config) || empty($config['base_url']) || empty($config['api_key']) || empty($config['model'])) {
    http_response_code(500);
    echo json_encode([
        'ok' => false,
        'error' => 'ai_config.php is invalid',
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

$dataDir = __DIR__ . '/data';
$analysisFile = $dataDir . '/event-analysis.json';
$debugFile = $dataDir . '/ai-vision-debug.log';
if (!is_dir($dataDir)) {
    mkdir($dataDir, 0755, true);
}

$analysisMap = read_json_file_map($analysisFile);
$existing = $analysisMap[$eventId] ?? null;
if (!$force && is_array($existing) && ($existing['analysis_status'] ?? '') === 'done' && !empty($existing['analysis_result'])) {
    echo json_encode([
        'ok' => true,
        'cached' => true,
        'event_id' => $eventId,
        'analysis_status' => 'done',
        'analysis_updated_at' => $existing['analysis_updated_at'] ?? null,
        'analysis_error' => (string)($existing['analysis_error'] ?? ''),
        'analysis_result' => $existing['analysis_result'],
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

$item = [
    'event_id' => $eventId,
    'event_type' => (string)($payload['event_type'] ?? 'unknown'),
    'labels' => is_array($payload['labels'] ?? null) ? $payload['labels'] : [],
    'meta' => is_array($payload['meta'] ?? null) ? $payload['meta'] : [],
];
$prompt = build_analysis_prompt($item);

$analysisMap[$eventId] = [
    'event_id' => $eventId,
    'analysis_status' => 'processing',
    'analysis_updated_at' => date('Y-m-d H:i:s'),
    'analysis_error' => '',
    'analysis_result' => null,
    'image_url' => $imageUrl,
];
write_json_file_map($analysisFile, $analysisMap);

$result = call_vision_api($config, (string)$config['model'], $imageUrl, $prompt);
$normalized = normalize_analysis_result($result['parsed']);

$debugLine = '[' . date('Y-m-d H:i:s') . '] ' . json_encode([
    'event_id' => $eventId,
    'image_url' => $imageUrl,
    'status_code' => $result['status_code'],
    'reply' => $result['reply'],
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) . PHP_EOL;
@file_put_contents($debugFile, $debugLine, FILE_APPEND);

if (($result['status_code'] ?? 0) < 200 || ($result['status_code'] ?? 0) >= 300 || !$normalized) {
    $error = 'vision api failed';
    if (($result['status_code'] ?? 0) === 0) {
        $error = 'vision api timeout or no response';
    } elseif (trim((string)$result['reply']) === '') {
        $error = 'vision api returned empty reply';
    }

    $analysisMap[$eventId] = [
        'event_id' => $eventId,
        'analysis_status' => 'failed',
        'analysis_updated_at' => date('Y-m-d H:i:s'),
        'analysis_error' => $error,
        'analysis_result' => null,
        'image_url' => $imageUrl,
    ];
    write_json_file_map($analysisFile, $analysisMap);

    http_response_code(502);
    echo json_encode([
        'ok' => false,
        'event_id' => $eventId,
        'analysis_status' => 'failed',
        'analysis_error' => $error,
        'raw_reply' => $result['reply'],
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

$analysisMap[$eventId] = [
    'event_id' => $eventId,
    'analysis_status' => 'done',
    'analysis_updated_at' => date('Y-m-d H:i:s'),
    'analysis_error' => '',
    'analysis_result' => $normalized,
    'image_url' => $imageUrl,
];
write_json_file_map($analysisFile, $analysisMap);

echo json_encode([
    'ok' => true,
    'cached' => false,
    'event_id' => $eventId,
    'analysis_status' => 'done',
    'analysis_updated_at' => $analysisMap[$eventId]['analysis_updated_at'],
    'analysis_error' => '',
    'analysis_result' => $normalized,
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
