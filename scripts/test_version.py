#!/usr/bin/env python3
"""Check version labels against isolated Git fixtures, never the project repository."""
from pathlib import Path
import tempfile
import subprocess
from version_info import firmware_version
with tempfile.TemporaryDirectory(prefix='pager-version-') as temporary:
    project=Path(temporary)
    assert firmware_version(project,'')=='dev'
    assert firmware_version(project,'v2.3.4')=='v2.3.4'
    try:
        firmware_version(project,'unsafe\nlabel')
        raise AssertionError('Control characters accepted')
    except ValueError:
        pass
    def git(*args):
        return subprocess.check_output(['git','-c','user.name=Fixture','-c','user.email=fixture@example.invalid','-c','commit.gpgsign=false',*args],cwd=project,text=True,stderr=subprocess.DEVNULL).strip()
    git('init','--quiet')
    assert firmware_version(project,'')=='dev'
    source=project/'source.txt';source.write_text('one\n');git('add','source.txt');git('commit','--quiet','-m','initial')
    assert firmware_version(project,'')==git('rev-parse','--short=8','HEAD')
    git('tag','v0.1.0');assert firmware_version(project,'')=='v0.1.0'
    source.write_text('two\n');git('add','source.txt');git('commit','--quiet','-m','next')
    assert firmware_version(project,'')==git('rev-parse','--short=8','HEAD')
    source.write_text('dirty\n');assert firmware_version(project,'')==git('rev-parse','--short=8','HEAD')
print('Version labels pass HEAD tag, short commit, override and no-commit fallback checks.')
