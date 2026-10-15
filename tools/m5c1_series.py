#!/usr/bin/env python3
"""M5c1: серія «DBG,failnext,1[,decode] + NEXT» через UART0 (pyserial). Лише перевірка на залізі, у збірку не входить.
Використання: python tools/m5c1_series.py COM5 [серій=20] [read|decode]
Після серії друкує DBG,aout / decode / tasks / heap; увесь лог — у m5c1_series.log."""
import sys
import time

import serial

port = sys.argv[1]
count = int(sys.argv[2]) if len(sys.argv) > 2 else 20
mode = sys.argv[3] if len(sys.argv) > 3 else "read"
ser = serial.Serial(port, 460800, timeout=0.1)
log = open("m5c1_series.log", "w", encoding="utf-8")


def drain(sec):
    """Читає рядки sec секунд, пише в лог, повертає список."""
    out, end, buf = [], time.time() + sec, b""
    while time.time() < end:
        buf += ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("utf-8", "replace").rstrip()
            out.append(s)
            log.write(s + "\n")
    return out


def send(cmd):
    ser.write((cmd + "\n").encode())


ok = 0
for i in range(1, count + 1):
    drain(0.2)
    send(f"DBG,failnext,1,{mode}")
    drain(0.4)
    send("NEXT")
    lines = drain(4.0)  # збій -> ERR -> наступний трек -> ≥1 с гри (скидає лічильник помилок)
    err = any("ERR" in s and "play" in s for s in lines)
    starts = sum(1 for s in lines if "play #" in s)
    good = err and starts >= 2
    ok += good
    print(f"серія {i:2}: ERR={'так' if err else 'НІ'} стартів треку={starts} -> {'ok' if good else 'ПЕРЕВІРИТИ'}")
print(f"ok {ok}/{count}")
for cmd in ("DBG,aout", "DBG,decode", "DBG,tasks", "DBG,heap"):
    send(cmd)
    for s in drain(1.5):
        print(s)
