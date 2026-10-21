#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
with tempfile.TemporaryDirectory(prefix='audioknob-rtp-timeline-') as temp:
    out=Path(temp)/'check'
    subprocess.run(['clang','-std=gnu11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',f'-I{ROOT}/firmware/s3/src',str(HERE/'check.c'),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=60)
