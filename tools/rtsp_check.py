#!/usr/bin/env python3
"""
rtsp_check.py —— RTSP 单客户端验收脚本

用法:
  ./rtsp_check.py [host] [port] [seconds]

检查项(任何一项失败都会打印 FAIL 并以非 0 退出):
  1. OPTIONS / DESCRIBE / SETUP / PLAY 四步都返回 200
  2. DESCRIBE 的 **响应体** 是 SDP(必须按 Content-Length 单独读,不能从响应头里找)
  3. SDP 含 m=video、a=rtpmap:96 H264/90000、sprop-parameter-sets
  4. SDP 里 profile-level-id 与 sprop 解出的 SPS 字节一致
  5. 收流若干秒:统计 RTP 包数/字节数/序号缺口/关键帧数/实测帧率

设计说明(踩过的坑):
  - RTSP 响应分「头」和「体」两部分,体长度由 Content-Length 给出。
    只在 recv 到的那一坨字节里找 "v=0" 会漏 —— 头和体常常在同一次 recv 里,
    但 TCP 不保证,必须按 Content-Length 精确读取剩余部分。老版本正是在这里
    打印 SDP_MISS 误报。
  - 服务器走 TCP interleaved(RTP over RTSP),数据帧以 b'$' + 1B channel + 2B len 开头,
    与 RTSP 响应共用一条连接,所以必须先读完响应体再切到帧模式。
"""

import base64
import socket
import struct
import sys
import time

TIMEOUT = 8.0


def recv_until(sock, marker):
    """读到包含 marker 为止,返回已读字节。"""
    buf = b""
    while marker not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed while waiting for %r" % marker)
        buf += chunk
    return buf


def parse_response(raw):
    """拆出状态码/头/已读到的体片段。"""
    head, _, rest = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    status = lines[0].decode("latin-1")
    code = int(status.split()[1]) if len(status.split()) > 1 else 0
    headers = {}
    for ln in lines[1:]:
        if b":" in ln:
            k, v = ln.split(b":", 1)
            headers[k.strip().lower().decode()] = v.strip().decode("latin-1")
    return code, headers, rest


def read_response(sock):
    """完整读一个 RTSP 响应:读到响应头,再按 Content-Length 读完响应体。"""
    raw = recv_until(sock, b"\r\n\r\n")
    code, headers, rest = parse_response(raw)
    need = int(headers.get("content-length", "0") or 0)
    while len(rest) < need:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return code, headers, rest[:need]


def request(sock, text):
    sock.sendall(text.encode())
    return read_response(sock)


