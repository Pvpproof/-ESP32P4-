<?php
date_default_timezone_set('Asia/Shanghai');
header('Content-Type: application/json; charset=utf-8');

$token = 'jarvis_speech_token_001';
$baseDir = __DIR__;
$uploadDir = $baseDir . '/uploads/audio';
$dataDir = $baseDir . '/data';
$logFile = $dataDir . '/device-messages.json';
$dialogFile = $dataDir . '/system-dialog.json';
$runner = $baseDir . '/asr_runner.py';
$aiChatFile = $baseDir . '/ai_chat.php';
$pythonBin = 'python3';
$maxMessages = 200;
$defaultLang = 'zh';
$defaultCarApiBaseUrl = 'http://101.132.116.112/car_api';
$defaultDeviceId = 'smartcar-p4-01';

if (!is_dir($uploadDir)) {
    mkdir($uploadDir, 0755, true);
}
if (!is_dir($dataDir)) {
    mkdir($dataDir, 0755, true);
}
if (!file_exists($logFile)) {
    file_put_contents($logFile, json_encode([], JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
}
if (!file_exists($dialogFile)) {
    file_put_contents($dialogFile, json_encode([], JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
}
$debugFile = $dataDir . '/asr-debug.log';

function read_messages($file) {
    $raw = @file_get_contents($file);
    $data = json_decode($raw ?: '[]', true);
    return is_array($data) ? $data : [];
}

function write_messages($file, $messages) {
    file_put_contents($file, json_encode($messages, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);
}

function append_bus_message($logFile, $device, $message, $level = 'info', $type = 'text') {
    global $maxMessages;
    $messages = read_messages($logFile);
    $item = [
        'id' => uniqid('msg_', true),
        'device' => $device,
        'message' => $message,
        'level' => $level,
        'type' => $type,
        'created_at' => date('Y-m-d H:i:s'),
        'timestamp' => time(),
        'remote_addr' => $_SERVER['REMOTE_ADDR'] ?? ''
    ];
    $messages[] = $item;
    if (count($messages) > $maxMessages) {
        $messages = array_slice($messages, -$maxMessages);
    }
    write_messages($logFile, $messages);
    return $item;
}

function append_dialog_message($file, $role, $text, $source = 'web', $meta = []) {
    global $maxMessages;
    $raw = @file_get_contents($file);
    $items = json_decode($raw ?: '[]', true);
    if (!is_array($items)) {
        $items = [];
    }
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
    if (count($items) > $maxMessages) {
        $items = array_slice($items, -$maxMessages);
    }
    $result = file_put_contents($file, json_encode($items, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);
    return [$item, $result];
}

function debug_log($file, $data) {
    $line = '[' . date('Y-m-d H:i:s') . '] ' . json_encode($data, JSON_UNESCAPED_UNICODE) . PHP_EOL;
    @file_put_contents($file, $line, FILE_APPEND);
}

function http_json_request($url, $method = 'POST', $payload = null) {
    $headers = [
        'Accept: application/json',
    ];
    $options = [
        'http' => [
            'method' => $method,
            'timeout' => 8,
            'ignore_errors' => true,
            'header' => implode("\r\n", $headers),
        ]
    ];

    if ($payload !== null) {
        $json = json_encode($payload, JSON_UNESCAPED_UNICODE);
        $headers[] = 'Content-Type: application/json';
        $headers[] = 'Content-Length: ' . strlen($json);
        $options['http']['header'] = implode("\r\n", $headers);
        $options['http']['content'] = $json;
    }

    $context = stream_context_create($options);
    $response = @file_get_contents($url, false, $context);
    $statusCode = 0;
    if (!empty($http_response_header[0]) && preg_match('#HTTP/\S+\s+(\d{3})#', $http_response_header[0], $matches)) {
        $statusCode = intval($matches[1]);
    }

    return [
        'status_code' => $statusCode,
        'body' => $response === false ? '' : $response,
        'json' => json_decode($response === false ? '' : $response, true),
    ];
}

$reqToken = $_GET['token'] ?? ($_POST['token'] ?? '');
if ($reqToken !== $token) {
    http_response_code(403);
    echo json_encode(['ok' => false, 'error' => 'invalid token'], JSON_UNESCAPED_UNICODE);
    exit;
}

$device = trim((string)($_GET['device'] ?? ($_POST['device'] ?? 'm5stick-s3-mic')));
if ($device === '') {
    $device = 'm5stick-s3-mic';
}
$lang = trim((string)($_GET['lang'] ?? ($_POST['lang'] ?? $defaultLang)));
if ($lang === '') {
    $lang = $defaultLang;
}
$carApiBaseUrl = trim((string)($_GET['car_api_base_url'] ?? ($_POST['car_api_base_url'] ?? $defaultCarApiBaseUrl)));
if ($carApiBaseUrl === '') {
    $carApiBaseUrl = $defaultCarApiBaseUrl;
}
$carDeviceId = trim((string)($_GET['car_device_id'] ?? ($_POST['car_device_id'] ?? $defaultDeviceId)));
if ($carDeviceId === '') {
    $carDeviceId = $defaultDeviceId;
}

$raw = file_get_contents('php://input');
if (!$raw || strlen($raw) < 128) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'empty audio body'], JSON_UNESCAPED_UNICODE);
    exit;
}

$filename = $device . '_' . date('Ymd_His') . '_' . substr(md5((string)microtime(true)), 0, 8) . '.wav';
$filePath = $uploadDir . '/' . $filename;
file_put_contents($filePath, $raw);

debug_log($debugFile, [
    'stage' => 'saved_audio',
    'device' => $device,
    'lang' => $lang,
    'filePath' => $filePath,
    'saved_file' => 'uploads/audio/' . $filename,
    'exists' => file_exists($filePath),
    'size' => @filesize($filePath),
    'raw_size' => strlen($raw)
]);

append_bus_message($logFile, $device, '收到音频上传，开始识别：' . $filename, 'info', 'audio');

$asr = [
    'ok' => false,
    'mode' => 'none',
    'text' => '',
    'error' => ''
];
$aiResult = [
    'ok' => false,
    'triggered' => false,
    'reply' => '',
    'error' => ''
];

if (file_exists($runner)) {
    $cmd = escapeshellarg($pythonBin) . ' ' . escapeshellarg($runner) . ' ' . escapeshellarg($filePath) . ' ' . escapeshellarg($device) . ' ' . escapeshellarg($lang) . ' 2>&1';
    debug_log($debugFile, [
        'stage' => 'before_runner',
        'runner' => $runner,
        'python' => $pythonBin,
        'cmd' => $cmd,
        'file_exists' => file_exists($filePath),
        'file_size' => @filesize($filePath)
    ]);
    $output = shell_exec($cmd);
    debug_log($debugFile, [
        'stage' => 'after_runner',
        'output' => $output
    ]);
    $parsed = json_decode((string)$output, true);
    debug_log($debugFile, [
        'stage' => 'after_json_decode',
        'parsed' => $parsed,
        'json_last_error' => json_last_error(),
        'json_last_error_msg' => json_last_error_msg()
    ]);
    if (is_array($parsed)) {
        $asr = $parsed;
    } else {
        $asr = [
            'ok' => false,
            'mode' => 'runner-error',
            'text' => '',
            'error' => trim((string)$output)
        ];
    }
} else {
    debug_log($debugFile, [
        'stage' => 'runner_missing',
        'runner_exists' => file_exists($runner),
        'python_bin' => $pythonBin
    ]);
}

if (!empty($asr['ok']) && !empty($asr['text'])) {
    append_bus_message($logFile, $device, '语音识别结果：' . $asr['text'], 'info', 'asr');
    [$dialogItem, $dialogWriteResult] = append_dialog_message($dialogFile, 'user', $asr['text'], $device, [
        'type' => 'voice-asr',
        'device' => $device,
        'saved_file' => 'uploads/audio/' . $filename,
        'language' => $lang
    ]);
    debug_log($debugFile, [
        'stage' => 'dialog_append_success_branch',
        'dialog_item' => $dialogItem,
        'dialog_write_result' => $dialogWriteResult
    ]);

    if (file_exists($aiChatFile)) {
        $aiPayload = [
            'text' => $asr['text'],
            'device_id' => $carDeviceId,
            'car_api_base_url' => rtrim($carApiBaseUrl, '/'),
            'plan' => ['A' => true, 'B' => true, 'C' => true, 'D' => true]
        ];
        $aiResultRaw = http_json_request((isset($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off' ? 'https' : 'http') . '://' . ($_SERVER['HTTP_HOST'] ?? '127.0.0.1') . rtrim(dirname($_SERVER['PHP_SELF'] ?? '/'), '/') . '/ai_chat.php', 'POST', $aiPayload);
        $aiJson = is_array($aiResultRaw['json'] ?? null) ? $aiResultRaw['json'] : [];
        $aiResult = [
            'ok' => !empty($aiJson['ok']),
            'triggered' => true,
            'reply' => (string)($aiJson['reply'] ?? ''),
            'error' => !empty($aiJson['ok']) ? '' : trim((string)($aiJson['error'] ?? 'AI trigger failed')),
            'action' => (string)($aiJson['action'] ?? ''),
            'status_code' => intval($aiResultRaw['status_code'] ?? 0)
        ];
        debug_log($debugFile, [
            'stage' => 'ai_chat_triggered',
            'payload' => $aiPayload,
            'result' => $aiResult,
            'raw_status_code' => $aiResultRaw['status_code'] ?? 0,
            'raw_body' => $aiResultRaw['body'] ?? ''
        ]);
    } else {
        $aiResult = [
            'ok' => false,
            'triggered' => false,
            'reply' => '',
            'error' => 'ai_chat.php not found'
        ];
        debug_log($debugFile, [
            'stage' => 'ai_chat_missing',
            'ai_chat_file' => $aiChatFile
        ]);
    }
} else {
    append_bus_message($logFile, $device, '语音识别暂未完成，音频已保存。', 'warn', 'asr');
    debug_log($debugFile, [
        'stage' => 'dialog_append_skipped',
        'asr' => $asr
    ]);
}

$response = [
    'ok' => true,
    'device' => $device,
    'language' => $lang,
    'saved_file' => 'uploads/audio/' . $filename,
    'size' => strlen($raw),
    'asr' => $asr,
    'ai' => $aiResult,
    'text' => $asr['text'] ?? '',
    'server_time' => date('Y-m-d H:i:s')
];

echo json_encode($response, JSON_UNESCAPED_UNICODE);
