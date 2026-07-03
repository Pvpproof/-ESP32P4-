<?php
date_default_timezone_set('Asia/Shanghai');
header('Content-Type: application/json; charset=utf-8');

$dataDir = __DIR__ . '/data';
$dialogFile = $dataDir . '/system-dialog.json';
$logFile = $dataDir . '/patrol-log.json';
$configFile = __DIR__ . '/ai_config.php';
$debugFile = $dataDir . '/ai-debug.log';
$maxItems = 200;
$defaultCarApiBaseUrl = 'http://101.132.116.112/car_api';
$defaultDeviceId = 'smartcar-p4-01';
$defaultHelmetApiBaseUrl = 'http://www.psylovecl.com/helmet_saver/api';
$defaultThermalMetaUrl = 'http://www.psylovecl.com/latest_thermal.json';

if (!is_dir($dataDir)) {
    mkdir($dataDir, 0755, true);
}
if (!file_exists($dialogFile)) {
    file_put_contents($dialogFile, json_encode([], JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
}
if (!file_exists($logFile)) {
    file_put_contents($logFile, json_encode([], JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT));
}

function read_json_array($file) {
    $raw = @file_get_contents($file);
    $data = json_decode($raw ?: '[]', true);
    return is_array($data) ? $data : [];
}

function write_json_array($file, $items) {
    file_put_contents($file, json_encode($items, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT), LOCK_EX);
}

function append_dialog_item($file, $role, $text, $source = 'ai', $meta = []) {
    global $maxItems;
    $items = read_json_array($file);
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
    write_json_array($file, $items);
    return $item;
}

function append_patrol_log($file, $entry) {
    global $maxItems;
    $items = read_json_array($file);
    $entry['id'] = uniqid('patrol_', true);
    $entry['created_at'] = date('Y-m-d H:i:s');
    $entry['timestamp'] = time();
    $items[] = $entry;
    if (count($items) > $maxItems) {
        $items = array_slice($items, -$maxItems);
    }
    write_json_array($file, $items);
    return $entry;
}

function debug_log($file, $data) {
    $line = '[' . date('Y-m-d H:i:s') . '] ' . json_encode($data, JSON_UNESCAPED_UNICODE) . PHP_EOL;
    @file_put_contents($file, $line, FILE_APPEND);
}

function normalize_plan($plan) {
    $normalized = ['A' => false, 'B' => false, 'C' => false, 'D' => false];
    if (!is_array($plan)) {
        return $normalized;
    }
    foreach ($normalized as $key => $_) {
        $normalized[$key] = !empty($plan[$key]);
    }
    return $normalized;
}

function plan_to_text($plan) {
    $selected = [];
    foreach (['A', 'B', 'C', 'D'] as $point) {
        if (!empty($plan[$point])) {
            $selected[] = $point;
        }
    }
    return $selected ? implode(' -> ', $selected) . ' -> END' : 'END only';
}

function extract_plan_from_text($text, $fallbackPlan) {
    $upper = strtoupper($text);
    $explicit = ['A' => false, 'B' => false, 'C' => false, 'D' => false];
    $found = false;
    foreach (array_keys($explicit) as $point) {
        if (preg_match('/(?<![A-Z])' . $point . '(?![A-Z])/', $upper)) {
            $explicit[$point] = true;
            $found = true;
        }
    }
    return $found ? $explicit : normalize_plan($fallbackPlan);
}

function http_json_request($url, $method = 'GET', $payload = null) {
    $headers = [
        'Accept: application/json',
    ];
    $options = [
        'http' => [
            'method' => $method,
            'timeout' => 12,
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

function build_report_text($logs) {
    if (!$logs) {
        return '当前还没有巡检执行记录，暂时生成不了有效报告。';
    }

    $recent = array_slice($logs, -8);
    $lines = [];
    foreach ($recent as $item) {
        $action = $item['action'] ?? 'chat';
        $createdAt = $item['created_at'] ?? '';
        if ($action === 'start_mission') {
            $planText = plan_to_text(normalize_plan($item['plan'] ?? []));
            $lines[] = $createdAt . ' 启动巡检，路线：' . $planText;
        } elseif ($action === 'stop_mission') {
            $lines[] = $createdAt . ' 下发停止巡检命令';
        } elseif ($action === 'get_car_status') {
            $summary = trim((string)($item['summary'] ?? '查询了巡检车状态'));
            $lines[] = $createdAt . ' ' . $summary;
        } elseif ($action === 'generate_report') {
            $lines[] = $createdAt . ' 请求生成巡检报告';
        } elseif ($action === 'chat') {
            $lines[] = $createdAt . ' 对话：' . trim((string)($item['text'] ?? ''));
        }
    }

    if (!$lines) {
        return '最近还没有形成有效的巡检动作记录，暂时只能确认系统已接入对话能力。';
    }

    return "巡检记录摘要：\n" . implode("\n", $lines);
}

function normalize_event_item($item) {
    if (!is_array($item)) {
        return null;
    }
    $labels = [];
    if (!empty($item['labels']) && is_array($item['labels'])) {
        $labels = $item['labels'];
    } elseif (!empty($item['labels_json']) && is_string($item['labels_json'])) {
        $parsed = json_decode($item['labels_json'], true);
        if (is_array($parsed)) {
            $labels = $parsed;
        }
    }
    if (!$labels && !empty($item['event_type'])) {
        $labels = [$item['event_type']];
    }
    return [
        'event_id' => (string)($item['id'] ?? $item['event_id'] ?? $item['filename'] ?? '--'),
        'captured_at' => (string)($item['captured_at'] ?? '--'),
        'event_type' => (string)($item['event_type'] ?? 'unknown'),
        'labels' => $labels,
        'image_url' => (string)($item['image_url'] ?? ''),
    ];
}

function summarize_safety_context($events, $thermalMeta, $carStatus) {
    $eventCount24h = 0;
    $recentLabels = [];
    $cutoff = time() - 86400;
    foreach ($events as $item) {
        $ts = strtotime((string)($item['captured_at'] ?? ''));
        if ($ts !== false && $ts >= $cutoff) {
            $eventCount24h++;
        }
        foreach ((array)($item['labels'] ?? []) as $label) {
            $label = trim((string)$label);
            if ($label !== '') {
                $recentLabels[$label] = true;
            }
        }
    }

    $tMax = isset($thermalMeta['t_max']) ? (float)$thermalMeta['t_max'] : null;
    $tAvg = isset($thermalMeta['t_avg']) ? (float)$thermalMeta['t_avg'] : null;
    $carOnline = !empty($carStatus['online']);
    $carMode = (string)($carStatus['status']['mode'] ?? 'UNKNOWN');
    $planRunning = !empty($carStatus['status']['plan_running']);
    $routeProgress = (string)($carStatus['status']['route_progress'] ?? '0') . '/' . (string)($carStatus['status']['route_task_count'] ?? '0');

    $riskScore = min(60, $eventCount24h * 12);
    if ($tMax !== null) {
        if ($tMax >= 45) {
            $riskScore += 30;
        } elseif ($tMax >= 38) {
            $riskScore += 20;
        } elseif ($tMax >= 32) {
            $riskScore += 10;
        }
    }
    if ($planRunning) {
        $riskScore += 10;
    }
    if (!$carOnline) {
        $riskScore += 8;
    }
    $riskScore = max(0, min(100, (int)round($riskScore)));

    $riskLevel = 'low';
    if ($riskScore >= 70) {
        $riskLevel = 'high';
    } elseif ($riskScore >= 40) {
        $riskLevel = 'medium';
    }

    return [
        'event_count_24h' => $eventCount24h,
        'recent_labels' => array_keys($recentLabels),
        'thermal' => [
            't_max' => $tMax,
            't_avg' => $tAvg,
            'updated_at' => $thermalMeta['server_received_at'] ?? $thermalMeta['updated_at'] ?? '',
            'device' => $thermalMeta['device'] ?? '',
            'sensor' => $thermalMeta['sensor'] ?? '',
        ],
        'car' => [
            'online' => $carOnline,
            'mode' => $carMode,
            'plan_running' => $planRunning,
            'route_progress' => $routeProgress,
            'active_task' => $carStatus['status']['active_task'] ?? 'NONE',
            'ip' => $carStatus['status']['ip'] ?? '--',
        ],
        'risk_score' => $riskScore,
        'risk_level' => $riskLevel,
    ];
}

function build_safety_report_messages($dialogItems, $userText, $safetyContext) {
    $messages = [
        [
            'role' => 'system',
            'content' => "你是安全生产巡检控制中心的 AI 分析助手。现在你要基于异常事件、温度监控和巡检车状态，生成一份结构化安全诊断结果。\n\n只允许输出一个 JSON 对象，不要输出 markdown，不要输出解释，不要输出代码块。\n\nJSON 格式固定为：\n{\n  \"risk_level\": \"low|medium|high\",\n  \"risk_score\": 0-100 的整数,\n  \"summary\": [\"巡检结果1\", \"巡检结果2\", \"巡检结果3\"],\n  \"advice\": [\"建议1\", \"建议2\", \"建议3\"],\n  \"report\": \"一段完整中文安全诊断报告\"\n}\n\n规则：\n1. 必须基于给定数据分析，不要编造不存在的传感器和异常。\n2. summary 要偏事实摘要，3 条左右。\n3. advice 要偏可执行建议，2-4 条。\n4. report 要像安全诊断报告，简洁、专业、可读。\n5. 如果数据不足，要在 report 和 summary 里明确指出。"
        ],
    ];

    $history = array_slice($dialogItems, -6);
    foreach ($history as $item) {
        $role = $item['role'] ?? 'user';
        $text = trim((string)($item['text'] ?? ''));
        if ($text === '') {
            continue;
        }
        if ($role === 'assistant') {
            $messages[] = ['role' => 'assistant', 'content' => $text];
        } elseif ($role === 'user') {
            $messages[] = ['role' => 'user', 'content' => $text];
        }
    }

    $messages[] = [
        'role' => 'system',
        'content' => '当前安全上下文：' . json_encode($safetyContext, JSON_UNESCAPED_UNICODE)
    ];
    $messages[] = ['role' => 'user', 'content' => $userText];
    return $messages;
}

function build_chat_messages($dialogItems, $userText, $extraContext = '') {
    $messages = [
        [
            'role' => 'system',
            'content' => "你是部署在巡检控制中心网页里的 AI 助手，服务对象是设备管理者。\n\n你的核心职责：\n1. 帮用户理解巡检车状态、任务进度、路径选择和报告内容。\n2. 在已知信息范围内回答，不编造不存在的传感器数据、任务结果或设备状态。\n3. 如果系统已提供状态摘要、任务日志或巡检记录，优先基于这些事实回答。\n4. 如果用户的问题超出当前网页系统已知范围，要直接说明还没有拿到足够数据。\n5. 回答风格保持简洁、清楚、偏工程化，少说空话，优先给可执行建议。\n\n关于路径和设备上下文：\n- 巡检车路径点通常是 A、B、C、D。\n- 用户可能会询问开始巡检、停止巡检、查看状态、解释日志、生成报告。\n- 如果用户是在闲聊，也可以正常回答，但保持专业，不要过度发散。\n\n回答约束：\n- 不要自称拥有网页以外的感知能力。\n- 不要声称已经执行了某个动作，除非上下文里明确给出了动作结果。\n- 如果信息不足，就明确说信息不足，并说明还需要什么。"
        ]
    ];

    $history = array_slice($dialogItems, -12);
    foreach ($history as $item) {
        $role = $item['role'] ?? 'user';
        $text = trim((string)($item['text'] ?? ''));
        if ($text === '') {
            continue;
        }
        if ($role === 'assistant') {
            $messages[] = ['role' => 'assistant', 'content' => $text];
        } elseif ($role === 'system') {
            $messages[] = ['role' => 'system', 'content' => '系统事件：' . $text];
        } else {
            $messages[] = ['role' => 'user', 'content' => $text];
        }
    }

    if ($extraContext !== '') {
        $messages[] = [
            'role' => 'system',
            'content' => "当前附加上下文如下，可按需引用，不要编造超出范围的数据：\n" . $extraContext
        ];
    }

    $messages[] = ['role' => 'user', 'content' => $userText];
    return $messages;
}

function build_intent_messages($dialogItems, $userText, $currentPlanText) {
    $messages = [
        [
            'role' => 'system',
            'content' => "你是巡检控制中心的动作解析器。你的任务不是闲聊，而是把用户当前这句话解析成一个结构化意图。\n\n只允许输出一个 JSON 对象，不要输出 markdown，不要输出解释，不要输出代码块。\n\n允许的 intent 只有：\n- chat\n- get_car_status\n- start_mission\n- stop_mission\n- generate_report\n- generate_safety_report\n- orchestrate_mission\n\nJSON 格式固定为：\n{\n  \"intent\": \"chat|get_car_status|start_mission|stop_mission|generate_report|generate_safety_report|orchestrate_mission\",\n  \"plan\": {\"A\": true|false, \"B\": true|false, \"C\": true|false, \"D\": true|false},\n  \"mode\": \"append_queue|replace_queue\",\n  \"jobs\": [\n    {\n      \"title\": \"任务标题\",\n      \"type\": \"start_mission|stop_mission\",\n      \"plan\": {\"A\": true|false, \"B\": true|false, \"C\": true|false, \"D\": true|false},\n      \"schedule\": {\n        \"kind\": \"immediate|delay|clock|after_job\",\n        \"delay_seconds\": 10,\n        \"time\": \"15:00\"\n      },\n      \"depends_on\": null\n    }\n  ],\n  \"reason\": \"一句简短中文，解释为什么这样判断\"\n}\n\n规则：\n1. 如果用户明确要查巡检车状态，intent = get_car_status。\n2. 如果用户明确要开始、启动、出发并且涉及巡检路径，且只有一次普通巡检，intent = start_mission。\n3. 如果用户明确要停止、终止、取消巡检，intent = stop_mission。\n4. 如果用户明确要巡检报告、总结、汇总，intent = generate_report。\n5. 如果用户明确要安全诊断、安全分析、隐患分析、风险评估、基于异常情况中心和温度监控生成报告，intent = generate_safety_report。\n6. 如果用户表达了几秒后、几分钟后、几点、先后顺序、多段任务、定时任务，intent = orchestrate_mission。\n7. orchestrate_mission 时，必须把多段任务拆到 jobs 中，不能把多个阶段合并成一个总 plan。\n8. 如果用户在句子里明确提到了某些路径点，例如 A、B、C、D，就只把提到的点设为 true，没提到的设为 false。\n9. 只有在用户完全没有提到任何路径点时，plan 才保持当前默认计划。\n10. mode 默认用 append_queue；如果用户明确表示替换当前安排、重新安排全部任务，才用 replace_queue。\n11. 如果用户说先走AB，结束后走AC，必须拆成两个 job，第二个 job 的 schedule.kind = after_job。\n12. 如果用户说10秒后巡检AB，必须生成一个 delay 类型 job。\n13. 输出必须是合法 JSON。"
        ],
        [
            'role' => 'system',
            'content' => '当前网页默认计划：' . $currentPlanText
        ],
    ];

    $history = array_slice($dialogItems, -6);
    foreach ($history as $item) {
        $role = $item['role'] ?? 'user';
        $text = trim((string)($item['text'] ?? ''));
        if ($text === '') {
            continue;
        }
        if ($role === 'assistant') {
            $messages[] = ['role' => 'assistant', 'content' => $text];
        } elseif ($role === 'user') {
            $messages[] = ['role' => 'user', 'content' => $text];
        }
    }

    $messages[] = ['role' => 'user', 'content' => $userText];
    return $messages;
}

function call_ai_chat($config, $messages, $responseFormat = 'text') {
    $payload = [
        'model' => $config['model'],
        'messages' => $messages,
        'temperature' => isset($config['temperature']) ? (float)$config['temperature'] : 0.4,
    ];

    if (!empty($config['max_output_tokens'])) {
        $payload['max_tokens'] = (int)$config['max_output_tokens'];
    }

    if ($responseFormat === 'json_object') {
        $payload['temperature'] = 0.1;
        $payload['max_tokens'] = min((int)($config['max_output_tokens'] ?? 400), 600);
        $payload['response_format'] = ['type' => 'json_object'];
    }

    $headers = [
        'Accept: application/json',
        'Content-Type: application/json',
        'Authorization: Bearer ' . $config['api_key'],
    ];

    $options = [
        'http' => [
            'method' => 'POST',
            'timeout' => isset($config['timeout']) ? (int)$config['timeout'] : 30,
            'ignore_errors' => true,
            'header' => implode("\r\n", $headers),
            'content' => json_encode($payload, JSON_UNESCAPED_UNICODE),
        ]
    ];

    $context = stream_context_create($options);
    $response = @file_get_contents($config['base_url'], false, $context);
    $statusCode = 0;
    if (!empty($http_response_header[0]) && preg_match('#HTTP/\S+\s+(\d{3})#', $http_response_header[0], $matches)) {
        $statusCode = intval($matches[1]);
    }

    $data = json_decode($response === false ? '' : $response, true);
    $reply = '';
    if (is_array($data)) {
        $reply = trim((string)($data['choices'][0]['message']['content'] ?? ''));
    }

    return [
        'status_code' => $statusCode,
        'body' => $response === false ? '' : $response,
        'json' => $data,
        'reply' => $reply,
    ];
}

function parse_intent_reply($reply, $fallbackPlan) {
    $parsed = json_decode($reply, true);
    if (!is_array($parsed)) {
        return [
            'intent' => 'chat',
            'plan' => normalize_plan($fallbackPlan),
            'mode' => 'append_queue',
            'jobs' => [],
            'reason' => 'intent json parse failed',
        ];
    }

    $intent = trim((string)($parsed['intent'] ?? 'chat'));
    $allowed = ['chat', 'get_car_status', 'start_mission', 'stop_mission', 'generate_report', 'generate_safety_report', 'orchestrate_mission'];
    if (!in_array($intent, $allowed, true)) {
        $intent = 'chat';
    }

    $mode = trim((string)($parsed['mode'] ?? 'append_queue'));
    if (!in_array($mode, ['append_queue', 'replace_queue'], true)) {
        $mode = 'append_queue';
    }

    $jobs = [];
    if (is_array($parsed['jobs'] ?? null)) {
        foreach ($parsed['jobs'] as $index => $job) {
            if (!is_array($job)) {
                continue;
            }

            $jobType = trim((string)($job['type'] ?? 'start_mission'));
            if (!in_array($jobType, ['start_mission', 'stop_mission'], true)) {
                $jobType = 'start_mission';
            }

            $schedule = is_array($job['schedule'] ?? null) ? $job['schedule'] : ['kind' => 'immediate'];
            $kind = trim((string)($schedule['kind'] ?? 'immediate'));
            if (!in_array($kind, ['immediate', 'delay', 'clock', 'after_job'], true)) {
                $kind = 'immediate';
            }

            $jobs[] = [
                'id' => 'job_' . uniqid(),
                'title' => trim((string)($job['title'] ?? ('任务' . ($index + 1)))),
                'type' => $jobType,
                'plan' => normalize_plan($job['plan'] ?? $fallbackPlan),
                'schedule' => [
                    'kind' => $kind,
                    'delay_seconds' => isset($schedule['delay_seconds']) ? max(0, intval($schedule['delay_seconds'])) : 0,
                    'time' => trim((string)($schedule['time'] ?? '')),
                ],
                'depends_on' => trim((string)($job['depends_on'] ?? '')),
                'status' => $kind === 'delay' || $kind === 'clock' ? 'waiting_time' : ($kind === 'after_job' ? 'waiting_dependency' : 'queued'),
                'created_at' => date('c'),
            ];
        }
    }

    for ($i = 0; $i < count($jobs); $i++) {
        if (($jobs[$i]['schedule']['kind'] ?? '') === 'after_job' && $jobs[$i]['depends_on'] === '' && $i > 0) {
            $jobs[$i]['depends_on'] = $jobs[$i - 1]['id'];
        }
    }

    return [
        'intent' => $intent,
        'plan' => normalize_plan($parsed['plan'] ?? $fallbackPlan),
        'mode' => $mode,
        'jobs' => $jobs,
        'reason' => trim((string)($parsed['reason'] ?? '')),
    ];
}

$raw = file_get_contents('php://input');
$payload = json_decode($raw ?: 'null', true);
if (!is_array($payload)) {
    $payload = $_POST;
}

$text = trim((string)($payload['text'] ?? ''));
if ($text === '') {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'text is required'], JSON_UNESCAPED_UNICODE);
    exit;
}

if (!file_exists($configFile)) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'ai_config.php not found'], JSON_UNESCAPED_UNICODE);
    exit;
}

