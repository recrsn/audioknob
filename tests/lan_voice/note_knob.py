#!/usr/bin/env python3
"""Send a knob a voice note from this Mac, as another knob would.

    python3 tests/lan_voice/note_knob.py <knob-ip> [--text "..."] [--seconds N]
    python3 tests/lan_voice/note_knob.py --listen [--minutes N]

Speaks the text with macOS `say`, encodes it as the knob does (Ogg Opus,
16 kHz mono, one 20 ms packet per page) and sends it as SET_PARAMETER /note,
printing the knob's reply: 200 once saved. The note shows up in a thread from
"Mac test caller". Needs Homebrew's opus and ffmpeg.

With --listen the Mac advertises itself as that knob and takes notes sent to
it (saved as note-<id>.ogg here) and played receipts, printing each.
"""
import argparse, ctypes, os, random, socket, struct, subprocess, tempfile

RATE, FRAME = 16000, 320
OPUS_APPLICATION_VOIP = 2048
PRESKIP = 312  # 48 kHz samples, as libopus reports at 16 kHz × 3
KEY = '0a1b2c3d4e5f'

opus = ctypes.CDLL('/opt/homebrew/lib/libopus.dylib')
opus.opus_encoder_create.restype = ctypes.c_void_p
opus.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_char_p,
                             ctypes.c_int32]


def crc32(data):
    crc = 0
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7 if crc & 0x80000000 else crc << 1) & 0xFFFFFFFF
    return crc


def page(serial, seq, granule, flags, packet):
    lacing = bytes([255] * (len(packet) // 255) + [len(packet) % 255])
    head = b'OggS' + struct.pack('<BBqIII', 0, flags, granule, serial, seq, 0) + bytes([len(lacing)]) + lacing
    raw = head + packet
    return raw[:22] + struct.pack('<I', crc32(raw)) + raw[26:]


def speech(text, seconds):
    with tempfile.TemporaryDirectory() as tmp:
        aiff, raw = os.path.join(tmp, 's.aiff'), os.path.join(tmp, 's.raw')
        subprocess.run(['say', '-o', aiff, text], check=True)
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', aiff, '-t', str(seconds), '-ar', str(RATE), '-ac', '1',
                        '-f', 's16le', raw], check=True)
        pcm = open(raw, 'rb').read()
    return pcm + bytes(-len(pcm) % (FRAME * 2))


def ogg_opus(pcm):
    err = ctypes.c_int()
    enc = opus.opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, ctypes.byref(err))
    assert not err.value
    serial = random.getrandbits(32)
    head = b'OpusHead' + struct.pack('<BBHIhB', 1, 1, PRESKIP, RATE, 0, 0)
    tags = b'OpusTags' + struct.pack('<I', 9) + b'AudioKnob' + struct.pack('<I', 0)
    out = [page(serial, 0, 0, 2, head), page(serial, 1, 0, 0, tags)]
    frames = len(pcm) // (FRAME * 2)
    levels = []
    buf = ctypes.create_string_buffer(1275)
    for i in range(frames):
        chunk = pcm[i * FRAME * 2:(i + 1) * FRAME * 2]
        samples = (ctypes.c_int16 * FRAME).from_buffer_copy(chunk)
        n = opus.opus_encode(enc, samples, FRAME, buf, 1275)
        assert n > 0
        out.append(page(serial, i + 2, (i + 1) * 960, 4 if i == frames - 1 else 0, buf.raw[:n]))
        peak = max(abs(s) for s in samples)
        levels.append(min(100, peak * 100 // 16384))
    wave = [max(levels[j * len(levels) // 24:(j + 1) * len(levels) // 24] or [0]) for j in range(24)]
    return b''.join(out), bytes(wave), frames


def send(ip, ogg, wave):
    note_id = '%016x' % random.getrandbits(64)
    headers = ['SET_PARAMETER /note RTSP/1.0', 'CSeq: 1', f'Content-Length: {len(ogg)}',
               'Content-Type: audio/ogg', f'X-Note-Id: {note_id}', f'X-Note-Wave: {wave.hex()}',
               'X-AudioKnob-Name: Mac test caller', f'X-AudioKnob-Id: {KEY}', 'User-Agent: AudioKnob Talk/3']
    with socket.create_connection((ip, 42424), timeout=25) as sock:
        sock.sendall(('\r\n'.join(headers) + '\r\n\r\n').encode() + ogg)
        reply = b''
        while b'\r\n\r\n' not in reply:
            chunk = sock.recv(1024)
            if not chunk:
                break
            reply += chunk
    return note_id, reply.split(b'\r\n', 1)[0].decode(errors='replace')


def read_request(conn):
    data = b''
    while b'\r\n\r\n' not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None, {}, b''
        data += chunk
    head, body = data.split(b'\r\n\r\n', 1)
    lines = head.decode(errors='replace').split('\r\n')
    headers = {k.strip().lower(): v.strip() for k, v in (l.split(':', 1) for l in lines[1:] if ':' in l)}
    while len(body) < int(headers.get('content-length', 0)):
        chunk = conn.recv(65536)
        if not chunk:
            break
        body += chunk
    return lines[0], headers, body


def listen(minutes):
    ad = subprocess.Popen(['dns-sd', '-R', 'Mac test caller', '_audioknob._tcp', 'local', '42424', 'v=3',
                           'name=Mac test caller', f'id={KEY}'], stdout=subprocess.DEVNULL)
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('', 42424))
    server.listen(4)
    server.settimeout(minutes * 60)
    print(f'listening as "Mac test caller" ({KEY}) for {minutes} min')
    try:
        while True:
            conn, (ip, _) = server.accept()
            with conn:
                conn.settimeout(20)
                line, headers, body = read_request(conn)
                if not line:
                    continue
                note_id, code = headers.get('x-note-id', ''), 200
                if line.startswith('SET_PARAMETER /note/played'):
                    print(f'{ip} played {note_id}')
                elif line.startswith('SET_PARAMETER /note'):
                    name = f'note-{note_id}.ogg'
                    open(name, 'wb').write(body)
                    print(f'{ip} ({headers.get("x-audioknob-name")}) sent {note_id}: {len(body)} bytes -> {name}')
                else:
                    print(f'{ip}: {line} (not handled)')
                    code = 501
                reason = 'OK' if code == 200 else 'Not Implemented'
                conn.sendall(f'RTSP/1.0 {code} {reason}\r\nCSeq: {headers.get("cseq", "1")}\r\n'
                             f'Content-Length: 0\r\n\r\n'.encode())
    except socket.timeout:
        pass
    finally:
        ad.terminate()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip', nargs='?')
    ap.add_argument('--listen', action='store_true')
    ap.add_argument('--minutes', type=float, default=10)
    ap.add_argument('--text', default='Hi, this is a voice note from the Mac. Can you hear me?')
    ap.add_argument('--seconds', type=float, default=60)
    args = ap.parse_args()
    if args.listen:
        return listen(args.minutes)
    if not args.ip:
        ap.error('knob ip required')
    ogg, wave, frames = ogg_opus(speech(args.text, args.seconds))
    note_id, status = send(args.ip, ogg, wave)
    print(f'note {note_id}: {frames * 20 / 1000:.1f} s, {len(ogg)} bytes -> {status}')


if __name__ == '__main__':
    main()
