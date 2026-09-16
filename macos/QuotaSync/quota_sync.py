#!/usr/bin/env python3
"""Publish LLMQuota's generated dashboard file without recalculating usage."""

from __future__ import annotations

import argparse
import json
import os
import ssl
import urllib.request
import urllib.parse
from pathlib import Path


DEFAULT_SOURCE = Path.home() / "Library/Application Support/LLMQuotaBar/shared/dashboard.json"
DEFAULT_CONFIG_DIR = Path.home() / "Library/Application Support/RoundClock"
DEFAULT_CERT_DIR = DEFAULT_CONFIG_DIR


def load_taskboards(directory: Path) -> bytes:
    """Only export the existing display contract, never prompts, paths or credentials."""
    board_fields = ('machineID', 'nodeName', 'generatedAt', 'tasksTruncated')
    task_fields = ('id', 'title', 'state', 'platform', 'repoAlias', 'landedAt',
                   'stepIndex', 'stepTotal', 'progressPhase', 'progressSummary',
                   'progressNextStep', 'progressUpdatedAt', 'waitReason')
    boards = []
    for path in sorted(directory.glob('*.json'))[:16]:
        if path.is_symlink() or path.stat().st_size > 1024*1024:
            raise ValueError('invalid task board file')
        original = json.loads(path.read_bytes())
        if original.get('machineID') != path.stem or not isinstance(original.get('tasks'), list):
            raise ValueError('task board identity mismatch')
        board = {k: original[k] for k in board_fields if k in original}
        board['tasks'] = [{k: t[k] for k in task_fields if k in t} for t in original['tasks'][:100]]
        if len(original['tasks']) > 100:
            board['tasksTruncated'] = True
        boards.append(board)
    payload = json.dumps({'boards': boards}, ensure_ascii=False).encode()
    if len(payload) > 128*1024:
        raise ValueError('task boards too large')
    return payload


def load_dashboard(path: Path) -> bytes:
    payload = path.read_bytes()
    value = json.loads(payload)
    if not isinstance(value, dict) or not isinstance(value.get("generatedAt"), str):
        raise ValueError("not an LLMQuota dashboard projection")
    reports = value.get("reports")
    if not isinstance(reports, list) or not reports:
        raise ValueError("LLMQuota dashboard has no reports")
    return payload


def post_payload(payload: bytes, url: str, cert: Path, key: Path, ca: Path, token: str) -> dict:
    if urllib.parse.urlparse(url).scheme != "https":
        raise ValueError("only HTTPS device endpoints are allowed")
    context = ssl.create_default_context(cafile=str(ca))
    context.load_cert_chain(certfile=str(cert), keyfile=str(key))
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, data=payload, headers=headers, method="POST")
    with urllib.request.urlopen(request, context=context, timeout=20) as response:  # nosec B310
        result = json.load(response)
    return result


def publish(source: Path, url: str, cert: Path, key: Path, ca: Path, token: str) -> str:
    result = post_payload(load_dashboard(source), url, cert, key, ca, token)
    return f"已同步 LLMQuota 快照 {result['generatedAt']}（{result['bytes']} 字节）"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    base_file = DEFAULT_CONFIG_DIR / "api-base"
    default_base = base_file.read_text().strip() if base_file.is_file() else "https://device-api.example.invalid:8080"
    parser.add_argument("--url", default=os.environ.get("ROUND_QUOTA_URL", default_base + "/v1/quota"))
    parser.add_argument("--cert", type=Path, default=DEFAULT_CERT_DIR / "device-client.pem")
    parser.add_argument("--key", type=Path, default=DEFAULT_CERT_DIR / "device-client.key")
    parser.add_argument("--ca", type=Path, default=DEFAULT_CERT_DIR / "device-ca.pem")
    parser.add_argument("--token", default=os.environ.get("ROUND_QUOTA_TOKEN", ""))
    parser.add_argument("--token-file", type=Path, default=DEFAULT_CONFIG_DIR / "device-token")
    parser.add_argument("--agents-url", default=default_base + "/v1/agents")
    parser.add_argument("--taskboards", type=Path, default=DEFAULT_SOURCE.parent / 'taskboards')
    args = parser.parse_args()
    token = args.token or (args.token_file.read_text().strip() if args.token_file.is_file() else "")
    if len(token) < 32:
        raise ValueError("device token is missing or too short")
    print(publish(args.source, args.url, args.cert, args.key, args.ca, token))
    result = post_payload(load_taskboards(args.taskboards), args.agents_url, args.cert, args.key, args.ca, token)
    if not result.get('stored'):
        raise ValueError('agent snapshot not acknowledged')
    print('已同步数字人任务板（只读）')


if __name__ == "__main__":
    main()