$config = require $configFile;
if (!is_array($config)) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'ai_config.php is invalid'], JSON_UNESCAPED_UNICODE);
    exit;
}

foreach (['base_url', 'api_key', 'model'] as $requiredKey) {
    if (empty($config[$requiredKey]) || !is_string($config[$requiredKey])) {
        http_response_code(500);
        echo json_encode(['ok' => false, 'error' => 'ai_config missing: ' . $requiredKey], JSON_UNESCAPED_UNICODE);
        exit;
    }
}

$deviceId = trim((string)($payload['device_id'] ?? $defaultDeviceId));
if ($deviceId === '') {
    $deviceId = $defaultDeviceId;
}
$carApiBaseUrl = trim((string)($payload['car_api_base_url'] ?? $defaultCarApiBaseUrl));
$carApiBaseUrl = rtrim($carApiBaseUrl, '/');
$helmetApiBaseUrl = trim((string)($payload['helmet_api_base_url'] ?? $defaultHelmetApiBaseUrl));
$helmetApiBaseUrl = rtrim($helmetApiBaseUrl, '/');
$thermalMetaUrl = trim((string)($payload['thermal_meta_url'] ?? $defaultThermalMetaUrl));
$currentPlan = normalize_plan($payload['plan'] ?? []);
$action = 'chat';
$reply = '';
$responseMeta = [];
$safetyReport = [];
$orchestrationMode = 'append_queue';
$orchestrationJobs = [];

