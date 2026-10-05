"""Resolve a firmware label from the HEAD tag, short commit, or dev."""
import os
import subprocess
from pathlib import Path

def firmware_version(project: Path, override=None):
    override = os.environ.get('NOSTR_PAGER_VERSION') if override is None else override
    if override:
        value = override.strip()
        if not value or len(value) > 64 or any(ord(c)<32 or ord(c)>126 for c in value):
            raise ValueError('NOSTR_PAGER_VERSION must be 1–64 printable ASCII characters')
        return value
    try:
        commit = subprocess.run(['git','rev-parse','--verify','HEAD'],
                                cwd=project, capture_output=True, text=True, check=True)
        if commit.stdout.strip():
            tag = subprocess.run(['git','tag','--points-at','HEAD'],
                                 cwd=project, capture_output=True, text=True, check=True)
            tags = sorted(tag.stdout.splitlines())
            if tags:
                return tags[0][:64]
            short = subprocess.run(['git','rev-parse','--short=8','HEAD'],
                                   cwd=project, capture_output=True, text=True, check=True)
            return short.stdout.strip()[:64]
    except (OSError, subprocess.CalledProcessError):
        pass
    return 'dev'
