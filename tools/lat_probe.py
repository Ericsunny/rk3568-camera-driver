"""lat_probe.py —— 端到端延迟探针(在板子上跑 loopback,与服务端同一 CLOCK_MONOTONIC)
用法: python3 lat_probe.py [host] [port] [seconds]
输出: 每个 RTP 帧尾包的 到达时刻 - 采集时刻 = 编码+打包+传输 延迟,给 min/avg/p50/p95/max
"""
import socket, sys, time, re

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8554
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 6.0

s = socket.create_connection((host, port), timeout=10)
s.settimeout(10)
buf = b""


def recv_resp():
    global buf
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            raise RuntimeError("连接关闭")
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    buf = rest
    txt = head.decode("latin1")
    m = re.search(r"Content-Length:\s*(\d+)", txt, re.I)
    need = int(m.group(1)) if m else 0
    while len(buf) < need:
        d = s.recv(4096)
        if not d:
            break
        buf += d
    buf = buf[need:]
    return txt


s.sendall(b"OPTIONS rtsp://%s:%d/live RTSP/1.0\r\nCSeq: 1\r\n\r\n" % (host.encode(), port))
recv_resp()
s.sendall(b"DESCRIBE rtsp://%s:%d/live RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n" % (host.encode(), port))
sdp = recv_resp()
ch = 0
m = re.search(r"interleaved=(\d+)", sdp)
if m:
    ch = int(m.group(1))
s.sendall(("SETUP rtsp://%s:%d/live/trackID=0 RTSP/1.0\r\nCSeq: 3\r\n"
           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n" % (host, port)).encode())
recv_resp()
s.sendall(("PLAY rtsp://%s:%d/live RTSP/1.0\r\nCSeq: 4\r\nSession: 0\r\n\r\n" % (host, port)).encode())
recv_resp()

lat = []
frames = 0
t_end = time.monotonic() + secs
while time.monotonic() < t_end:
    if len(buf) < 4:
        d = s.recv(65536)
        if not d:
            break
        buf += d
        continue
    if buf[0:1] != b"$":
        i = buf.find(b"$")
        if i < 0:
            buf = b""
            continue
        buf = buf[i:]
        continue
    if len(buf) < 4:
        continue
    ln = (buf[2] << 8) | buf[3]
    if len(buf) < 4 + ln:
        d = s.recv(65536)
        if not d:
            break
        buf += d
        continue
    pkt = buf[4:4 + ln]
    buf = buf[4 + ln:]
    if len(pkt) < 12:
        continue
    marker = pkt[1] & 0x80
    rtp_ts = int.from_bytes(pkt[4:8], "big")
    if marker:
        frames += 1
        now_ms = time.monotonic() * 1000.0
        lat.append(now_ms - rtp_ts / 90.0)

s.close()
if not lat:
    print("未收到帧")
    sys.exit(1)
lat.sort()
n = len(lat)
print("帧数 %d  延迟: min %.1f  avg %.1f  p50 %.1f  p95 %.1f  max %.1f ms"
      % (n, lat[0], sum(lat) / n, lat[n // 2], lat[int(n * 0.95)], lat[-1]))