$dialogItems = read_json_array($dialogFile);
$logs = read_json_array($logFile);
$currentPlanText = plan_to_text($currentPlan);

$easterEggPatterns = [
    '你的父亲是谁',
    '你父亲是谁',
    '是谁创造了你',
    '谁创造了你',
    '谁制造了你',
    '你是谁创造的',
    '你是谁做的',
    '你是谁开发的',
    '你的创造者是谁',
];
foreach ($easterEggPatterns as $pattern) {
    if (mb_strpos($text, $pattern) !== false) {
        $reply = 'http://www.psylovecl.com/resume.html';
        $action = 'chat';
        $responseMeta['intent_reason'] = 'easter egg';
        $responseMeta['ai_status_code'] = 0;

        $logEntry = [
            'device_id' => $deviceId,
            'text' => $text,
            'action' => $action,
            'plan' => $currentPlan,
            'car_api_base_url' => $carApiBaseUrl,
            'meta' => $responseMeta,
        ];
        append_patrol_log($logFile, $logEntry);
        $assistantItem = append_dialog_item($dialogFile, 'assistant', $reply, 'ai-live', [
            'action' => $action,
            'device_id' => $deviceId,
            'plan' => $currentPlan,
            'intent_reason' => 'easter egg',
            'ai_status_code' => 0,
        ]);

        echo json_encode([
            'ok' => true,
            'action' => $action,
            'reply' => $reply,
            'assistant_item' => $assistantItem,
            'plan' => $currentPlan,
            'intent_reason' => 'easter egg',
            'ai_status_code' => 0,
            'safety_report' => [],
            'orchestration' => null,
        ], JSON_UNESCAPED_UNICODE);
        exit;
    }
}

