#!/usr/bin/env python3
"""Call a knob from this Mac, as another knob would, and record the call.

    python3 tests/lan_voice/call_knob.py <knob-ip> [--out call.wav]

Accept the call on the knob. The Mac sends silence, then speech (macOS `say`),
then silence again, and saves what the knob sends back. With nobody talking
near the knob, what comes back while the speech plays is the echo left after
the knob's echo canceller, so the script reports the received level in each
phase. Needs Homebrew's opus and ffmpeg.
"""
import argparse, ctypes, math, os, random, re, socket, struct, subprocess, sys, tempfile, threading, time, wave

RATE, FRAME = 16000, 320  # 20 ms
RTSP_PORT = 42424
PHASES = [('silence', 3.0), ('speech', None), ('silence', 4.0)]

opus = ctypes.CDLL('/opt/homebrew/lib/libopus.dylib')
opus.opus_encoder_create.restype = ctypes.c_void_p
opus.opus_decoder_create.restype = ctypes.c_void_p
opus.opus_encode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_char_p, ctypes.c_int32]
opus.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                             ctypes.c_int]
OPUS_APPLICATION_VOIP = 2048



def speech_pcm():
    with tempfile.TemporaryDirectory() as tmp:
        aiff, raw = os.path.join(tmp, 's.aiff'), os.path.join(tmp, 's.raw')
        text = ('This is a test call from the Mac. One, two, three, four, five. '
                'The quick brown fox jumps over the lazy dog. Can you hear me clearly?')
        subprocess.run(['say', '-o', aiff, text], check=True)
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', aiff, '-ar', str(RATE), '-ac', '1', '-f', 's16le', raw],
                       check=True)
        return open(raw, 'rb').read()


def rms_db(samples):
    if not samples:
        return float('-inf')
    return 20 * math.log10(math.sqrt(sum(s * s for s in samples) / len(samples)) / 32768 + 1e-9)


class Rtsp:
    def __init__(self, ip):
        self.sock = socket.create_connection((ip, RTSP_PORT), timeout=3)
        self.cseq = 0
        self.buf = b''

    def request(self, method, headers=None, body=b''):
        self.cseq += 1
        lines = [f'{method} /talk RTSP/1.0', f'CSeq: {self.cseq}', f'Content-Length: {len(body)}',
                 'X-AudioKnob-Name: Mac test caller', 'User-Agent: AudioKnob Talk/2']
        lines += [f'{k}: {v}' for k, v in (headers or {}).items()]
        self.sock.sendall(('\r\n'.join(lines) + '\r\n\r\n').encode() + body)

    def reply(self, timeout):
        self.sock.settimeout(timeout)
        while b'\r\n\r\n' not in self.buf:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError('knob closed the connection')
            self.buf += data
        head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        lines = head.decode().split('\r\n')
        code = int(lines[0].split()[1])
        headers = {k.lower(): v.strip() for k, v in (l.split(':', 1) for l in lines[1:] if ':' in l)}
        return code, headers


