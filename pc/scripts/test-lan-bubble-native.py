#!/usr/bin/env python3
"""Compile production controllers against synthetic in-memory transport, using prebuilt rlibs only."""
import argparse, subprocess
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--deps',type=Path,required=True)
p.add_argument('--rustc',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
a.output.mkdir(parents=True,exist_ok=False);(a.output/'usb_serial').mkdir()
(a.output/'usb_serial/production_shared.rs').write_bytes((root/'src-tauri/src/usb_serial/bubble_pixels.rs').read_bytes())
(a.output/'production_lan.rs').write_text((root/'src-tauri/src/lan_agent/bubble_pixels.rs').read_text()+'\n'+(root/'tests/lan_bubble_cases.inc').read_text())
(a.output/'fixture.rs').write_bytes((root/'tests/lan_bubble_transfer_host.rs').read_bytes())
command=[str(a.rustc),'--edition=2021','--test',str(a.output/'fixture.rs'),'-L','dependency='+str(a.deps),'-o',str(a.output/'lan-bubble-tests')]
for name in ('serde','serde_json','base64','uuid'):
    paths=list(a.deps.glob('lib'+name+'-*.rlib'))
    if not paths:raise SystemExit('Missing prebuilt dependency: '+name)
    command+=['--extern',name+'='+str(max(paths,key=lambda f:f.stat().st_mtime_ns))]
subprocess.run(command,check=True)
subprocess.run([str(a.output/'lan-bubble-tests'),'--test-threads=1'],check=True,timeout=30)
# Compile the actual authenticated exchange too, with only loopback sockets and
# synthetic keys. Retain the existing shared exchange for other control callers.
s=(root/'src-tauri/src/lan_agent.rs').read_text()
functions=[]
for name in ('session_digest','key_from_digest','nonce','read_before','exchange_link_bounded'):
    start=s.index('fn '+name+'(');end=s.index('\n}',start)+2
    functions.append(s[start:end])
(a.output/'production_auth.inc').write_text('\n\n'.join(functions)+'\n')
(a.output/'auth.rs').write_bytes((root/'tests/lan_bubble_auth_host.rs').read_bytes())
command=[str(a.rustc),'--edition=2021','--test',str(a.output/'auth.rs'),'-L','dependency='+str(a.deps),'-o',str(a.output/'lan-auth-tests')]
for name in ('ring','zeroize'):
    paths=list(a.deps.glob('lib'+name+'-*.rlib'))
    if not paths:raise SystemExit('Missing prebuilt dependency: '+name)
    command+=['--extern',name+'='+str(max(paths,key=lambda f:f.stat().st_mtime_ns))]
subprocess.run(command,check=True)
subprocess.run([str(a.output/'lan-auth-tests'),'--test-threads=1'],check=True,timeout=10)