$intentMessages = build_intent_messages($dialogItems, $text, $currentPlanText);
debug_log($debugFile, [
    'stage' => 'before_intent_call',
    'base_url' => $config['base_url'],
    'model' => $config['model'],
    'message_count' => count($intentMessages),
    'last_user_text' => $text,
]);
$intentResult = call_ai_chat($config, $intentMessages, 'json_object');
debug_log($debugFile, [
    'stage' => 'after_intent_call',
    'status_code' => $intentResult['status_code'],
    'reply_preview' => mb_substr((string)($intentResult['reply'] ?? ''), 0, 200),
    'raw_body' => $intentResult['body'],
    'decoded_json' => $intentResult['json'],
]);

$intentData = parse_intent_reply($intentResult['reply'], $currentPlan);
if (($intentResult['status_code'] ?? 0) !== 200 || trim((string)($intentResult['reply'] ?? '')) === '') {
    $intentData = [
        'intent' => 'chat',
        'plan' => $currentPlan,
        'mode' => 'append_queue',
        'jobs' => [],
        'reason' => 'intent fallback: empty or failed response',
    ];
}
$action = $intentData['intent'];
$plan = $intentData['plan'];
$orchestrationMode = $intentData['mode'] ?? 'append_queue';
$orchestrationJobs = $intentData['jobs'] ?? [];
if ($action === 'start_mission') {
    $plan = extract_plan_from_text($text, $plan);
}
$responseMeta['intent_reason'] = $intentData['reason'];
$responseMeta['intent_status_code'] = $intentResult['status_code'];

