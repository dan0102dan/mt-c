#!/usr/bin/env python3
"""Scan actual compiled payloads, not source #ifdefs or a runtime UI flag."""
import argparse
from pathlib import Path

BACKEND = [b'/api/v1/keenetic/policy', b'/rci/show/ip/policy', b'keeneticPolicy', b'POLICY_RULES']
FRONTEND = [b'/keenetic/policy', b'Use Keenetic policy', b'keenetic-policy-enabled', 'Политика Keenetic'.encode()]

def verify(path: Path, needles: list[bytes], enabled: bool) -> None:
    files = [path] if path.is_file() else [p for p in path.rglob('*') if p.is_file()]
    if not files:
        raise SystemExit(f'Missing payload: {path}')
    payload = b''.join(p.read_bytes() for p in files)
    for needle in needles:
        if (needle in payload) != enabled:
            raise SystemExit(f'{path}: {needle!r} must be {"present" if enabled else "absent"}')
    print(f'PASS {"Keenetic feature present" if enabled else "no Keenetic policy feature"}: {path}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('generic-binary', 'keenetic-binary', 'generic-frontend', 'keenetic-frontend'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    verify(args.generic_binary, BACKEND, False)
    verify(args.keenetic_binary, BACKEND, True)
    verify(args.generic_frontend, FRONTEND, False)
    verify(args.keenetic_frontend, FRONTEND, True)
