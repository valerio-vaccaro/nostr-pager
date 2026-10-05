#!/usr/bin/env python3
"""Run host checks for browser access control and client rendering."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
out = root / '.build-cache/web-tests'
out.mkdir(parents=True, exist_ok=True)
subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'),
                str(root/'scripts/native_fixtures/web_auth.cpp'), '-o', str(out/'auth')], check=True)
subprocess.run([str(out/'auth')], check=True)
subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'),
                str(root/'scripts/native_fixtures/history.cpp'), '-o', str(out/'history')], check=True)
subprocess.run([str(out/'history')], check=True)
subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-I'+str(root/'include'), str(root/'scripts/native_fixtures/web_bridge.cpp'),
                '-o', str(out/'bridge')], check=True)
subprocess.run([str(out/'bridge')], check=True)
subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'include'),
                str(root/'scripts/native_fixtures/message_expiry.cpp'), '-o', str(out/'expiry')], check=True)
subprocess.run([str(out/'expiry')], check=True)
subprocess.run(['node', '--check', str(root/'assets/web/app.js')], check=True)
subprocess.run(['node', str(root/'scripts/native_fixtures/web_client.js')], check=True)