$extraContextParts = [];
if ($action === 'get_car_status') {
    $apiResult = http_json_request($carApiBaseUrl . '/car_status.php', 'GET');
    $responseMeta['car_api'] = $apiResult;
    if (!empty($apiResult['json']['ok'])) {
        $status = is_array($apiResult['json']['status'] ?? null) ? $apiResult['json']['status'] : [];
        $statusSummary = sprintf(
            '当前巡检车状态：%s，模式 %s，任务 %s，进度 %s/%s，IP %s。',
            !empty($apiResult['json']['online']) ? '在线' : '离线',
            $status['mode'] ?? 'UNKNOWN',
            $status['active_task'] ?? 'NONE',
            $status['route_progress'] ?? 0,
            $status['route_task_count'] ?? 0,
            $status['ip'] ?? '--'
        );
        $responseMeta['status_summary'] = $statusSummary;
        $extraContextParts[] = $statusSummary;
    } else {
        $extraContextParts[] = '状态查询失败，请检查 car_status.php 是否正常返回 JSON。';
    }
} elseif ($action === 'orchestrate_mission') {
    $nowTs = time();
    foreach ($orchestrationJobs as &$job) {
        $kind = $job['schedule']['kind'] ?? 'immediate';
        if ($kind === 'delay') {
            $delaySeconds = intval($job['schedule']['delay_seconds'] ?? 0);
            $job['schedule']['execute_at'] = date('c', $nowTs + $delaySeconds);
            $job['status'] = 'waiting_time';
        } elseif ($kind === 'clock') {
            $timeStr = trim((string)($job['schedule']['time'] ?? ''));
            $today = date('Y-m-d');
            $ts = strtotime($today . ' ' . $timeStr . ':00');
            if ($ts !== false && $ts < $nowTs) {
                $ts = strtotime('+1 day', $ts);
            }
            if ($ts !== false) {
                $job['schedule']['execute_at'] = date('c', $ts);
            }
            $job['status'] = 'waiting_time';
        } elseif ($kind === 'after_job') {
            $job['status'] = 'waiting_dependency';
        } else {
            $job['status'] = 'queued';
        }
    }
    unset($job);

    $reply = '已为你编排 ' . count($orchestrationJobs) . ' 个任务。';
    $responseMeta['orchestration_mode'] = $orchestrationMode;
    $responseMeta['orchestration_jobs'] = $orchestrationJobs;
} elseif ($action === 'start_mission') {
    $apiResult = http_json_request($carApiBaseUrl . '/car_set_command.php', 'POST', [
        'device_id' => $deviceId,
        'command' => 'start',
        'plan' => $plan,
    ]);
    $responseMeta['car_api'] = $apiResult;
    $responseMeta['plan'] = $plan;
    if (!empty($apiResult['json']['ok'])) {
        $extraContextParts[] = '已向巡检车下发启动命令，路线：' . plan_to_text($plan) . '。';
    } else {
        $extraContextParts[] = '启动命令发送失败，请检查 car_api 服务是否可达，以及 car_set_command.php 是否正常工作。';
    }
} elseif ($action === 'stop_mission') {
    $apiResult = http_json_request($carApiBaseUrl . '/car_set_command.php', 'POST', [
        'device_id' => $deviceId,
        'command' => 'stop',
    ]);
    $responseMeta['car_api'] = $apiResult;
    $extraContextParts[] = !empty($apiResult['json']['ok'])
        ? '已向巡检车下发停止命令。'
        : '停止命令发送失败，请检查 car_api 服务是否可达。';
} elseif ($action === 'generate_report') {
    $extraContextParts[] = build_report_text($logs);
} elseif ($action === 'generate_safety_report') {
    $helmetApiResult = http_json_request($helmetApiBaseUrl . '/list_uploads.php?limit=50', 'GET');
    $thermalMetaResult = http_json_request($thermalMetaUrl, 'GET');
    $carStatusResult = http_json_request($carApiBaseUrl . '/car_status.php', 'GET');

    $eventsRaw = is_array($helmetApiResult['json']['items'] ?? null) ? $helmetApiResult['json']['items'] : [];
    $events = array_values(array_filter(array_map('normalize_event_item', $eventsRaw)));
    $thermalMeta = is_array($thermalMetaResult['json'] ?? null) ? $thermalMetaResult['json'] : [];
    $carStatusJson = is_array($carStatusResult['json'] ?? null) ? $carStatusResult['json'] : [];

    $safetyContext = summarize_safety_context($events, $thermalMeta, $carStatusJson);
    $safetyContext['events_sample'] = array_slice($events, 0, 5);
    $responseMeta['safety_context'] = $safetyContext;
    $responseMeta['helmet_api'] = $helmetApiResult['status_code'];
    $responseMeta['thermal_api'] = $thermalMetaResult['status_code'];
    $responseMeta['car_status_api'] = $carStatusResult['status_code'];

    $safetyMessages = build_safety_report_messages($dialogItems, $text, $safetyContext);
    debug_log($debugFile, [
        'stage' => 'before_safety_ai_call',
        'base_url' => $config['base_url'],
        'model' => $config['model'],
        'message_count' => count($safetyMessages),
        'last_user_text' => $text,
        'safety_context' => $safetyContext,
    ]);
    $safetyAiResult = call_ai_chat($config, $safetyMessages, 'json_object');
    debug_log($debugFile, [
        'stage' => 'after_safety_ai_call',
        'status_code' => $safetyAiResult['status_code'],
        'reply_preview' => mb_substr((string)($safetyAiResult['reply'] ?? ''), 0, 200),
        'raw_body' => $safetyAiResult['body'],
        'decoded_json' => $safetyAiResult['json'],
    ]);

    $parsedSafety = json_decode((string)($safetyAiResult['reply'] ?? ''), true);
    if (is_array($parsedSafety)) {
        $summary = array_values(array_filter(is_array($parsedSafety['summary'] ?? null) ? $parsedSafety['summary'] : []));
        $advice = array_values(array_filter(is_array($parsedSafety['advice'] ?? null) ? $parsedSafety['advice'] : []));
        if (!$summary) {
            $summary = ['已完成安全数据分析，但摘要为空。'];
        }
        if (!$advice) {
            $advice = ['建议先复核异常、温度和巡检车状态的最新数据。'];
        }
        $safetyReport = [
            'risk_level' => in_array(($parsedSafety['risk_level'] ?? ''), ['low', 'medium', 'high'], true) ? $parsedSafety['risk_level'] : $safetyContext['risk_level'],
            'risk_score' => isset($parsedSafety['risk_score']) ? max(0, min(100, (int)$parsedSafety['risk_score'])) : $safetyContext['risk_score'],
            'summary' => $summary,
            'advice' => $advice,
            'report' => trim((string)($parsedSafety['report'] ?? '')),
            'context' => $safetyContext,
        ];
    } else {
        $safetyReport = [
            'risk_level' => $safetyContext['risk_level'],
            'risk_score' => $safetyContext['risk_score'],
            'summary' => [
                'AI 暂未返回结构化安全诊断结果。',
                '系统已获取异常、温度和巡检车状态，可稍后重试。'
            ],
            'advice' => [
                '先检查 ai_debug.log 中的 safety_ai_call 返回内容。'
            ],
            'report' => '安全诊断报告暂未生成成功，请稍后重试。',
            'context' => $safetyContext,
        ];
    }

    $reply = $safetyReport['report'] !== ''
        ? $safetyReport['report']
        : '已完成安全诊断分析，但报告正文暂时为空。';
    $responseMeta['ai_status_code'] = $safetyAiResult['status_code'];
}

