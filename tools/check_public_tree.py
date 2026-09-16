#!/usr/bin/env python3
"""Fail when the public Git tree contains deployment secrets or local identity."""

from __future__ import annotations

import ipaddress
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_PATHS = (
    "main/clock_secrets.h",
    "main/certs/",
    "server/.env",
    "artifacts/",
    "backups/",
    "build/",
    ".venv/",
    "managed_components/",
)
TEXT_PATTERNS = {
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "personal home path": re.compile(r"/Users/[A-Za-z0-9._-]+/"),
    "non-placeholder MiniMax key": re.compile(r"MINIMAX_API_KEY=(?!replace-me|$).+"),
}
IPV4_CANDIDATE = re.compile(r"(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)")
EMAIL_CANDIDATE = re.compile(
    r"(?<![A-Za-z0-9._%+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"
)
ALLOWED_AUTHOR_NAMES = {
    "Round Clock Maintainer",
    "dependabot[bot]",
    "github-actions[bot]",
}
ALLOWED_AUTHOR_EMAIL_SUFFIX = "@users.noreply.github.com"


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"], cwd=ROOT, check=True, capture_output=True,
    )
    return [item.decode() for item in result.stdout.split(b"\0") if item]


def commit_identities() -> list[tuple[str, str]]:
    result = subprocess.run(
        ["git", "log", "HEAD", "--format=%an%x00%ae"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    identities = []
    for line in result.stdout.splitlines():
        name, email = line.decode().split("\0", 1)
        identities.append((name, email.lower()))
    return identities


def main() -> None:
    errors: list[str] = []
    for name in tracked_files():
        if name == "tools/check_public_tree.py":
            continue
        if any(name == prefix.rstrip("/") or name.startswith(prefix) for prefix in FORBIDDEN_PATHS):
            errors.append(f"forbidden path: {name}")
            continue
        path = ROOT / name
        try:
            text = path.read_text()
        except (UnicodeDecodeError, OSError):
            continue
        for label, pattern in TEXT_PATTERNS.items():
            if pattern.search(text):
                errors.append(f"{label}: {name}")
        for value in IPV4_CANDIDATE.findall(text):
            try:
                address = ipaddress.ip_address(value)
            except ValueError:
                continue
            if address.is_global:
                errors.append(f"public IPv4 address: {name}")
        for value in EMAIL_CANDIDATE.findall(text):
            lowered = value.lower()
            if not lowered.endswith(("@example.com", ALLOWED_AUTHOR_EMAIL_SUFFIX)):
                errors.append(f"non-placeholder email: {name}")
    for author_name, author_email in commit_identities():
        if author_name not in ALLOWED_AUTHOR_NAMES:
            errors.append("non-anonymous Git author name in commit history")
        if not author_email.endswith(ALLOWED_AUTHOR_EMAIL_SUFFIX):
            errors.append("non-noreply Git author email in commit history")
    if errors:
        raise SystemExit("public tree check failed:\n" + "\n".join(sorted(errors)))
    print(f"public tree check passed ({len(tracked_files())} tracked files)")


if __name__ == "__main__":
    main()
