#!/usr/bin/env python3
"""Build and flash a knob for unattended testing.

By default the S3 image is built with AK_DEV_AUTO_ACCEPT (core/dev.h), which installs
uploaded firmware and answers Talk calls without a press on the device.
That press is all that guards firmware installs from the unauthenticated web
page, so put a dev knob back on a normal build with --release when done.

  tools/dev_flash.py --usb /dev/cu.usbmodem2101      # S3 over USB
  tools/dev_flash.py --ota 192.168.50.166            # S3 over the web page
  tools/dev_flash.py --ota 192.168.50.166 --esp32    # Bluetooth chip, relayed by the S3
  tools/dev_flash.py --ota 192.168.50.166 --release  # back to a normal build

An OTA upload to a knob not yet on a dev build still waits for a press there.
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV_LOCAL = os.path.join(ROOT, 'firmware', 's3', 'src', 'core', 'dev_local.h')


def build(esp32, release, upload_port=None):
    project, env = ('esp32', 'esp32') if esp32 else ('s3', 's3')
    cmd = ['pio', 'run', '-d', os.path.join(ROOT, 'firmware', project), '-e', env]
    if upload_port:
        cmd += ['-t', 'upload', '--upload-port', upload_port]
    dev = not esp32 and not release
    print('+', ' '.join(cmd), '(dev build)' if dev else '')
    # Only for this build, so a plain `pio run` never picks it up. Only the
    # files that include core/dev.h recompile when it comes or goes.
    if dev:
        with open(DEV_LOCAL, 'w') as f:
            f.write('// Written by tools/dev_flash.py for one build; see core/dev.h.\n#define AK_DEV_AUTO_ACCEPT 1\n')
    try:
        if subprocess.run(cmd).returncode:
            sys.exit('build failed')
    finally:
        if os.path.exists(DEV_LOCAL):
            os.remove(DEV_LOCAL)
    return os.path.join(ROOT, 'firmware', project, '.pio', 'build', env, 'firmware.bin')


def state(host, timeout=3):
    with urllib.request.urlopen(f'http://{host}/api/state', timeout=timeout) as r:
        return json.load(r)


def upload(host, image):
    with open(image, 'rb') as f:
        body = f.read()
    req = urllib.request.Request(f'http://{host}/api/update', data=body, method='POST',
                                 headers={'Content-Type': 'application/octet-stream'})
    print(f'uploading {len(body) // 1024} KB to {host}')
    with urllib.request.urlopen(req, timeout=300) as r:
        r.read()


def wait_installed(host, limit_s):
    deadline = time.time() + limit_s
    last = None
    # 'installed' lingers from the previous update until this one shows.
    started = False
    while time.time() < deadline:
        try:
            s = state(host)
        except OSError:
            s = None  # restarting
            started = True
        if s:
            u = s.get('update', {})
            seen = (u.get('state'), u.get('progress'))
            if seen != last:
                print(f"  {u.get('state')} {u.get('progress', '')}")
                last = seen
            started = started or u.get('state') in ('receiving', 'ready', 'installing')
            if started and u.get('state') == 'installed':
                print(f"installed: S3 {s.get('version')}, Bluetooth {s.get('btVersion')}"
                      f"{', dev build' if s.get('devAutoAccept') else ''}")
                return
            if u.get('state') in ('failed', 'declined'):
                sys.exit(f"update {u.get('state')}: {u.get('error', '')}")
        time.sleep(1)
    sys.exit('timed out waiting for the install')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument('--usb', metavar='PORT', help='serial port to flash over USB')
    where.add_argument('--ota', metavar='HOST', help="knob's IP address, to upload through its web page")
    ap.add_argument('--esp32', action='store_true', help='the Bluetooth chip instead of the S3')
    ap.add_argument('--release', action='store_true', help='a normal build, without auto-accept')
    args = ap.parse_args()

    if args.usb:
        build(args.esp32, args.release, upload_port=args.usb)
        return
    before = state(args.ota)
    if not before.get('devAutoAccept'):
        print(f"{before.get('name')} isn't on a dev build: accept the update on the device")
    upload(args.ota, build(args.esp32, args.release))
    wait_installed(args.ota, 300)


if __name__ == '__main__':
    main()
