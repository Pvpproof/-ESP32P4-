#!/usr/bin/env python3
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
WHISPER_DIR = os.path.join(ROOT, 'vendor', 'whisper.cpp')
WHISPER_BIN = os.path.join(WHISPER_DIR, 'build', 'bin', 'whisper-cli')
MODEL_PATH = os.path.join(WHISPER_DIR, 'models', 'ggml-base.bin')


def fail(msg):
    print(json.dumps({"ok": False, "error": msg}, ensure_ascii=False))
    return 1


def extract_text_from_stdout(stdout_text):
    lines = []
    for raw in (stdout_text or '').splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith('[') and '-->' in line:
            cleaned = re.sub(r'^\[[^\]]+\]\s*', '', line).strip()
            if cleaned:
                lines.append(cleaned)
            continue

        if line.startswith('whisper_') or line.startswith('system_info:') or line.startswith('main:'):
            continue
        if line.startswith('output_') or line.startswith('open: failed to open'):
            continue
        if line.startswith('loading model') or line.startswith('processing '):
            continue
        if line.startswith('use gpu') or line.startswith('flash attn') or line.startswith('gpu_device'):
            continue
        if line.startswith('devices') or line.startswith('backends') or line.startswith('n_'):
            continue
        if line.startswith('CPU total size') or line.startswith('model size'):
            continue
        if line.startswith('[') and line.endswith(']'):
            continue
        if re.match(r'^(load time|fallbacks|mel time|sample time|encode time|decode time|batchd time|prompt time|total time)\s*=.*', line):
            continue
        if re.match(r'^[A-Za-z0-9_./:-]+\s*=.*', line):
            continue
        lines.append(line)

    text = ' '.join(lines).strip()
    text = re.sub(r'\s+', ' ', text).strip()
    return text


def read_first_existing(paths):
    for path in paths:
        if os.path.exists(path):
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                return f.read().strip()
    return ''


def main():
    if len(sys.argv) < 3:
        return fail('usage: asr_runner.py <wav_path> <device> [lang]')

    wav_path = sys.argv[1]
    device = sys.argv[2]
    lang = sys.argv[3] if len(sys.argv) > 3 else 'zh'

    if not os.path.exists(wav_path):
        return fail('wav not found')
    if not os.path.exists(WHISPER_BIN):
        return fail('whisper-cli not found')
    if not os.path.exists(MODEL_PATH):
        return fail('ggml-base.bin not found')

    cmd = [
        WHISPER_BIN,
        '-m', MODEL_PATH,
        '-f', wav_path,
        '-l', lang,
        '-nt',
    ]

    try:
        completed = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=180,
            universal_newlines=True,
        )
    except subprocess.TimeoutExpired:
        return fail('whisper timeout')
    except Exception as e:
        return fail('whisper exec failed: %s' % e)

    stdout_text = completed.stdout or ''
    if completed.returncode != 0:
        return fail('whisper failed: ' + stdout_text[-2000:])

    text = extract_text_from_stdout(stdout_text)

    if not text:
        return fail('asr text output missing; stdout=' + stdout_text[-2000:])

    print(json.dumps({
        'ok': True,
        'mode': 'whisper.cpp',
        'device': device,
        'language': lang,
        'text': text,
    }, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    sys.exit(main())
