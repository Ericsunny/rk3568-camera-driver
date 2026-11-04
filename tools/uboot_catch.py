#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""uboot_catch.py — 登入板子 -> reboot -> 从头灌键拦截 U-Boot,停在 => 提示符。
只做拦截,不刷写、不改启动配置;检测到 => 后立即停止发送。"""
import sys, time
import serial

PORT, BAUD = "COM7", 1500000
FLOOD_MAX = 60.0          # 灌键最长秒数
TAIL = 6.0                # 见到 => 后静默观察秒数

log = bytearray()

def mark(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

def pump(s, seconds):
    """读串口 seconds 秒,写进 log 并回显"""
    end = time.time() + seconds
    while time.time() < end:
        d = s.read(4096)
        if d:
            log.extend(d)
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()

def wait_for(s, needle_bytes, timeout):
    end = time.time() + timeout
    while time.time() < end:
        d = s.read(4096)
        if d:
            log.extend(d)
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
            if needle_bytes in log:
                return True
    return False

def login(s):
    s.reset_input_buffer(); log.clear()
    s.write(b"\n"); time.sleep(0.5)
    if wait_for(s, b"#", 4):          # 可能已是 shell
        mark("已是 shell 提示符")
        return True
    if b"login:" not in bytes(log):
        mark("没等到 login 提示符,再发一个回车")
        s.write(b"\n"); pump(s, 1.0)
    for user, pwd in ((b"root", b"root"), (b"yckj", b"111111")):
        mark(f"尝试登录 {user.decode()}")
        s.write(user + b"\n"); pump(s, 1.0)
        s.write(pwd + b"\n"); time.sleep(0.3)
        if wait_for(s, b"#", 5):
            tail = bytes(log[-200:]).decode("utf-8", "replace")
            if "incorrect" in tail or "Password" in tail.splitlines()[-1]:
                continue
            mark(f"登录成功({user.decode()})")
            return True
        if b"Login incorrect" in bytes(log):
            mark(f"{user.decode()} 密码不对,换下一组")
            log.clear()
    return False

def main():
    s = serial.Serial()
    s.port, s.baudrate, s.bytesize, s.parity, s.stopbits = PORT, BAUD, 8, "N", 1
    s.timeout, s.write_timeout = 0.05, 2
    try:
        s.open()
    except serial.SerialException as e:
        sys.exit(f"[!] 打开 {PORT} 失败:{e}(串口被占用?)")
    s.dtr = s.rts = False
    mark(f"已打开 {PORT}@{BAUD}")

    if not login(s):
        sys.exit("[!] 登录失败,无法发起 reboot")

    mark("发送 reboot,随即开始灌键(回车/Ctrl+C 交替,30ms 间隔)")
    s.write(b"reboot\n")
    t0 = time.time()
    seen_uboot_banner = False
    while time.time() - t0 < FLOOD_MAX:
        s.write(b"\n"); time.sleep(0.03)
        s.write(b"\x03"); time.sleep(0.03)
        d = s.read(65536)
        if d:
            log.extend(d)
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
        if not seen_uboot_banner and (b"U-Boot " in bytes(log) or b"Hit any key" in bytes(log)):
            seen_uboot_banner = True
            mark("看到 U-Boot 阶段输出")
        if b"=>" in bytes(log):
            mark(f"拦截成功:看到 => 提示符(灌键 {time.time()-t0:.1f}s),停止发送")
            break
    else:
        sys.exit("[!] 灌键 60s 未见到 =>。日志尾见上,板子可能已进系统。")

    pump(s, TAIL)                     # 只听不说,确认提示符稳定
    tail = bytes(log[-400:]).decode("utf-8", "replace")
    print("\n===== 日志尾部 =====\n" + tail)
    s.close()
    mark("串口已安全关闭,板子停在 U-Boot => 提示符(未发任何命令)")

if __name__ == "__main__":
    main()