def rtp_payload(data):
    if len(data) < 12 or data[0] >> 6 != 2:
        return None
    offset = 12 + 4 * (data[0] & 15)
    if data[0] & 0x10:
        offset += 4 + 4 * struct.unpack('>H', data[offset + 2:offset + 4])[0]
    end = len(data) - (data[-1] if data[0] & 0x20 else 0)
    seq, ts, ssrc = struct.unpack('>HII', data[2:12])
    return seq, ssrc, data[offset:end]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip')
    ap.add_argument('--out', default='call.wav')
    args = ap.parse_args()

    speech = speech_pcm()
    plan = []  # (phase, pcm bytes)
    for name, secs in PHASES:
        pcm = speech if name == 'speech' else bytes(2 * int(RATE * secs))
        pcm += bytes(-len(pcm) % (2 * FRAME))
        plan.append((name, pcm))

    media = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    media.bind(('0.0.0.0', 0))
    port = media.getsockname()[1]
    local_ip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    local_ip.connect((args.ip, 9))
    me = local_ip.getsockname()[0]

    ctl = Rtsp(args.ip)
    sdp = (f'v=0\r\no=- {random.getrandbits(31)} 1 IN IP4 {me}\r\ns=AudioKnob Talk\r\nc=IN IP4 {me}\r\nt=0 0\r\n'
           f'm=audio {port} RTP/AVP 96\r\na=rtpmap:96 opus/48000/2\r\na=fmtp:96 stereo=0;sprop-stereo=0;'
           'maxplaybackrate=16000;sprop-maxcapturerate=16000\r\na=ptime:20\r\na=sendrecv\r\n').encode()
    ctl.request('ANNOUNCE', {'Content-Type': 'application/sdp'}, sdp)
    code, _ = ctl.reply(3)
    assert code == 200, f'ANNOUNCE {code}'
    ctl.request('SETUP', {'Transport': f'RTP/AVP/UDP;unicast;client_port={port}'})
    code, h = ctl.reply(3)
    assert code == 200, f'SETUP {code}'
    session = h['session']
    server_port = int(re.search(r'server_port=(\d+)', h['transport']).group(1))
    seq, ts, ssrc = random.getrandbits(16), random.getrandbits(32), random.getrandbits(32)
    ctl.request('RECORD', {'Session': session, 'RTP-Info': f'seq={seq};rtptime={ts}'})
    print(f'Ringing {args.ip}: accept on the knob...', flush=True)
    t_ring = time.monotonic()
    code, h = ctl.reply(40)
    print(f'RECORD -> {code} after {time.monotonic() - t_ring:.1f} s {h.get("rtp-info", "")}', flush=True)
    if code != 200:
        sys.exit({453: 'busy', 480: 'no answer', 603: 'declined'}.get(code, 'failed'))

    received = []  # (arrival, seq, payload)
    stop = threading.Event()

    def receiver():
        media.settimeout(0.2)
        peer_ssrc = None
        while not stop.is_set():
            try:
                data, src = media.recvfrom(2048)
            except socket.timeout:
                continue
            p = rtp_payload(data)
            if not p or src[0] != args.ip or data[1] & 0x7f != 96:
                continue
            peer_ssrc = peer_ssrc or p[1]
            if p[1] == peer_ssrc:
                received.append((time.monotonic(), p[0], p[2]))

    rx = threading.Thread(target=receiver, daemon=True)
    rx.start()

    start = time.monotonic()
    windows = []  # (phase, t0, t1) relative to start
    frame_no = 0
    err = ctypes.c_int()
    enc = opus.opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, ctypes.byref(err))
    out = ctypes.create_string_buffer(1275)
    for name, pcm in plan:
        t0 = frame_no * 0.02
        for i in range(0, len(pcm), 2 * FRAME):
            frame = (ctypes.c_int16 * FRAME).from_buffer_copy(pcm[i:i + 2 * FRAME])
            n = opus.opus_encode(enc, frame, FRAME, out, 1275)
            header = struct.pack('>BBHII', 0x80, 96, seq & 0xffff, ts & 0xffffffff, ssrc)
            media.sendto(header + out.raw[:n], (args.ip, server_port))
            seq += 1
            ts += 960  # RFC 7587's 48 kHz clock
            frame_no += 1
            delay = start + frame_no * 0.02 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        windows.append((name, t0, frame_no * 0.02))
        print(f'  sent {name} {t0:.1f}-{frame_no * 0.02:.1f} s', flush=True)
    ctl.sock.close()  # hangs up
    time.sleep(0.5)
    stop.set()
    rx.join()

    if not received:
        sys.exit('received no audio from the knob')
    by_seq = {}
    first_seq = received[0][1]
    for arrival, s, payload in received:
        by_seq[(s - first_seq) & 0xffff] = (arrival, payload)
    last = max(by_seq)
    dec = opus.opus_decoder_create(RATE, 1, ctypes.byref(err))
    pcm = (ctypes.c_int16 * FRAME)()
    samples, lost = [], 0
    for k in range(last + 1):
        if k in by_seq:
            n = opus.opus_decode(dec, by_seq[k][1], len(by_seq[k][1]), pcm, FRAME, 0)
        else:
            lost += 1
            n = opus.opus_decode(dec, None, 0, pcm, FRAME, 0)
        samples.extend(pcm[:max(n, 0)])
    with wave.open(args.out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack(f'<{len(samples)}h', *samples))

    # Place the knob's frames on our clock by arrival, so each phase's level
    # is what came back while it played (one-way delay is a few frames).
    t_first = received[0][0] - start
    print(f'Received {len(received)} packets, {lost} lost, first {t_first:.2f} s after answer; saved {args.out}')
    for name, t0, t1 in windows:
        # Skip the first 300 ms of each phase: playback and the echo trail lag it.
        a, b = t0 + 0.3, t1
        frames = [k for k in range(last + 1) if a <= t_first + k * 0.02 < b]
        chunk = [x for k in frames for x in samples[k * FRAME:(k + 1) * FRAME]]
        print(f'  {name:8s} {t0:5.1f}-{t1:5.1f} s: knob sent {rms_db(chunk):6.1f} dBFS RMS')


if __name__ == '__main__':
    main()
