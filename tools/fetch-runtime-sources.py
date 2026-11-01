#!/usr/bin/env python3
"""Download exact Debian source packages into an existing destination.
Requires apt configured with Debian bullseye deb-src repositories. Does not use sudo.
Usage: fetch-runtime-sources.py /path/to/runtime-sources.json /path/to/destination
"""
import json, subprocess, sys
from pathlib import Path
manifest=json.loads(Path(sys.argv[1]).read_text()); dest=Path(sys.argv[2]).resolve()
dest.mkdir(parents=True,exist_ok=True)
for name,version in sorted({(p['source'],p['source_version']) for p in manifest}):
    subprocess.run(['apt-get','source','--download-only',f'{name}={version}'],cwd=dest,check=True)
