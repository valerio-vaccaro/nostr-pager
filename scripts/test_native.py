#!/usr/bin/env python3
"""Run protocol tests without a board; requires GCC/G++."""
from pathlib import Path
import subprocess
from concurrent.futures import ThreadPoolExecutor
root=Path(__file__).resolve().parents[1]
out=root/'.build-cache/native';out.mkdir(parents=True,exist_ok=True)
flags=['-O1','-g','-DUSE_STDONLY','-I'+str(root/'lib/uBitcoin/src'),'-I'+str(root/'include')]
sources=list((root/'lib/uBitcoin/src').rglob('*.c'))+list((root/'lib/uBitcoin/src').rglob('*.cpp'))+[root/'src/key_codec.cpp',root/'scripts/native_fixtures/main.cpp']
def compile(source):
    obj=out/('_'.join(source.relative_to(root).parts)+'.o')
    subprocess.run(['gcc' if source.suffix=='.c' else 'g++',*flags,'-c',str(source),'-o',str(obj)],check=True,cwd=root)
    return str(obj)
with ThreadPoolExecutor(max_workers=4) as pool:
    objects=list(pool.map(compile,sources))
subprocess.run(['g++',*objects,'-o',str(out/'protocol-tests')],check=True,cwd=root)
subprocess.run([str(out/'protocol-tests')],check=True,cwd=root)
