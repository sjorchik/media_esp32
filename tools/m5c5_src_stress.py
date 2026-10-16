#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M5c5: стрес 100× перемикання SRC (частина A) і пачка рядків у UART0 (частина B) для медіамодуля WROOM.

Підключення (S3 ВІДКЛЮЧЕНИЙ від GPIO32/33):
  --ctl COMx : USB-UART адаптер на UART1 (адаптер TX -> GPIO32, адаптер RX <- GPIO33, GND), 921600 8N1.
               Канал команд і подій контракту (кадри $BODY*XX).
  --dbg COMy : штатний USB-порт плати (UART0, 460800). Консоль DBG,* для heap / aout / tasks / decode.

Стрес:   python tools/m5c5_src_stress.py --ctl COMx --dbg COMy --cycles 100 [--seed N] [--no-usb] [--no-bt]
Пачка:   python tools/m5c5_src_stress.py --dbg COMy --burst 50      (і --burst 200; --ctl не потрібен)

Кодування кадрів, розбір UART1 і XOR-сума беруться з tools/m5c4_frames.py (імпорт, без копії).
Код виходу: 0 — без FAIL; 1 — є FAIL (або в режимі --burst є втрати); 2 — не вдалось відкрити порт / немає HB.
FAIL — функціональні помилки (STATE, перезавантаження, HB, мʼют, невідомі ERR, underrun>0) та порушення правил M5c6b:
  heap  : пороги залежать від стану джерела (sd/off free>=20 КБ; bt з телефоном free>=15 КБ; для всіх largest>=12 КБ);
          витік = free монотонно падає на >=3 послідовних контрольних точках одного ключа і сумарно >=2048 Б;
  стеки : мінімальний вільний запас будь-якої задачі < 1024 Б.