def sps_from_sprop(sprop):
    """从 sprop-parameter-sets 取 SPS,返回 (profile_level_id_hex, 字节串)。"""
    first = sprop.split(",")[0]
    data = base64.b64decode(first)
    # NAL: 0x67 (SPS) profile_idc level_idc ... -> profile-level-id = 67 PP LL
    if len(data) < 4 or data[0] & 0x1F != 7:
        raise RuntimeError("sprop 第一段不是 SPS: %s" % data[:4].hex())
    # profile-level-id 取 SPS 的第 2~4 字节(跳过 NAL 头 0x67):
    # 0x64=High profile, 0x10=constraint flags, 0x33=level 5.1 -> "641033"
    return "%02x%02x%02x" % (data[1], data[2], data[3]), data


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8554
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    url = "rtsp://%s:%d/live" % (host, port)

    sock = socket.create_connection((host, port), timeout=TIMEOUT)
    sock.settimeout(TIMEOUT)
    fails = []

    # 1. OPTIONS
    code, _, _ = request(sock, "OPTIONS %s RTSP/1.0\r\nCSeq: 1\r\n\r\n" % url)
    print("OPTIONS  : %d %s" % (code, "OK" if code == 200 else "FAIL"))
    if code != 200:
        fails.append("OPTIONS")

    # 2. DESCRIBE —— 体按 Content-Length 精确读
    code, headers, body = request(
        sock,
        "DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n" % url,
    )
    sdp = body.decode("utf-8", "replace")
    print("DESCRIBE : %d %s (sdp %d 字节)" % (code, "OK" if code == 200 else "FAIL", len(body)))
    if code != 200:
        fails.append("DESCRIBE")
    if not sdp.startswith("v="):
        print("SDP_FAIL : 响应体不是 SDP(前 40 字节: %r)" % body[:40])
        fails.append("SDP_BODY")
    else:
        print("SDP_BODY : OK")

    # 3. SDP 关键字段
    fmtp = ""
    for line in sdp.splitlines():
        if line.startswith("a=fmtp:"):
            fmtp = line
            break
    have_video = "m=video" in sdp
    have_rtpmap = "a=rtpmap:96 H264/90000" in sdp
    sprop = ""
    for item in fmtp.split(";"):
        if item.strip().startswith("sprop-parameter-sets="):
            sprop = item.split("=", 1)[1].split(";")[0].strip()
    print("SDP_MEDIA: m=video=%s rtpmap=%s sprop=%s"
          % (have_video, have_rtpmap, "有" if sprop else "无"))
    for name, ok in (("m=video", have_video), ("rtpmap", have_rtpmap), ("sprop", bool(sprop))):
        if not ok:
            fails.append("SDP_" + name)

    # 4. profile-level-id 自洽性(SDP 声明的 vs 从 SPS 字节解出的)
    if sprop:
        try:
            pli, _ = sps_from_sprop(sprop)
            declared = ""
            for item in fmtp.split(";"):
                if item.strip().startswith("profile-level-id="):
                    declared = item.split("=", 1)[1].split(";")[0].strip()
            ok = (declared.lower() == pli)
            print("SDP_PLI  : 声明=%s SPS解出=%s %s"
                  % (declared or "(无)", pli, "一致" if ok else "不一致"))
            if not ok:
                fails.append("SDP_PLI")
        except Exception as e:  # noqa: BLE001
            print("SDP_PLI  : 解析失败 %s" % e)
            fails.append("SDP_PLI")

    # 5. SETUP / PLAY
    code, _, _ = request(
        sock,
        "SETUP %s/trackID=0 RTSP/1.0\r\nCSeq: 3\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n" % url,
    )
    print("SETUP    : %d %s" % (code, "OK" if code == 200 else "FAIL"))
    if code != 200:
        fails.append("SETUP")

    code, _, _ = request(sock, "PLAY %s RTSP/1.0\r\nCSeq: 4\r\nRange: npt=0.000-\r\n\r\n" % url)
    print("PLAY     : %d %s" % (code, "OK" if code == 200 else "FAIL"))
    if code != 200:
        fails.append("PLAY")
        print("RESULT: FAIL %s" % ",".join(fails))
        return 1

    # 6. 收流:TCP interleaved 帧
    sock.settimeout(1.0)
    deadline = time.time() + seconds
    buf = b""
    pkts = nbytes = 0
    seqs = []
    markers = 0
    frames = 0
    ts_first = ts_last = None
    while time.time() < deadline:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            continue
        while True:
            # 跳过非 '$' 的杂字节(可能是响应残留)
            idx = buf.find(b"$")
            if idx < 0:
                buf = b""
                break
            buf = buf[idx:]
            if len(buf) < 4:
                break
            plen = struct.unpack(">H", buf[2:4])[0]
            if len(buf) < 4 + plen:
                break
            payload = buf[4:4 + plen]
            buf = buf[4 + plen:]
            # RTP 头最小 12 字节
            if len(payload) < 12 or (payload[0] >> 6) != 2:
                continue
            pkts += 1
            nbytes += plen
            seq = struct.unpack(">H", payload[2:4])[0]
            ts = struct.unpack(">I", payload[4:8])[0]
            marker = payload[1] >> 7
            seqs.append(seq)
            if ts_first is None:
                ts_first = ts
            ts_last = ts
            if marker:
                markers += 1
                frames += 1

    gaps = 0
    for a, b in zip(seqs, seqs[1:]):
        d = (b - a) & 0xFFFF
        if d != 1:
            gaps += 1
    fps = 0.0
    if ts_first is not None and ts_last is not None and ts_last > ts_first:
        fps = (ts_last - ts_first) / 90000.0
        fps = (frames - 1) / fps if fps > 0 else 0.0

    print("RESULT pkts=%d bytes=%d seq_first=%s seq_last=%s gaps=%d markers=%d fps=%.2f"
          % (pkts, nbytes, seqs[0] if seqs else "-", seqs[-1] if seqs else "-", gaps, markers, fps))
    if pkts == 0:
        fails.append("NO_RTP")
    if gaps:
        fails.append("SEQ_GAP")

    # 7. TEARDOWN(非致命)
    try:
        sock.settimeout(2.0)
        sock.sendall(b"TEARDOWN %s RTSP/1.0\r\nCSeq: 5\r\n\r\n" % url.encode())
    except OSError:
        pass
    sock.close()

    print("VERDICT  : %s%s" % ("PASS" if not fails else "FAIL ",
                              "" if not fails else ",".join(fails)))
    return 0 if not fails else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001
        print("RESULT: FAIL exception %s" % exc)
        sys.exit(2)
