#!/usr/bin/env python3
"""M5c1: серія «DBG,failnext,1[,decode] + NEXT» через UART0 (pyserial). Лише перевірка на залізі, у збірку не входить.
Використання: python tools/m5c1_series.py COM5 [серій=20] [read|decode]
ПОТРІБНЕ СЕРЕДОВИЩЕ: esp32dev_test (збірка з -DSKIP_TEST_CMDS=1). В esp32dev DBG,failnext відповідає «вимкнено».
Перед запуском перезавантажте плату (RST): лічильник failnext живе в RAM і лишається після попередньої серії.
Скрипт сам шле SRC,sd і чекає старту треку (play #). Далі серії, після них DBG,aout / decode / tasks / heap.
Увесь лог — у m5c1_series.log (включно з підсумком ok N/серій). Код виходу: 0 — усі серії ok, 1 — є ПЕРЕВІРИТИ, 2 — передумова.
Потрібен pyserial: pip install pyserial"""
import sys
import time

import serial

port = sys.argv[1]
count = int(sys.argv[2]) if len(sys.argv) > 2 else 20
mode = sys.argv[3] if len(sys.argv) > 3 else "read"
if mode not in ("read", "decode"):
    sys.exit("режим: read або decode")
want = "read_fail" if mode == "read" else "decode_fail"  # код ERR, який шле sd_source при штучному збої

ser = serial.Serial()
ser.port = port
ser.baudrate = 460800
ser.timeout = 0.1
ser.dtr = False  # відкриття порту не скидає плату (як у m5c5)
ser.rts = False
ser.open()
log = open("m5c1_series.log", "w", encoding="utf-8")


def out(text):
    print(text)
    log.write(text + "\n")


def drain(sec):
    """Читає рядки sec секунд, пише в лог, повертає список."""
    res, end, buf = [], time.time() + sec, b""
    while time.time() < end:
        buf += ser.read(512)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("utf-8", "replace").rstrip()
            res.append(s)
            log.write(s + "\n")
    return res


def send(cmd):
    ser.write((cmd + "\n").encode())


def wait_for(pattern, sec):
    """Читає, доки з’явиться рядок із pattern, але не довше sec секунд."""
    end = time.time() + sec
    while time.time() < end:
        if any(pattern in s for s in drain(0.2)):
            return True
    return False


try:
    time.sleep(3)  # стек піднімається після старту
    drain(1)
    send("SRC,sd")  # після scan відновлюється збережений трек
    if not wait_for("play #", 20):
        out("ПОМИЛКА: трек не стартував за 20 с (перевірте картку, DBG,sd і STATE)")
        sys.exit(2)
    drain(1)  # відіграти ≥1 с, щоб лічильник помилок був у штатному стані

    ok = 0
    for i in range(1, count + 1):
        drain(0.2)
        send(f"DBG,failnext,1,{mode}")
        fl = drain(0.4)
        if any("вимкнено" in s for s in fl):
            out("ПОМИЛКА: прошивка без SKIP_TEST_CMDS=1 (потрібен esp32dev_test)")
            sys.exit(2)
        send("NEXT")  # перший старт (штучний збій) → ERR → наступний трек
        lines = drain(4.0)
        err = any(f"ERR,{want}" in s and ",play" in s for s in lines)
        starts = sum(1 for s in lines if "play #" in s)
        good = err and starts >= 2
        ok += good
        out(f"серія {i:2}: ERR={'так' if err else 'НІ'} стартів треку={starts} -> {'ok' if good else 'ПЕРЕВІРИТИ'}")
    out(f"ok {ok}/{count}")

    for cmd in ("DBG,aout", "DBG,decode", "DBG,tasks", "DBG,heap"):
        send(cmd)
        for s in drain(1.5):
            print(s)  # уже записано в лог усередині drain
    sys.exit(0 if ok == count else 1)
finally:
    log.close()
    ser.close()