INFO (не впливає на код виходу): одиничні просідання heap, просідання стеків відносно прогрітої бази (>128 Б).
Контрольна точка чекає --cp-delay (2,5 с) перед DBG,heap; --idle-tail N додає точку heap після N с спокою в off.
Довший тренд: --cycles 300 (контрольні точки кожні 10 циклів). Перевірка правил без заліза: --selftest.
Увага: відкриття UART0 на деяких платах скидає ESP32 через DTR/RTS; скрипт вимикає DTR/RTS до відкриття
й чекає старту, а перезавантаження до початку прогону не рахує.
"""
import argparse
import os
import random
import re
import sys
import threading
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import serial  # pyserial
except ImportError:
    serial = None
from m5c4_frames import Rx, make_frame  # noqa: E402  (кодування кадрів — перевикористання)

SRC_ALL = ["bt", "sd", "usb", "off"]
CMDS = ["PLAY", "PAUSE", "TOGGLE", "NEXT", "PREV", "FNEXT", "FPREV", "SHUF,0", "SHUF,1", "REP,0", "REP,1", "REP,2", "STOP"]
# Коди ERR із контракту (розд. 5). Для згенерованих валідних команд unknown_cmd/bad_arg/not_implemented бути не можуть → FAIL.
ERR_KNOWN = {"unknown_cmd", "bad_arg", "bad_state", "no_media", "read_fail", "decode_fail", "too_many_files",
             "not_implemented", "bt_fail"}
ERR_IMPOSSIBLE = {"unknown_cmd", "bad_arg", "not_implemented"}

# Формати відповідей DBG,* (зі зразків dbg_samples.txt і debug_cmds.cpp)
RE_HEAP = re.compile(r"\[heap\] \[dbg\] free=(\d+) largest=(\d+) min_free=(\d+)")
RE_STACK = re.compile(r"\[stack\]\s+(\S+)\s+вільно \(мін\.\)\s+(\d+)")
RE_AOUT1 = re.compile(r"aout: ready=(\d+) muted=(\d+) abort=(\d+)")
RE_AOUT2 = re.compile(r"aout: src=(\w+) state=(\w+)")
RE_WAITUN = re.compile(r"waitUnmuted довше \d+ мс: (\d+) разів")
RE_UNDER = re.compile(r"underrun=(\d+)")
RE_WTO = re.compile(r"write_timeout=(\d+)")
RE_CON1 = re.compile(r"console: RX-буфер cap=(\d+) peak=(\d+) high=(\d+)")
RE_CON2 = re.compile(r"console: рядки ok=(\d+) long=(\d+) longB=(\d+) maxlen=(\d+)/(\d+) байтів=(\d+)")


def open_port(name, baud):
    """Відкриває порт, не смикаючи DTR/RTS (інакше плата ESP32 може перезавантажитись)."""
    s = serial.Serial()
    s.port = name
    s.baudrate = baud
    s.timeout = 0.05
    s.write_timeout = 3
    s.dtr = False
    s.rts = False
    s.open()
    return s


# ---------------------------------------------------------------- UART0 (консоль DBG)
class Dbg(threading.Thread):
    """Читає рядки UART0, шукає банер перезавантаження, дає query() для DBG,*."""

    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.lock = threading.Lock()
        self.lines = []  # (t, текст)
        self.boots = 0
        self.stop_flag = False

    def run(self):
        buf = b""
        while not self.stop_flag:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception:  # noqa: BLE001
                break
            if not data:
                continue
            now = time.monotonic()
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                s = raw.decode("utf-8", "replace").strip("\r")
                with self.lock:
                    self.lines.append((now, s))
                    if "[I][main] медіамодуль WROOM" in s:
                        self.boots += 1

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return [s for _, s in self.lines[mark:]]

    def send(self, text):
        self.ser.write((text + "\n").encode("utf-8"))
        self.ser.flush()

    def query(self, cmd, pat, term=None, quiet=0.35, timeout=2.5):
        """Шле cmd; збирає рядки, що містять pat. Кінець: рядок із term, або тиша quiet після останнього рядка."""
        pat_re = re.compile(pat)
        term_re = re.compile(term) if term else None
        mark = self.mark()
        self.send(cmd)
        t0 = time.monotonic()
        last_new = None
        seen = 0
        while True:
            now = time.monotonic()
            ls = [s for s in self.since(mark) if pat_re.search(s)]
            if len(ls) != seen:
                seen = len(ls)
                last_new = now
            if term_re and any(term_re.search(s) for s in ls):
                return self.since(mark)
            if not term_re and last_new is not None and now - last_new >= quiet:
                return self.since(mark)
            if now - t0 >= timeout:
                return self.since(mark)
            time.sleep(0.02)


def parse_heap(lines):
    for s in lines:
        m = RE_HEAP.search(s)
        if m:
            return tuple(int(x) for x in m.groups())
    return None


def parse_tasks(lines):
    out = {}
    for s in lines:
        m = RE_STACK.search(s)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def parse_aout(lines):
    a = {}
    for s in lines:
        m = RE_AOUT1.search(s)
        if m:
            a["ready"], a["muted"], a["abort"] = (int(x) for x in m.groups())
        m = RE_AOUT2.search(s)
        if m:
            a["src"], a["state"] = m.group(1), m.group(2)
        m = RE_WAITUN.search(s)
        if m:
            a["waitlong"] = int(m.group(1))
    return a if "muted" in a and "state" in a else None


def parse_decode(lines):
    d = {}
    for s in lines:
        m = RE_UNDER.search(s)
        if m:
            d["underrun"] = int(m.group(1))
        m = RE_WTO.search(s)
        if m:
            d["wto"] = int(m.group(1))
    return d if "underrun" in d else None


# ---------------------------------------------------------------- оцінка heap/стеків (M5c6b, чисті функції)
def thresholds_from_args(a):
    """Збирає використані пороги в один словник (друкується в підсумку)."""
    return {
        "free_sd": a.free_min_sd,          # src=sd і off
        "free_bt_phone": a.free_min_bt,    # src=bt з підключеним телефоном
        "free_other": a.free_min_other,    # bt без телефону, usb та невідомий стан
        "largest": a.largest_min,          # усі стани
        "leak_steps": a.leak_steps,        # скільки послідовних просідань free = ознака витоку
        "leak_bytes": a.leak_bytes,        # мінімальне сумарне падіння для витоку
        "leak_step_min": a.leak_step_min,  # мінімальне падіння одного кроку, щоб він рахувався в серії (відсікає шум)
        "leak_recover": a.leak_recover,    # серія не витік, якщо пізніше free повернувся до старту серії з цим допуском
        "info": a.heap_drift,              # поріг INFO для одиничного просідання free між точками одного ключа
        "stack_min": a.stack_min,          # FAIL, якщо мін. вільно < цього
        "stack_tol": a.stack_tol,          # INFO, якщо просідання від бази > цього
    }


def heap_free_req(key, th):
    """Поріг free для ключа стану: sd/off — 20 КБ, bt+phone — 15 КБ, решта — free_other."""
    if key == "bt+phone":
        return th["free_bt_phone"]
    if key in ("sd", "off"):
        return th["free_sd"]
    return th["free_other"]


def eval_heap(samples, th):
    """samples: список dict(key, free, largest, minfree, base, tag) у порядку знімання.
    Повертає (рядки таблиці, FAIL-рядки, INFO-рядки)."""
    fails, infos, rows = [], [], []
    keys = []
    for smp in samples:
        if smp["key"] not in keys:
            keys.append(smp["key"])
    for k in keys:
        ss = [x for x in samples if x["key"] == k]
        base = next((x for x in ss if x["base"]), ss[0])
        after = ss[ss.index(base):]
        dfree = base["free"] - min(x["free"] for x in after)
        dlarg = base["largest"] - min(x["largest"] for x in after)
        rows.append((k, len(ss), base["free"], min(x["free"] for x in ss), dfree, base["largest"],
                     min(x["largest"] for x in ss), dlarg))
        # 1) пороги за станом джерела
        # точки прогріву (base) лише INFO: BT звільняє памʼять із затримкою, а база ще не усталена
        req = heap_free_req(k, th)
        for x in ss:
            if not x["base"]:
                continue
            if x["free"] < req or x["largest"] < th["largest"]:
                infos.append("прогрів heap[%s]: free %d / largest %d нижче порогів (%d / %d), точка %s — не оцінюється" % (
                    k, x["free"], x["largest"], req, th["largest"], x["tag"]))
        chk = [x for x in ss if not x["base"]]
        if chk:
            worst = min(chk, key=lambda x: x["free"])
            if worst["free"] < req:
                fails.append("heap[%s]: free %d Б < %d Б (точка %s)" % (k, worst["free"], req, worst["tag"]))
            worst_l = min(chk, key=lambda x: x["largest"])
            if worst_l["largest"] < th["largest"]:
                fails.append("heap[%s]: largest %d Б < %d Б (точка %s)" % (k, worst_l["largest"], th["largest"],
                                                                          worst_l["tag"]))
        # 2) тренд: максимальні серії строго спадного free між послідовними точками ключа
        in_leak = set()  # індекси кроків (i-1 -> i), які входять у FAIL-серію
        r = 0
        for i in range(1, len(ss) + 1):
            if i < len(ss) and ss[i - 1]["free"] - ss[i]["free"] >= th["leak_step_min"]:
                continue  # крок вниз не менше leak_step_min — серія триває (дрібний шум серію обриває)
            steps, total = i - 1 - r, ss[r]["free"] - ss[i - 1]["free"]
            if steps >= th["leak_steps"] and total >= th["leak_bytes"]:
                txt = "free монотонно падає %d кроків поспіль, -%d Б (%d→%d, точки %s…%s)" % (
                    steps, total, ss[r]["free"], ss[i - 1]["free"], ss[r]["tag"], ss[i - 1]["tag"])
                # якщо пізніше free повернувся до рівня старту серії — це не витік (тимчасово зайнята памʼять, напр. BT)
                if any(x["free"] >= ss[r]["free"] - th["leak_recover"] for x in ss[i:]):
                    infos.append("heap[%s]: %s, але далі відновився — не витік" % (k, txt))
                else:
                    fails.append("heap[%s]: витік? %s, без відновлення" % (k, txt))
                in_leak.update(range(r + 1, i))
            r = i
        # 3) одиничні просідання — лише INFO
        for i in range(1, len(ss)):
            drop = ss[i - 1]["free"] - ss[i]["free"]
            if drop >= th["info"] and i not in in_leak:
                infos.append("heap[%s]: одиничне просідання free на %d Б (%d→%d, %s→%s)" % (
                    k, drop, ss[i - 1]["free"], ss[i]["free"], ss[i - 1]["tag"], ss[i]["tag"]))
    return rows, fails, infos


def eval_stacks(samples, th):
    """samples: список (tag, base, {задача: мін. вільно}). Повертає (рядки, FAIL, INFO).
    FAIL — лише абсолютний мінімум; просідання від прогрітої бази — INFO."""
    fails, infos, rows = [], [], []
    base_stack, worst = {}, {}
    for tag, base, t in samples:  # база: мін. по зразках прогріву (watermark лише спадає)
        if base:
            for name, v in t.items():
                base_stack[name] = min(base_stack.get(name, v), v)
    for tag, base, t in samples:
        for name, v in t.items():
            base_stack.setdefault(name, v)  # задача зʼявилась пізніше (напр. mp3) — база = перший зразок
            worst[name] = min(worst.get(name, v), v)
    for name in sorted(base_stack):
        drop = base_stack[name] - worst[name]
        rows.append((name, base_stack[name], worst[name], drop))
        if worst[name] < th["stack_min"]:
            fails.append("стек %s: мін. вільно %d Б < %d Б" % (name, worst[name], th["stack_min"]))
        if drop > th["stack_tol"]:
            infos.append("стек %s: просів на %d Б (база %d → %d Б)" % (name, drop, base_stack[name], worst[name]))
    return rows, fails, infos


def run_selftest(args):
    """Сухий прогін оцінки на синтетичних контрольних точках (без портів і заліза)."""
    th = thresholds_from_args(args)

    def mk(key, frees, largest=14000):
        return [{"key": key, "free": f, "largest": largest, "minfree": f, "base": i == 0, "tag": "p%d" % i}
                for i, f in enumerate(frees)]

    cases = [
        ("BT з телефоном 17,8 КБ (стабільно) → PASS", "heap", mk("bt+phone", [17800, 17850, 17790, 17810]), False, False),
        ("монотонне падіння 4 точки по ≥600 Б → FAIL", "heap", mk("sd", [28000, 27400, 26800, 26200, 25600]), True, False),
        ("одиничне просідання 1,4 КБ → INFO", "heap", mk("sd", [28000, 26600, 28000, 27900]), False, True),
        ("піла з відновленням (3 падіння, далі назад) → INFO, не FAIL", "heap",
         mk("sd", [24976, 24852, 21744, 21456, 24852]), False, True),
        ("дрібний шум 8 Б на кроці не рахується серією → без FAIL", "heap", mk("sd", [24996, 24988, 24980, 24972, 24964]), False, False),
        ("прогрів нижче порога (BT ще звільняє) → лише INFO", "heap", mk("off", [14960, 24852, 24900]), False, True),
        ("sd free 19 КБ (< 20 КБ; перша точка — прогрів, INFO) → FAIL", "heap", mk("sd", [19000, 19000]), True, True),
        ("largest 11 КБ (< 12 КБ) → FAIL", "heap", mk("off", [30000, 30000], largest=11000), True, True),
        ("стек 1 КБ → 900 Б → FAIL", "stack", [("w", True, {"t": 1024}), ("c", False, {"t": 900})], True, False),
        ("стек 180 Б просідання при запасі ≈3 КБ → INFO", "stack",
         [("w", True, {"t": 3200}), ("c", False, {"t": 3020})], False, True),
    ]
    bad = 0
    for title, kind, data, want_fail, want_info in cases:
        _, f, i = eval_heap(data, th) if kind == "heap" else eval_stacks(data, th)
        ok = bool(f) == want_fail and bool(i) == want_info
        bad += not ok
        print("[%s] %s | FAIL=%d INFO=%d" % ("ok" if ok else "НЕВІРНО", title, len(f), len(i)))
        for x in f:
            print("      FAIL: " + x)
        for x in i:
            print("      INFO: " + x)
    print("САМОТЕСТ: %s" % ("УСПІХ" if not bad else "ПРОВАЛ (%d)" % bad))
    return 1 if bad else 0


# ---------------------------------------------------------------- режим --burst (частина B)
BURST_CMDS = {"heap": ("DBG,heap", r"\[heap\] \[dbg\] free="), "state": ("DBG,state", r"\[dbg\] src=")}


def read_console_stats(dbg):
    ls = dbg.query("DBG,console", r"console:", term=r"console: рядки", timeout=2.0)
    r = {}
    for s in ls:
        m = RE_CON1.search(s)
        if m:
            r["cap"], r["peak"], r["high"] = (int(x) for x in m.groups())
        m = RE_CON2.search(s)
        if m:
            r["ok"], r["long"], r["longB"], r["maxlen"], _, r["bytes"] = (int(x) for x in m.groups())
    return r


def run_burst(args):
    n = args.burst
    cmd, pat = BURST_CMDS[args.burst_cmd]
    pat_re = re.compile(pat)
    ser = open_port(args.dbg, args.dbg_baud)
    dbg = Dbg(ser)
    dbg.start()
    time.sleep(2.0)  # якщо відкриття порту все ж скинуло плату — дочекатись старту
    lost_total = 0
    print("Пачка: %d× '%s' одним записом у UART0 (%d Б, %d бод), %d повторів" % (
        n, cmd, n * (len(cmd) + 1), args.dbg_baud, args.burst_repeat))
    try:
        for trial in range(1, args.burst_repeat + 1):
            dbg.query("DBG,console,reset", r"console:", term=r"лічильники скинуто", timeout=1.5)
            time.sleep(0.3)
            mark = dbg.mark()
            payload = ((cmd + "\n") * n).encode("utf-8")
            t0 = time.monotonic()
            ser.write(payload)
            ser.flush()
            last_new, seen = t0, 0
            while True:
                now = time.monotonic()
                got = sum(1 for s in dbg.since(mark) if pat_re.search(s))
                if got != seen:
                    seen, last_new = got, now
                if got >= n or now - last_new >= 1.5 or now - t0 >= n * 0.03 + 6:
                    break
                time.sleep(0.02)
            ls = dbg.since(mark)
            recv = sum(1 for s in ls if pat_re.search(s))
            bad = sum(1 for s in ls if "невідома DBG-команда" in s or "рядок задовгий" in s)
            lost = n - recv
            lost_total += max(0, lost)
            st = read_console_stats(dbg)
            extra = ""
            if st:
                extra = " | firmware: RX cap=%s peak=%s high=%s, рядків ok=%s long=%s" % (
                    st.get("cap"), st.get("peak"), st.get("high"), st.get("ok"), st.get("long"))
            else:
                extra = " | DBG,console без відповіді (стара прошивка?)"
            print("повтор %d: надіслано=%d отримано=%d втрачено=%d (спотворених: %d), %.2f с%s" % (
                trial, n, recv, lost, bad, now - t0, extra))
            time.sleep(0.5)
    finally:
        dbg.stop_flag = True
        dbg.join(1.0)
        ser.close()
    print("ПІДСУМОК пачки %d: %s (втрачено разом %d)" % (n, "0 втрат" if not lost_total else "Є ВТРАТИ", lost_total))
    return 0 if lost_total == 0 else 1


# ---------------------------------------------------------------- стрес SRC (частина A)
class Stress:
    def __init__(self, ser, rx, dbg, args, pool, logf):
        self.ser, self.rx, self.dbg, self.a, self.pool, self.logf = ser, rx, dbg, args, pool, logf
        self.rng = random.Random(args.seed)
        self.t_last_cmd = 0.0
        self.cycle_res = []  # (n, ok, reasons)
        self.heap = []  # dict(key, free, largest, minfree, base, tag)
        self.stack_samples = []  # (tag, base, {імʼя: мін. вільно})
        self.under = []
        self.wto = []
        self.mute = {"checks": 0, "skip": 0, "fail": 0, "sticky": 0}
        self.waitun0 = None
        self.waitun_max = 0
        self.t0 = 0.0
        self.boots0 = 0
        self.global_fail = []

    # ---- утиліти
    def log(self, text):
        print(text)
        sys.stdout.flush()
        if self.logf:
            self.logf.write(text + "\n")
            self.logf.flush()

    def send(self, body):
        self.ser.write(make_frame(body))
        self.ser.flush()
        self.t_last_cmd = time.monotonic()

    def nonhb(self, mark):
        return [f for f in self.rx.since(mark) if f.name != "HB"]

    def last_frame(self, name):
        with self.rx.lock:
            for f in reversed(self.rx.frames):
                if f.name == name:
                    return f
        return None

    def cur_src(self):
        f = self.last_frame("STATE")
        return f.args[0] if f else None

    def media(self):
        f = self.last_frame("MEDIA")
        if not f or len(f.args) < 2:
            return {"sd": False, "usb": False}
        return {"sd": f.args[0] == "1", "usb": f.args[1] == "1"}

    def bt_conn(self):
        f = self.last_frame("BT")
        return bool(f and f.args and f.args[0] == "connected")

    def get_state(self):
        mark = self.rx.count()
        self.send("GET")
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            for f in self.nonhb(mark):
                if f.name == "STATE":
                    return f.args[0], f.args[1]
            time.sleep(0.02)
        return None, None

    # ---- один SRC (з гонкою або без)
    def extras(self):
        sent = []
        for _ in range(self.rng.randint(1, 3)):
            b = self.rng.choice(CMDS)
            self.send(b)
            sent.append(b)
            time.sleep(self.rng.uniform(0.01, 0.2))
        return sent

    def switch(self, x, race=None, extras_when=None):
        reasons = []
        before = self.cur_src()
        has = self.media()
        mark = self.rx.count()
        self.send("SRC," + x)
        final = x
        if race:
            time.sleep(self.rng.uniform(0, 0.012))
            self.send("SRC," + race)
            final = race
        sent_extras = []
        if extras_when == "scan":
            sent_extras = self.extras()
        strict = final != before  # інше джерело → STATE обовʼязковий; те саме → мовчання допустиме
        nomedia = final in ("sd", "usb") and not has[final]
        any_nomedia = any(s in ("sd", "usb") and not has[s] for s in (x, race) if s)

        def look():
            st, err = None, False
            for f in self.nonhb(mark):
                if f.name == "STATE" and f.args[0] == final:
                    good = f.args[1] == "idle" if (final == "off" or nomedia) else f.args[1] != "scan"
                    if good:
                        st = f
                elif f.name == "ERR" and f.args and f.args[0] == "no_media":
                    err = True
            return st, err

        if strict:
            end = time.monotonic() + self.a.settle
            while True:
                st, err = look()
                if st and (err or not nomedia):
                    break
                if time.monotonic() >= end:
                    break
                time.sleep(0.02)
            if not st:
                reasons.append("немає STATE,%s,%s за %.0f с" % (final, "idle" if (final == "off" or nomedia) else "≠scan",
                                                                 self.a.settle))
        else:
            time.sleep(0.6)
            st, err = look()
        if nomedia and not look()[1]:
            reasons.append("немає ERR,no_media (носія немає)")
        if not any_nomedia and look()[1]:
            reasons.append("ERR,no_media при наявному носії")

        # Перехресна перевірка станом: GET (ловить мовчазні зависання й гонки)
        want_idle = final == "off" or nomedia
        end = time.monotonic() + self.a.settle
        s_src = s_state = None
        while True:
            s_src, s_state = self.get_state()
            if s_src == final and (s_state == "idle" if want_idle else s_state not in ("scan", None)):
                break
            if time.monotonic() >= end:
                reasons.append("GET: src=%s state=%s (очікувалось %s,%s)" % (s_src, s_state, final,
                                                                              "idle" if want_idle else "≠scan"))
                break
            time.sleep(0.4)
        if extras_when == "settled":
            sent_extras = self.extras()
        time.sleep(self.rng.uniform(0, self.a.dwell))
        return final, reasons, sent_extras

    # ---- DBG-запити
    def dbg_heap(self):
        return parse_heap(self.dbg.query("DBG,heap", r"\[heap\] \[dbg\]", term=r"\[heap\] \[dbg\]", timeout=2.0))

    def dbg_tasks(self):
        return parse_tasks(self.dbg.query("DBG,tasks", r"\[stack\]", quiet=0.4, timeout=3.0))

    def dbg_aout(self):
        return parse_aout(self.dbg.query("DBG,aout", r"\[dbg\] (aout|mp3):|\[heap\] \[aout\]", term=r"\[heap\] \[aout\]",
                                         timeout=2.5))

    def dbg_decode(self):
        return parse_decode(self.dbg.query("DBG,decode", r"decode:|\[heap\] \[decode\]", term=r"\[heap\] \[decode\]",
                                           timeout=1.5))

    # ---- перевірка мʼюту (4) і «залиплого» мʼюту
    def mute_check(self, final):
        reasons = []
        st = self.last_frame("STATE")
        if not st or st.args[0] != final:
            self.mute["skip"] += 1
            return reasons
        ev_state = st.args[1]
        need = 1.6 if (final == "sd" and ev_state == "play") else 0.3
        delay = max(st.t, self.t_last_cmd) + need - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        a = self.dbg_aout()
        if a is None:
            reasons.append("DBG,aout без відповіді (UART0)")
            self.mute["fail"] += 1
            return reasons
        if not a.get("ready", 1):
            reasons.append("aout: ready=0")
        if self.waitun0 is None:
            self.waitun0 = a.get("waitlong", 0)
        if a.get("waitlong", 0) > self.waitun0:
            reasons.append("mp3: waitUnmuted довше порогу %d→%d разів («залиплий» мʼют)" % (self.waitun0, a["waitlong"]))
            self.waitun0 = a["waitlong"]
        self.waitun_max = max(self.waitun_max, a.get("waitlong", 0))
        if self.last_frame("STATE") is not st or a["src"] != final or a["state"] != ev_state:
            self.mute["skip"] += 1  # стан встиг змінитись між подією і запитом — не оцінюємо
            return reasons
        self.mute["checks"] += 1
        if ev_state != "play" and a["muted"] != 1:
            self.mute["fail"] += 1
            reasons.append("state=%s, але aout muted=%d через ≥300 мс (src=%s)" % (ev_state, a["muted"], final))
        if final == "sd" and ev_state == "play" and time.monotonic() - st.t >= 1.5:
            self.mute["sticky"] += 1
            if a["muted"] != 0:
                self.mute["fail"] += 1
                reasons.append("sd play ≥1,5 с, але muted=1 («залиплий» мʼют)")
        return reasons

    # ---- контрольна точка heap/stack/decode
    def checkpoint(self, tag, baseline=False):
        fails = []
        src = self.cur_src() or "?"
        key = src + ("+phone" if src == "bt" and self.bt_conn() else "")
        if self.a.cp_delay > 0:
            time.sleep(self.a.cp_delay)  # дати heap «осісти» після перемикання (напр. BT звільняє памʼять із затримкою)
        h = self.dbg_heap()
        if h is None:
            fails.append("DBG,heap без відповіді")
        else:
            self.heap.append({"key": key, "free": h[0], "largest": h[1], "minfree": h[2], "base": baseline, "tag": tag})
        t = self.dbg_tasks()
        if not t:
            fails.append("DBG,tasks без відповіді")
        else:
            self.stack_samples.append((tag, baseline, t))
        d = None
        if src == "sd":
            d = self.dbg_decode()
            if d:
                self.under.append(d["underrun"])
                self.wto.append(d.get("wto", 0))
        self.log("   [checkpoint %s src=%s] heap free=%s largest=%s min_free=%s | стеки: %s | underrun=%s" % (
            tag, key, *(h if h else ("?", "?", "?")), ("%d задач" % len(t)) if t else "?",
            d["underrun"] if d else "н/д"))
        return fails

    # ---- головний прогін
    def warmup(self):
        order = [s for s in ("sd", "usb", "bt", "off") if s in self.pool]
        if "off" not in order:
            order.append("off")
        for p in range(1, self.a.warmup + 1):
            last = p == self.a.warmup
            for s in order:
                final, reasons, _ = self.switch(s)
                if s == "sd":
                    time.sleep(3.0)  # дати декодеру відіграти (стек mp3, underrun)
                if reasons:
                    self.global_fail.append("прогрів %s: %s" % (s, "; ".join(reasons)))
                if last:
                    for f in self.checkpoint("warmup-%s" % s, baseline=True):
                        self.global_fail.append("прогрів: " + f)
            self.log("прогрів %d/%d завершено" % (p, self.a.warmup))
        a = self.dbg_aout()
        if a:
            self.waitun0 = a.get("waitlong", 0)

    def run(self):
        N = self.a.cycles
        for i in range(1, N + 1):
            t_c = time.monotonic()
            race = self.rng.random() < 0.10
            x = self.rng.choice(self.pool)
            b = self.rng.choice(self.pool) if race else None
            ew = self.rng.choice(["scan", "settled"]) if self.rng.random() < 0.30 else None
            final, reasons, ex = self.switch(x, race=b, extras_when=ew)
            reasons += self.mute_check(final)
            ok = not reasons
            self.cycle_res.append((i, ok, reasons))
            self.log("[%3d/%d] SRC,%s%s%s -> %s (%.1f с)%s" % (
                i, N, x, ("+SRC," + b + " (гонка)") if race else "", (" +%s@%s" % (",".join(ex), ew)) if ex else "",
                "ok" if ok else "FAIL", time.monotonic() - t_c, "" if ok else ": " + "; ".join(reasons)))
            if i % 10 == 0 and i != N:
                for f in self.checkpoint("cycle-%d" % i):
                    self.global_fail.append("цикл %d: %s" % (i, f))
        # Завершення: sd-play із зразком decode, потім off і підсумкова точка
        if "sd" in self.pool and self.media()["sd"] and self.a.final_sd:
            final, reasons, _ = self.switch("sd")
            time.sleep(3.0)
            if reasons:
                self.global_fail.append("фінал sd: " + "; ".join(reasons))
            for f in self.checkpoint("final-sd"):
                self.global_fail.append("фінал sd: " + f)
        final, reasons, _ = self.switch("off")
        if reasons:
            self.global_fail.append("фінал off: " + "; ".join(reasons))
        for f in self.checkpoint("final-off"):
            self.global_fail.append("фінал: " + f)
        if self.a.idle_tail > 0:  # діагностика: чи повертається heap до бази після спокою в off
            self.log("   [idle-tail] спокій у off %.0f с..." % self.a.idle_tail)
            time.sleep(self.a.idle_tail)
            for f in self.checkpoint("tail-off"):
                self.global_fail.append("хвіст off: " + f)

    # ---- підсумок
    def summary(self):
        fails = list(self.global_fail)
        ok_n = sum(1 for _, ok, _ in self.cycle_res if ok)
        bad_n = len(self.cycle_res) - ok_n
        t_end = time.monotonic()
        with self.rx.lock:
            frames = [f for f in self.rx.frames if f.t >= self.t0]
        ready = sum(1 for f in frames if f.name == "READY")
        hb = [f for f in frames if f.name == "HB"]
        max_gap = max([b.t - a.t for a, b in zip(hb, hb[1:])] or [0.0])
        seq_gaps = [(a.args[0], b.args[0]) for a, b in zip(hb, hb[1:])
                    if (int(a.args[0]) + 1) % 65536 != int(b.args[0])]
        boots = self.dbg.boots - self.boots0
        errs = Counter(f.args[0] if f.args else "?" for f in frames if f.name == "ERR")

        if ready:
            fails.append("READY посеред прогону: %d (перезавантаження)" % ready)
        if boots:
            fails.append("банер завантаження в UART0: %d (перезавантаження)" % boots)
        if max_gap > 3.0:
            fails.append("розрив HB %.1f с > 3 с" % max_gap)
        if seq_gaps:
            fails.append("пропуски seq HB: %s" % seq_gaps[:5])
        if self.rx.bad:
            fails.append("биті/завеликі кадри від модуля: %d" % self.rx.bad)
        for c in errs:
            if c not in ERR_KNOWN:
                fails.append("невідомий код ERR: %s (×%d)" % (c, errs[c]))
            elif c in ERR_IMPOSSIBLE:
                fails.append("ERR,%s ×%d на валідні команди" % (c, errs[c]))
        if bad_n:
            fails.append("цикли з FAIL: %d" % bad_n)
        if self.mute["fail"]:
            fails.append("мʼют: %d невідповідностей" % self.mute["fail"])

        # heap: пороги за станом джерела + тренд витоку (одиничні просідання — INFO)
        th = thresholds_from_args(self.a)
        rows, hf, hi = eval_heap(self.heap, th)
        fails += hf
        infos = list(hi)
        min_free_dev = min([s["minfree"] for s in self.heap] or [0])
        min_free_obs = min([s["free"] for s in self.heap] or [0])
        min_larg_obs = min([s["largest"] for s in self.heap] or [0])
        if not self.heap:
            fails.append("жодного зразка DBG,heap")

        # стеки: FAIL лише абсолютний мінімум; просідання від бази — INFO
        stack_rows, sf, si = eval_stacks(self.stack_samples, th)
        fails += sf
        infos += si
        if self.under and max(self.under) > 0:
            fails.append("underrun=%d" % max(self.under))

        L = self.log
        L("")
        L("================ M5c5: підсумок стресу SRC ================")
        L("Цикли: ok %d / fail %d з %d | прогін %.0f с | seed=%s | пул=%s" % (ok_n, bad_n, len(self.cycle_res),
                                                                               t_end - self.t0, self.a.seed, ",".join(self.pool)))
        L("Перезавантаження: READY=%d, банер UART0=%d | HB: %d кадрів, макс. розрив %.2f с, пропусків seq %d" % (
            ready, boots, len(hb), max_gap, len(seq_gaps)))
        L("Heap по джерелах (Б):  ключ        n   free_base free_min dFree | largest_base largest_min dLargest")
        for r in rows:
            L("   %-12s %3d %10d %8d %6d | %12d %11d %8d" % r)
        L("   мін. спостережений free=%d, largest=%d; min_free пристрою=%d (за весь час роботи, лише інформативно)" % (
            min_free_obs, min_larg_obs, min_free_dev))
        L("underrun (decode, зразків %d): макс. %s; write_timeout макс. %s" % (
            len(self.under), max(self.under) if self.under else "н/д", max(self.wto) if self.wto else "н/д"))
        L("Стеки (мін. вільно, Б):  задача      база  найгірше  просідання")
        for r in stack_rows:
            L("   %-10s %8d %9d %10d" % r)
        L("Пороги heap: free >= %d (sd, off), >= %d (bt з телефоном), >= %d (bt без телефону, usb); largest >= %d (усі стани)" % (
            th["free_sd"], th["free_bt_phone"], th["free_other"], th["largest"]))
        L("   витік (FAIL): free монотонно падає >= %d кроків поспіль і сумарно >= %d Б; INFO: одиничне просідання >= %d Б" % (
            th["leak_steps"], th["leak_bytes"], th["info"]))
        L("Пороги стеків: FAIL якщо мін. вільно < %d Б; INFO якщо просідання від бази > %d Б" % (
            th["stack_min"], th["stack_tol"]))
        L("Мʼют: перевірок %d (з них «sd play ≥1,5 с» %d), пропущено %d, невідповідностей %d; waitUnmuted макс. %d" % (
            self.mute["checks"], self.mute["sticky"], self.mute["skip"], self.mute["fail"], self.waitun_max))
        L("ERR-гістограма: %s" % (", ".join("%s=%d" % kv for kv in sorted(errs.items())) or "порожня"))
        if fails:
            L("")
            L("FAIL:")
            for f in fails:
                L("  - " + f)
        if infos:
            L("")
            L("INFO (не впливає на результат):")
            for f in infos:
                L("  - " + f)
        L("")
        L("ПІДСУМОК: %s" % ("УСПІХ" if not fails else "ПРОВАЛ (%d)" % len(fails)))
        return 1 if fails else 0


def run_stress(args):
    if not args.ctl:
        print("Потрібен --ctl COMx (UART1) для стресу SRC")
        return 2
    pool = [s for s in SRC_ALL if not (s == "usb" and args.no_usb) and not (s == "bt" and args.no_bt)]
    try:
        ser = open_port(args.ctl, args.ctl_baud)
        dser = open_port(args.dbg, args.dbg_baud)
    except Exception as e:  # noqa: BLE001
        print("Не вдалось відкрити порт: %s" % e)
        return 2
    rx = Rx(ser)
    rx.start()
    dbg = Dbg(dser)
    dbg.start()
    logf = open(args.log, "w", encoding="utf-8") if args.log else None
    st = Stress(ser, rx, dbg, args, pool, logf)
    try:
        time.sleep(2.0)  # якщо відкриття UART0 скинуло плату — дочекатись старту
        end = time.monotonic() + 8.0
        while time.monotonic() < end and not any(f.name == "HB" for f in rx.since(0)):
            time.sleep(0.1)
        if not any(f.name == "HB" for f in rx.since(0)):
            print("Немає HB на UART1 за 10 с: перевір проводку (адаптер TX→GPIO32, RX←GPIO33, GND), 921600, S3 відключено")
            return 2
        st.send("SRC,off")
        time.sleep(1.0)
        s_src, s_state = st.get_state()
        if s_src is None:
            print("Модуль не відповідає на GET")
            return 2
        st.send("SRC,off")  # гарантовано стартуємо з off
        time.sleep(1.5)
        st.get_state()
        st.t0 = time.monotonic()
        st.boots0 = dbg.boots
        st.log("M5c5 стрес: цикли=%d seed=%s пул=%s settle=%.0f с; media=%s" % (args.cycles, args.seed, ",".join(pool),
                                                                                args.settle, st.media()))
        st.warmup()
        st.run()
        return st.summary()
    finally:
        try:
            st.send("SRC,off")
            time.sleep(0.3)
        except Exception:  # noqa: BLE001
            pass
        rx.stop_flag = True
        dbg.stop_flag = True
        rx.join(1.0)
        dbg.join(1.0)
        ser.close()
        dser.close()
        if logf:
            logf.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description="M5c5/M5c6b: стрес SRC (UART1+UART0) і пачка в UART0 (--burst); пороги heap — за станом джерела")
    ap.add_argument("--ctl", help="COM-порт USB-UART адаптера на UART1 модуля (921600)")
    ap.add_argument("--dbg", help="COM-порт штатного USB плати (UART0, 460800)")
    ap.add_argument("--cycles", type=int, default=100,
                    help="кількість циклів SRC (за замовч. 100; для довшого тренду — 300, контрольні точки кожні 10 циклів)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--settle", type=float, default=15.0, help="таймаут очікування STATE≠scan після SRC, с")
    ap.add_argument("--no-usb", action="store_true", help="не вибирати SRC,usb")
    ap.add_argument("--no-bt", action="store_true", help="не вибирати SRC,bt")
    ap.add_argument("--warmup", type=int, default=2, help="проходів прогріву по всіх джерелах до базових точок")
    ap.add_argument("--dwell", type=float, default=1.0, help="макс. випадкова пауза після SRC, с")
    ap.add_argument("--free-min-sd", type=int, default=20480,
                    help="FAIL-поріг heap free для src=sd і off, Б (за замовч. 20480 = 20 КБ)")
    ap.add_argument("--free-min-bt", type=int, default=15360,
                    help="FAIL-поріг heap free для src=bt З телефоном, Б (за замовч. 15360 = 15 КБ; норма ≈17,8 КБ)")
    ap.add_argument("--free-min-other", type=int, default=15360,
                    help="FAIL-поріг heap free для bt без телефону, usb і невідомого стану, Б (за замовч. 15360)")
    ap.add_argument("--largest-min", type=int, default=12288,
                    help="FAIL-поріг heap largest для всіх станів, Б (за замовч. 12288 = 12 КБ)")
    ap.add_argument("--leak-steps", type=int, default=3,
                    help="витік: скільки послідовних просідань free одного ключа (за замовч. 3)")
    ap.add_argument("--leak-bytes", type=int, default=2048,
                    help="витік: мінімальне сумарне падіння free за серію, Б (за замовч. 2048); FAIL лише при обох умовах")
    ap.add_argument("--leak-step-min", type=int, default=64,
                    help="витік: мінімальне падіння одного кроку, щоб рахуватись у серії, Б (за замовч. 64; відсікає шум)")
    ap.add_argument("--leak-recover", type=int, default=1024,
                    help="витік: серія не FAIL, якщо пізніше free повернувся до старту серії з допуском, Б (за замовч. 1024)")
    ap.add_argument("--heap-drift", type=int, default=1024,
                    help="INFO (не FAIL): друкувати одиничне просідання free між точками одного ключа від цього значення, Б")
    ap.add_argument("--stack-tol", type=int, default=128,
                    help="INFO (не FAIL): просідання запасу стека від прогрітої бази більше за це, Б")
    ap.add_argument("--stack-min", type=int, default=1024,
                    help="FAIL-поріг: мінімальний вільний запас будь-якої задачі, Б (за замовч. 1024)")
    ap.add_argument("--cp-delay", dest="cp_delay", type=float, default=2.5,
                    help="пауза перед DBG,heap у кожній контрольній точці, с (за замовч. 2.5; 0 — без паузи)")
    ap.add_argument("--idle-tail", dest="idle_tail", type=float, default=0.0,
                    help="діагностика: після прогону лишити SRC,off на N с і зняти ще одну точку heap (0 — вимкнено)")
    ap.add_argument("--selftest", action="store_true",
                    help="сухий прогін оцінки heap/стеків на синтетичних точках (без портів)")
    ap.add_argument("--no-final-sd", dest="final_sd", action="store_false", help="не робити фінальний sd-play із DBG,decode")
    ap.add_argument("--log", help="дублювати вивід у файл")
    ap.add_argument("--burst", type=int, metavar="N", help="режим частини B: N рядків DBG одним записом у UART0")
    ap.add_argument("--burst-cmd", choices=sorted(BURST_CMDS), default="heap")
    ap.add_argument("--burst-repeat", type=int, default=3)
    ap.add_argument("--ctl-baud", type=int, default=921600)
    ap.add_argument("--dbg-baud", type=int, default=460800)
    args = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:  # noqa: BLE001
        pass
    if args.selftest:
        return run_selftest(args)
    if serial is None:
        print("Потрібен pyserial:  pip install pyserial")
        return 2
    if not args.dbg:
        ap.error("потрібен --dbg COMy (UART0)")
    if args.burst:
        return run_burst(args)
    return run_stress(args)


if __name__ == "__main__":
    sys.exit(main())
