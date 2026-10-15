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

        # heap: дрейф по ключах (джерело[+phone]) від базової точки (прогрів), мінімуми
        keys = []
        for s in self.heap:
            if s["key"] not in keys:
                keys.append(s["key"])
        rows = []
        for k in keys:
            ss = [s for s in self.heap if s["key"] == k]
            base = next((s for s in ss if s["base"]), ss[0])
            after = ss[ss.index(base):]
            dfree = base["free"] - min(s["free"] for s in after)
            dlarg = base["largest"] - min(s["largest"] for s in after)
            rows.append((k, len(ss), base["free"], min(s["free"] for s in ss), dfree, base["largest"],
                         min(s["largest"] for s in ss), dlarg))
            if dfree > self.a.heap_drift:
                fails.append("heap[%s]: free просів на %d Б (> %d)" % (k, dfree, self.a.heap_drift))
            if dlarg > self.a.heap_drift:
                fails.append("heap[%s]: largest просів на %d Б (> %d)" % (k, dlarg, self.a.heap_drift))
        min_free_dev = min([s["minfree"] for s in self.heap] or [0])
        min_free_obs = min([s["free"] for s in self.heap] or [0])
        min_larg_obs = min([s["largest"] for s in self.heap] or [0])
        if self.heap:
            if min(min_free_dev, min_free_obs) < 20480:
                fails.append("free < 20 КБ (min_free пристрою %d, спостережений %d)" % (min_free_dev, min_free_obs))
            if min_larg_obs < 12288:
                fails.append("largest < 12 КБ (%d)" % min_larg_obs)
        else:
            fails.append("жодного зразка DBG,heap")

        # стеки: просідання відносно базової точки, абсолютний мінімум
        base_stack, worst = {}, {}
        for tag, base, t in self.stack_samples:  # база: мін. по зразках прогріву (watermark лише спадає)
            if base:
                for name, v in t.items():
                    base_stack[name] = min(base_stack.get(name, v), v)
        for tag, base, t in self.stack_samples:
            for name, v in t.items():
                base_stack.setdefault(name, v)  # задача зʼявилась пізніше (напр. mp3) — база = перший зразок
                worst[name] = min(worst.get(name, v), v)
        stack_rows = []
        for name in sorted(base_stack):
            drop = base_stack[name] - worst[name]
            stack_rows.append((name, base_stack[name], worst[name], drop))
            if drop > self.a.stack_tol:
                fails.append("стек %s просів на %d Б (> %d)" % (name, drop, self.a.stack_tol))
            if worst[name] < self.a.stack_min:
                fails.append("стек %s: мін. вільно %d Б < %d" % (name, worst[name], self.a.stack_min))
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
        L("   min_free пристрою=%d, мін. спостережений free=%d, largest=%d (пороги 20480 / 12288)" % (
            min_free_dev, min_free_obs, min_larg_obs))
        L("underrun (decode, зразків %d): макс. %s; write_timeout макс. %s" % (
            len(self.under), max(self.under) if self.under else "н/д", max(self.wto) if self.wto else "н/д"))
        L("Стеки (мін. вільно, Б):  задача      база  найгірше  просідання")
        for r in stack_rows:
            L("   %-10s %8d %9d %10d" % r)
        L("Мʼют: перевірок %d (з них «sd play ≥1,5 с» %d), пропущено %d, невідповідностей %d; waitUnmuted макс. %d" % (
            self.mute["checks"], self.mute["sticky"], self.mute["skip"], self.mute["fail"], self.waitun_max))
        L("ERR-гістограма: %s" % (", ".join("%s=%d" % kv for kv in sorted(errs.items())) or "порожня"))
        if fails:
            L("")
            L("FAIL:")
            for f in fails:
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
    ap = argparse.ArgumentParser(description="M5c5: стрес SRC (UART1+UART0) і пачка в UART0 (--burst)")
    ap.add_argument("--ctl", help="COM-порт USB-UART адаптера на UART1 модуля (921600)")
    ap.add_argument("--dbg", help="COM-порт штатного USB плати (UART0, 460800)")
    ap.add_argument("--cycles", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--settle", type=float, default=15.0, help="таймаут очікування STATE≠scan після SRC, с")
    ap.add_argument("--no-usb", action="store_true", help="не вибирати SRC,usb")
    ap.add_argument("--no-bt", action="store_true", help="не вибирати SRC,bt")
    ap.add_argument("--warmup", type=int, default=2, help="проходів прогріву по всіх джерелах до базових точок")
    ap.add_argument("--dwell", type=float, default=1.0, help="макс. випадкова пауза після SRC, с")
    ap.add_argument("--heap-drift", type=int, default=1024, help="допустимий дрейф free/largest вниз, Б")
    ap.add_argument("--stack-tol", type=int, default=128, help="допустиме просідання запасу стека, Б")
    ap.add_argument("--stack-min", type=int, default=512, help="абсолютний мінімум запасу стека, Б")
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
