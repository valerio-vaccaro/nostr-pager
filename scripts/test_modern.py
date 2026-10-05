#!/usr/bin/env python3
"""Test production NIP-44/NIP-59 code with OpenSSL-backed host AES/base64/random adapters."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import subprocess
import os
root=Path(__file__).resolve().parents[1]
out=root/'.build-cache/modern';out.mkdir(parents=True,exist_ok=True)
flags=['-O1','-g','-DUSE_STDONLY','-DARDUINOJSON_ENABLE_ARDUINO_STRING=1','-I'+str(root/'scripts/native_fixtures/crypto'),'-I'+str(root/'include'),'-I'+str(root/'lib/ArduinoJson/src'),'-I'+str(root/'lib/uBitcoin/src')]
sources=list((root/'lib/uBitcoin/src').rglob('*.c'))+list((root/'lib/uBitcoin/src').rglob('*.cpp'))+[root/'src/key_codec.cpp',root/'src/nip44.cpp',root/'src/nostr_crypto.cpp',root/'src/contacts.cpp',root/'scripts/native_fixtures/modern.cpp']
def compile(source):
    obj=out/('_'.join(source.relative_to(root).parts)+'.o')
    subprocess.run(['gcc' if source.suffix=='.c' else 'g++',*flags,'-c',str(source),'-o',str(obj)],check=True)
    return str(obj)
with ThreadPoolExecutor(max_workers=4) as pool:objects=list(pool.map(compile,sources))
subprocess.run(['g++',*objects,'-lcrypto','-o',str(out/'modern-tests')],check=True)
oracle=os.environ.get('NOSTR_ORACLE_MODULES')
if oracle:
    subprocess.run(['node',str(root/'scripts/native_fixtures/modern_oracle.js')],check=True,cwd=root)
    subprocess.run([str(out/'modern-tests'),str(out/'oracle.json')],check=True,cwd=root)
    subprocess.run(['node',str(root/'scripts/native_fixtures/modern_oracle.js'),'--check'],check=True,cwd=root)
else:
    subprocess.run([str(out/'modern-tests')],check=True,cwd=root)

requests=compile(root/'scripts/native_fixtures/inbox_requests.cpp')
subprocess.run(['g++',requests,'-o',str(out/'inbox-request-tests')],check=True)
subprocess.run([str(out/'inbox-request-tests')],check=True,cwd=root)

routing=[]
for source in [root/'src/dm_relay.cpp', root/'scripts/native_fixtures/dm_routing.cpp']:
    routing.append(compile(source))
shared=[obj for obj in objects if 'scripts_native_fixtures_modern.cpp' not in obj]
subprocess.run(['g++',*shared,*routing,'-lcrypto','-o',str(out/'routing-tests')],check=True)
subprocess.run([str(out/'routing-tests')],check=True,cwd=root)
subprocess.run([str(out/'routing-tests'),'publication'],check=True,cwd=root)
subprocess.run([str(out/'routing-tests'),'offline'],check=True,cwd=root)
contact_tests=compile(root/'scripts/native_fixtures/contacts.cpp')
subprocess.run(['g++',*shared,contact_tests,'-lcrypto','-o',str(out/'contact-tests')],check=True)
subprocess.run([str(out/'contact-tests')],check=True,cwd=root)
public_objects=[compile(root/'src/public_relays.cpp'),compile(root/'scripts/native_fixtures/public_relays.cpp')]
subprocess.run(['g++',*shared,*public_objects,'-lcrypto','-o',str(out/'public-relay-tests')],check=True)
subprocess.run([str(out/'public-relay-tests')],check=True,cwd=root)
