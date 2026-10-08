#!/usr/bin/env python3
import re
import subprocess
import sys
from pathlib import Path

DISALLOWED_IDENTITY = re.compile(r"\b(?:claude|codex)\b|@(?:anthropic\.com|claude\.ai|openai\.com)\b", re.I)
COAUTHOR_TRAILER = re.compile(r"^\s*Co-Authored-By\s*:\s*(.*)$", re.I)


def violations(author, message):
    result = []
    if DISALLOWED_IDENTITY.search(author):
        result.append("disallowed author identity")
    for line in message.splitlines():
        trailer = COAUTHOR_TRAILER.match(line)
        if trailer and DISALLOWED_IDENTITY.search(trailer.group(1)):
            result.append("disallowed co-author trailer")
    return result


def main():
    if len(sys.argv) != 2:
        print("usage: check_commit_metadata.py <commit-message-file>", file=sys.stderr)
        return 2
    author = subprocess.run(["git", "var", "GIT_AUTHOR_IDENT"], check=True, capture_output=True, text=True).stdout.strip()
    message = Path(sys.argv[1]).read_text(encoding="utf-8")
    errors = violations(author, message)
    if errors:
        print("Commit rejected: Claude and Codex cannot be recorded as commit authors or co-authors.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