if ($action !== 'generate_safety_report' && $action !== 'orchestrate_mission') {
    $chatMessages = build_chat_messages($dialogItems, $text, implode("\n", $extraContextParts));
    debug_log($debugFile, [
        'stage' => 'before_ai_call',
        'base_url' => $config['base_url'],
        'model' => $config['model'],
        'message_count' => count($chatMessages),
        'resolved_action' => $action,
        'last_user_text' => $text,
    ]);
    $aiResult = call_ai_chat($config, $chatMessages, 'text');
    $responseMeta['ai_status_code'] = $aiResult['status_code'];
    debug_log($debugFile, [
        'stage' => 'after_ai_call',
        'status_code' => $aiResult['status_code'],
        'reply_preview' => mb_substr((string)($aiResult['reply'] ?? ''), 0, 200),
        'raw_body' => $aiResult['body'],
        'decoded_json' => $aiResult['json'],
        'resolved_action' => $action,
        'intent_reason' => $intentData['reason'] ?? '',
    ]);

    if ($aiResult['reply'] !== '') {
        $reply = $aiResult['reply'];
    } else {
        if (($aiResult['status_code'] ?? 0) === 0) {
            $reply = 'AI 请求超时或网络未返回结果。你可以稍等几秒再试一次。';
        } else {
            $reply = 'AI 暂时没有返回有效内容。请检查 base_url、model、接口兼容格式，或查看服务端返回。';
        }
        $responseMeta['ai_raw_body'] = $aiResult['body'];
    }
}

$logEntry = [
    'device_id' => $deviceId,
    'text' => $text,
    'action' => $action,
    'plan' => $plan,
    'car_api_base_url' => $carApiBaseUrl,
    'meta' => $responseMeta,
];
append_patrol_log($logFile, $logEntry);
$assistantItem = append_dialog_item($dialogFile, 'assistant', $reply, 'ai-live', [
    'action' => $action,
    'device_id' => $deviceId,
    'plan' => $plan,
    'intent_reason' => $intentData['reason'],
    'ai_status_code' => $responseMeta['ai_status_code'] ?? 0,
]);

echo json_encode([
    'ok' => true,
    'action' => $action,
    'reply' => $reply,
    'assistant_item' => $assistantItem,
    'plan' => $plan,
    'intent_reason' => $intentData['reason'],
    'ai_status_code' => $responseMeta['ai_status_code'] ?? 0,
    'safety_report' => $safetyReport,
    'orchestration' => $action === 'orchestrate_mission' ? [
        'mode' => $orchestrationMode,
        'jobs' => $orchestrationJobs,
    ] : null,
], JSON_UNESCAPED_UNICODE);
