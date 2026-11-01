#!/usr/bin/env python3
"""Collect the transitive ARM ELF dependencies into a private KUAL runtime.
No files in the target device's /lib or /usr/lib are modified.
"""
import argparse, json, re, shutil, subprocess
from pathlib import Path
ap=argparse.ArgumentParser()
ap.add_argument('--sysroot', type=Path, required=True)
ap.add_argument('--binary', type=Path, required=True)
ap.add_argument('--fbink', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
a=ap.parse_args(); root=a.sysroot.resolve(); dest=a.output.resolve()
project=Path(__file__).resolve().parents[1]
(dest/'bin').mkdir(parents=True, exist_ok=True)
(dest/'runtime/lib').mkdir(parents=True, exist_ok=True)
for p in (project/'kindle').iterdir(): shutil.copy2(p, dest/p.name)
shutil.copy2(a.binary, dest/'bin/moonlight-kindle')
shutil.copy2(root/'lib/ld-linux-armhf.so.3', dest/'runtime/ld-linux-armhf.so.3')
search=[root/'usr/lib/arm-linux-gnueabihf',root/'lib/arm-linux-gnueabihf',root/'usr/lib',root/'lib']
queue=[a.binary, a.fbink]; seen=set(); manifest={}
shutil.copy2(a.fbink, dest/'runtime/lib/libfbink.so.1')
while queue:
    p=queue.pop()
    info=subprocess.check_output(['readelf','-d',str(p)],text=True)
    for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]',info):
        if name in seen: continue
        match=next((d/name for d in search if (d/name).is_file()),None)
        if match is None: raise SystemExit('Missing runtime dependency: '+name)
        seen.add(name); shutil.copy2(match,dest/'runtime/lib'/name); queue.append(match)
        manifest[name]=str(match.relative_to(root))
# dlopen resolver support is absent from DT_NEEDED.
for name in ['libnss_dns.so.2','libnss_files.so.2','libresolv.so.2']:
    p=root/'lib/arm-linux-gnueabihf'/name
    if p.exists(): shutil.copy2(p,dest/'runtime/lib'/name); manifest[name]=str(p.relative_to(root))
font_dir=dest/'runtime/fonts'; font_dir.mkdir(exist_ok=True)
shutil.copy2(root/'usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',font_dir/'DejaVuSans.ttf')
(dest/'runtime/fonts.conf').write_text('''<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><dir>/mnt/us/extensions/moonlight/runtime/fonts</dir><cachedir>/mnt/us/extensions/moonlight/data/font-cache</cachedir>
<alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias></fontconfig>
''')
(dest/'runtime/gtkrc').write_text('gtk-theme-name="Raleigh"\ngtk-font-name="DejaVu Sans 14"\n')
(dest/'runtime/loaders.cache').write_text('# No external image loader modules required.\n')
(dest/'runtime-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for p in [dest/'launch.sh',dest/'run.sh',dest/'bin/moonlight-kindle',dest/'runtime/ld-linux-armhf.so.3']:
    p.chmod(0o755)
print(f'{len(seen)} ELF dependencies collected in {dest}')
