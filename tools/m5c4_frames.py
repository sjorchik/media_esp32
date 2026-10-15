#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""M5c4: перевірка протоколу UART1 медіамодуля WROOM (контракт proto_ver=1) на залізі.

Підключення: USB-UART адаптер (3,3 В) до UART1 модуля:
  адаптер TX -> GPIO32 (RX модуля), адаптер RX <- GPIO33 (TX модуля), спільна земля.
S3 має бути ВІДКЛЮЧЕНИЙ від GPIO32/33 (див. hwcheck_M5c4.md). Швидкість 921600 8N1.

Запуск:  python tools/m5c4_frames.py COMx
Скрипт шле кадри з правильною XOR-сумою й друкує PASS / FAIL / SKIP / INFO по кожному сценарію.
Код виходу 0 — немає FAIL; 1 — є FAIL; 2 — не вдалось відкрити порт / не зайшла передумова.
Звичайні сценарії (крім SKIP) залишають модуль у SRC,off із початковими shuf/rep.
"""
import argparse
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:  # пояснення замість traceback
    serial = None

MAX_FRAME = 255


# ---------------------------------------------------------------- кадри
def xor_sum(b):
    x = 0
    for c in b:
        x ^= c
    return x


def make_frame(body):
    if isinstance(body, str):
        body = body.encode("utf-8")
    return b"$" + body + b"*%02X\n" % xor_sum(body)


def pct_decode(b):
    out = bytearray()
    i = 0
    while i < len(b):
        if b[i] == 0x25 and i + 2 < len(b):
            try:
                out.append(int(b[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(b[i])
        i += 1
    return out.decode("utf-8", errors="replace")


class Frame:
    def __init__(self, t, body, raw):
        self.t = t
        self.raw = raw
        parts = body.split(b",")
        self.name = parts[0].decode("ascii", errors="replace")
        self.args = [pct_decode(p) for p in parts[1:]]

    def text(self):
        return ",".join([self.name] + self.args)

    def __repr__(self):
        return self.text()


class Rx(threading.Thread):
    """Читає UART, розбирає кадри модуля за контрактом, рахує биті/завеликі."""

    def __init__(self, ser):
        super().__init__(daemon=True)
        self.ser = ser
        self.lock = threading.Lock()
        self.frames = []
        self.bad = 0
        self.max_len = 0
        self.stop_flag = False
        self.buf = None
        self.n = 0

    def run(self):
        while not self.stop_flag:
            try:
                data = self.ser.read(256)
            except Exception:
                break
            now = time.monotonic()
            for b in data:
                self._feed(b, now)

    def _feed(self, b, now):
        if b == 0x24:  # '$' — початок / ресинхронізація
            self.buf = bytearray()
            self.n = 1
            return
        if self.buf is None:
            return
        self.n += 1
        if b == 0x0D:
            return
        if b == 0x0A:
            self._finish(now)
            self.buf = None
            return
        self.buf.append(b)

    def _finish(self, now):
        total = self.n  # '$' + тіло + CR (якщо були) + LF; LF уже пораховано в _feed
        buf = bytes(self.buf)
        self.max_len = max(self.max_len, total)
        ok = total <= MAX_FRAME and len(buf) >= 4 and buf[-3] == 0x2A
        if ok:
            try:
                got = int(buf[-2:].decode("ascii"), 16)
                ok = buf[-2:] == buf[-2:].upper() and xor_sum(buf[:-3]) == got
            except ValueError:
                ok = False
        if not ok:
            self.bad += 1
            return
        fr = Frame(now, buf[:-3], buf)
        with self.lock:
            self.frames.append(fr)

    def count(self):
        with self.lock:
            return len(self.frames)

    def since(self, mark):
        with self.lock:
            return list(self.frames[mark:])


# ---------------------------------------------------------------- контекст і хелпери
class Ctx:
    def __init__(self, ser, rx, args):
        self.ser = ser
        self.rx = rx
        self.args = args
        self.base = {}  # початкові shuf/rep, media, bt


def send(ctx, body):
    ctx.ser.write(make_frame(body))
    ctx.ser.flush()


def send_raw(ctx, data):
    ctx.ser.write(data)
    ctx.ser.flush()


def non_hb(frames):
    return [f for f in frames if f.name != "HB"]


def collect(ctx, mark, first_wait=1.2, quiet=0.3, max_wait=4.0):
    """Чекає перші не-HB кадри (до first_wait), далі — тишу quiet. Повертає не-HB кадри після mark."""
    t0 = time.monotonic()
    last = None
    while True:
        now = time.monotonic()
        fr = non_hb(ctx.rx.since(mark))
        if fr:
            n = len(fr)
            if last is None or n != last[0]:
                last = (n, now)
            elif now - last[1] >= quiet:
                return fr
        elif now - t0 >= first_wait:
            return []
        if now - t0 >= max_wait:
            return fr
        time.sleep(0.02)


def silence(ctx, mark, dur=0.7):
    time.sleep(dur)
    return non_hb(ctx.rx.since(mark))


def fmt(frames):
    return "[" + " | ".join(f.text() for f in frames) + "]" if frames else "[нічого]"


def match(frame, name, args):
    if frame.name != name:
        return False
    for i, a in enumerate(args):
        if a is None:
            continue
        if i >= len(frame.args) or frame.args[i] != a:
            return False
    return len(frame.args) >= len(args)


def expect(frames, specs):
    """specs: [(NAME, [арг або None, ...]), ...] — рівно такі кадри в такому порядку."""
    if len(frames) != len(specs):
        return False, "очікувалось %d кадр(ів), отримано %d: %s" % (len(specs), len(frames), fmt(frames))
    for f, (n, a) in zip(frames, specs):
        if not match(f, n, a):
            return False, "очікувалось %s%s, отримано %s" % (n, ("," + ",".join("*" if x is None else x for x in a)) if a else "", f.text())
    return True, fmt(frames)


def cmd(ctx, body, **kw):
    mark = ctx.rx.count()
    send(ctx, body)
    return collect(ctx, mark, **kw)


def settle(ctx, t=0.6):
    time.sleep(t)


# ---------------------------------------------------------------- сценарії (повертають (статус, деталь))
PASS, FAIL, SKIP, INFO = "PASS", "FAIL", "SKIP", "INFO"


def sc_hb(ctx):
    """HB від модуля: інтервал 1000 мс, seq по колу."""
    mark = ctx.rx.count()
    time.sleep(6.6)
    hb = [f for f in ctx.rx.since(mark) if f.name == "HB"]
    if len(hb) < 5:
        return FAIL, "отримано лише %d HB за 6,6 с (немає звʼязку? перевір проводку й швидкість)" % len(hb)
    iv = [b.t - a.t for a, b in zip(hb, hb[1:])]
    mean = sum(iv) / len(iv)
    bad_iv = [round(x, 3) for x in iv if not 0.7 <= x <= 1.3]
    seqs = [int(f.args[0]) for f in hb]
    bad_seq = [(a, b) for a, b in zip(seqs, seqs[1:]) if (a + 1) % 65536 != b]
    if bad_iv or not 0.95 <= mean <= 1.05:
        return FAIL, "інтервали: середній %.3f с, поза 0,7–1,3 с: %s" % (mean, bad_iv)
    if bad_seq:
        return FAIL, "seq не послідовний: %s" % bad_seq
    return PASS, "%d HB, середній інтервал %.3f с, seq %d..%d" % (len(hb), mean, seqs[0], seqs[-1])


def sc_precond(ctx):
    """Передумова: SRC,off, потім GET; запамʼятовує shuf/rep і media."""
    send(ctx, "SRC,off")
    settle(ctx, 1.5)
    fr = cmd(ctx, "GET")
    ok, d = expect(fr, [("STATE", ["off", "idle", None, None]), ("MEDIA", [None, None]), ("BT", [None, None])])
    if not ok:
        return FAIL, "після SRC,off GET: " + d
    st = fr[0].args
    ctx.base = {"shuf": st[2], "rep": st[3], "sd": fr[1].args[0], "usb": fr[1].args[1], "bt": fr[2].args[0]}
    return PASS, "старт src=off idle, shuf=%s rep=%s, media sd=%s usb=%s, bt=%s" % (
        st[2], st[3], ctx.base["sd"], ctx.base["usb"], ctx.base["bt"])


def sc_bad_checksum(ctx):
    good = make_frame("PLAY")
    bad = good[:-3] + b"%02X\n" % ((xor_sum(b"PLAY") + 1) & 0xFF)
    mark = ctx.rx.count()
    send_raw(ctx, bad)
    fr = silence(ctx, mark)
    if fr:
        return FAIL, "кадр із поганою сумою не мовчки відкинуто: " + fmt(fr)
    for name, raw in (("без суми", b"$PLAY\n"), ("порожня сума", b"$PLAY*\n"), ("одна цифра", b"$PLAY*0\n"),
                      ("не hex", b"$PLAY*ZZ\n")):
        mark = ctx.rx.count()
        send_raw(ctx, raw)
        fr = silence(ctx, mark, 0.4)
        if fr:
            return FAIL, "%s: не мовчки: %s" % (name, fmt(fr))
    fr = cmd(ctx, "PLAY")  # контроль: валідний PLAY при src=off → bad_state
    ok, d = expect(fr, [("ERR", ["bad_state", "PLAY"])])
    return (PASS, "поганий кадр — тиша, контроль відповів " + d) if ok else (FAIL, "контроль: " + d)


def sc_oversize(ctx):
    for n in (251, 400):
        mark = ctx.rx.count()
        send_raw(ctx, make_frame("X" * n))
        fr = silence(ctx, mark)
        if fr:
            return FAIL, "кадр із тілом %d Б (завеликий) не відкинуто: %s" % (n, fmt(fr))
    mark = ctx.rx.count()
    send_raw(ctx, make_frame("X" * 250))  # рівно 255 Б з '$' і LF — межа, має прийматись
    fr = collect(ctx, mark)
    if len(fr) != 1 or not match(fr[0], "ERR", ["unknown_cmd"]):
        return FAIL, "кадр 255 Б (межа) має прийматись → ERR,unknown_cmd: " + fmt(fr)
    det = fr[0].args[1] if len(fr[0].args) > 1 else ""
    if len(det) < 200 or det.strip("X"):
        return FAIL, "деталь ERR порожня/спотворена (довжина %d)" % len(det)
    return PASS, "251 і 400 Б — тиша; 250 Б тіла (255 Б кадр) прийнято, ERR із деталлю %d Б" % len(det)


def sc_no_dollar(ctx):
    for raw in (b"PLAY*04\n", b"GET\n", b"HELLO,1\n"):
        mark = ctx.rx.count()
        send_raw(ctx, raw)
        fr = silence(ctx, mark, 0.4)
        if fr:
            return FAIL, "%r без '$' не мовчки: %s" % (raw, fmt(fr))
    return PASS, "кадри без '$' — тиша"


def sc_resync(ctx):
    cases = [(b"junk\n$SRC,s$PLAY*04\r\n", "сміття + обірваний кадр + валідний (CRLF)"),
             (b"$$$PLAY*04\n", "кілька '$' підряд"),
             (b"$HB,$PLAY*04\n", "'$' усередині тіла")]
    for raw, what in cases:
        mark = ctx.rx.count()
        send_raw(ctx, raw)
        fr = collect(ctx, mark)
        ok, d = expect(fr, [("ERR", ["bad_state", "PLAY"])])
        if not ok:
            return FAIL, "%s: %s" % (what, d)
    return PASS, "ресинхронізація по '$' працює (3 варіанти), CR ігнорується"


def sc_unknown_cmd(ctx):
    for body, nm in (("FOO", "FOO"), ("play", "play"), ("FOO,1,2", "FOO"), ("DBG,heap", "DBG")):
        ok, d = expect(cmd(ctx, body), [("ERR", ["unknown_cmd", nm])])
        if not ok:
            return FAIL, "%s: %s" % (body, d)
    return PASS, "невідома команда → ERR,unknown_cmd,<NAME> (4 випадки, зокрема регістр і DBG з UART1)"


def sc_bad_arg(ctx):
    cases = [("SRC,xyz", "SRC"), ("SRC", "SRC"), ("SRC,sd,x", "SRC"), ("SHUF,2", "SHUF"), ("SHUF", "SHUF"),
             ("SHUF,-1", "SHUF"), ("SHUF,+1", "SHUF"), ("SHUF, 1", "SHUF"), ("SHUF,abc", "SHUF"), ("REP,3", "REP"),
             ("REP", "REP"), ("REP,x", "REP"), ("PLAY,1", "PLAY"), ("GET,1", "GET"), ("NEXT,1", "NEXT"), ("HELLO", "HELLO")]
    for body, nm in cases:
        ok, d = expect(cmd(ctx, body), [("ERR", ["bad_arg", nm])])
        if not ok:
            return FAIL, "%s: %s" % (body, d)
    fr = cmd(ctx, "GET")
    if not fr or fr[0].name != "STATE" or fr[0].args[2:4] != [ctx.base["shuf"], ctx.base["rep"]]:
        return FAIL, "після bad_arg змінився shuf/rep: " + fmt(fr)
    return PASS, "%d випадків bad_arg, стан не змінився" % len(cases)


def sc_not_implemented(ctx):
    for body in ("LIST,0,10", "LIST", "PLAYFILE,3", "PLAYFILE"):
        ok, d = expect(cmd(ctx, body), [("ERR", ["not_implemented"])])
        if not ok:
            return FAIL, "%s: %s" % (body, d)
    return PASS, "LIST/PLAYFILE → ERR,not_implemented"


def sc_off_controls(ctx):
    for nm in ("PLAY", "PAUSE", "TOGGLE", "STOP", "NEXT", "PREV", "FNEXT", "FPREV"):
        ok, d = expect(cmd(ctx, nm), [("ERR", ["bad_state", nm])])
        if not ok:
            return FAIL, "%s: %s" % (nm, d)
    return PASS, "керування при src=off → ERR,bad_state,<NAME> (8 команд), підтверджень нема"


def sc_hello(ctx):
    fr = cmd(ctx, "HELLO,1")
    ok, d = expect(fr, [("READY", [None, "1"]), ("STATE", ["off", "idle", ctx.base["shuf"], ctx.base["rep"]]),
                        ("MEDIA", [None, None]), ("BT", [None, None])])
    if not ok:
        return FAIL, "HELLO,1: " + d
    if not fr[0].args[0]:
        return FAIL, "READY без версії прошивки"
    return PASS, "HELLO,1 → READY(fw=%s,proto=1) + повний стан: %s" % (fr[0].args[0], fmt(fr[1:]))


def sc_hello_mismatch(ctx):
    fr = cmd(ctx, "HELLO,2")
    return INFO, "HELLO,2 (інша proto_ver) → %s; контракт мовчить про реакцію WROOM — відкрите питання в audit_M5c4.md" % fmt(fr)


def sc_get(ctx):
    fr = cmd(ctx, "GET")
    ok, d = expect(fr, [("STATE", ["off", "idle", None, None]), ("MEDIA", [None, None]), ("BT", [None, None])])
    if not ok:
        return FAIL, d
    m, b = fr[1].args, fr[2].args
    if any(x not in ("0", "1") for x in m):
        return FAIL, "MEDIA має бути 0/1: " + fmt(fr)
    if b[0] not in ("connected", "disconnected"):
        return FAIL, "BT: перше поле connected|disconnected: " + fmt(fr)
    return PASS, "GET при src=off → STATE, MEDIA, BT (без TRACK/FOLDER/META/TIME): " + fmt(fr)


def sc_media_usb(ctx):
    exp = str(ctx.args.usb_expected)
    got = ctx.base.get("usb")
    if got == exp:
        return PASS, "MEDIA usb=%s (як очікувалось для збірки без CH376S)" % got
    return FAIL, "MEDIA usb=%s, очікувалось %s: USB не змонтовано (ENABLE_USB_CH376=0), але подія каже інакше — відкрите питання (STUB_USB_PRESENT)" % (got, exp)


def sc_src_off_idem(ctx):
    mark = ctx.rx.count()
    send(ctx, "SRC,off")
    fr = silence(ctx, mark)
    return (PASS, "SRC,off при src=off — без події (STATE лише при зміні)") if not fr else (FAIL, "зайві кадри: " + fmt(fr))


def sc_src_usb(ctx):
    try:
        fr = cmd(ctx, "SRC,usb")
        ok, d = expect(fr, [("STATE", ["usb", "idle", ctx.base["shuf"], ctx.base["rep"]]), ("ERR", ["no_media"])])
        return (PASS, "SRC,usb без носія → STATE,usb,idle + ERR,no_media") if ok else (FAIL, "SRC,usb: " + d + " (відкрите питання, див. audit)")
    finally:
        send(ctx, "SRC,off")
        settle(ctx, 1.0)


def sc_src_sd_nomedia(ctx):
    if ctx.base.get("sd") != "0":
        return SKIP, "картка SD вставлена; для сценарію витягніть її й запустіть: --only src_sd_nomedia"
    try:
        fr = cmd(ctx, "SRC,sd")
        ok, d = expect(fr, [("STATE", ["sd", "idle", ctx.base["shuf"], ctx.base["rep"]]), ("ERR", ["no_media"])])
        return (PASS, "SRC,sd без картки → STATE,sd,idle + ERR,no_media") if ok else (FAIL, "SRC,sd: " + d)
    finally:
        send(ctx, "SRC,off")
        settle(ctx, 1.0)


def sc_shuf_rep(ctx):
    s0, r0 = ctx.base["shuf"], ctx.base["rep"]
    s1 = "1" if s0 == "0" else "0"
    r1 = "2" if r0 != "2" else "1"
    steps = [("SHUF," + s1, [s1, r0]), ("REP," + r1, [s1, r1]), ("SHUF," + s0, [s0, r1]), ("REP," + r0, [s0, r0])]
    try:
        for body, (sh, rp) in steps:
            ok, d = expect(cmd(ctx, body), [("STATE", ["off", "idle", sh, rp])])
            if not ok:
                return FAIL, "%s: %s" % (body, d)
        mark = ctx.rx.count()
        send(ctx, "SHUF," + s0)  # те саме значення — події нема
        fr = silence(ctx, mark)
        if fr:
            return FAIL, "SHUF без зміни дав кадри: " + fmt(fr)
        return PASS, "SHUF/REP звітуються STATE,off,idle,<shuf>,<rep> при кожній зміні й лише при зміні; початкові значення відновлено"
    finally:
        for b in ("SHUF," + s0, "REP," + r0):
            send(ctx, b)
        settle(ctx, 0.5)


def sc_meta_escape(ctx):
    exp = ctx.args.expect_title
    if not exp:
        return SKIP, "не задано --expect-title (див. hwcheck_M5c4.md, сценарій META)"
    if ctx.base.get("sd") != "1":
        return SKIP, "картка SD не змонтована"
    found = None
    try:
        mark = ctx.rx.count()
        send(ctx, "SRC,sd")
        t_end = time.monotonic() + 25
        nexts = 0
        seen = 0
        while time.monotonic() < t_end and not found:
            time.sleep(0.25)
            for f in non_hb(ctx.rx.since(mark)):
                if f.name == "META" and len(f.args) == 3 and f.args[1] == exp:
                    found = f
                    break
            frs = non_hb(ctx.rx.since(mark))
            st = [f for f in frs if f.name == "STATE" and f.args[0] == "sd"]
            if not found and st and st[-1].args[1] in ("play", "stop") and len(frs) == seen and nexts < ctx.args.max_next:
                nexts += 1  # скан завершено, а потрібного треку ще не було — наступний
                send(ctx, "NEXT")
                time.sleep(1.0)
            seen = len(frs)
        if not found:
            return FAIL, "META з назвою %r не отримано (отримано META: %s)" % (
                exp, fmt([f for f in non_hb(ctx.rx.since(mark)) if f.name == "META"][:6]))
        raw = found.raw
        if raw.count(b",") != 3:
            return FAIL, "у META сира кома у полі: %r" % raw
        for ch, esc in ((",", b"%2C"), ("%", b"%25"), ("$", b"%24")):
            if ch in exp and esc not in raw:
                return FAIL, "символ %r не екрановано (%s) у %r" % (ch, esc.decode(), raw)
        run = ""
        for ch in exp:
            if ch in ",%$*" or ord(ch) < 0x20:
                break
            run += ch
        if run and any(ord(c) > 0x7F for c in run) and run.encode("utf-8") not in raw:
            return FAIL, "кирилиця/не-ASCII не передана як є: %r" % raw
        if len(raw) + 2 > MAX_FRAME:  # raw = тіло*XX без '$' і LF
            return FAIL, "кадр META довший за 255 Б"
        return PASS, "META з назвою %r: екранування й UTF-8 коректні (%d Б)" % (exp, len(raw) + 2)
    finally:
        send(ctx, "SRC,off")
        settle(ctx, 1.5)


def sc_final(ctx):
    with ctx.rx.lock:
        hb = [f for f in ctx.rx.frames if f.name == "HB"]
    seqs = [int(f.args[0]) for f in hb]
    gaps = [(a, b) for a, b in zip(seqs, seqs[1:]) if (a + 1) % 65536 != b]
    bad = []
    if ctx.rx.bad:
        bad.append("биті/завеликі кадри від модуля: %d" % ctx.rx.bad)
    if ctx.rx.max_len > MAX_FRAME:
        bad.append("кадр модуля %d Б > 255" % ctx.rx.max_len)
    if gaps:
        bad.append("пропуски seq HB: %s" % gaps[:5])
    if bad:
        return FAIL, "; ".join(bad)
    return PASS, "увесь прогін: %d HB без пропусків seq, усі кадри модуля валідні (макс. %d Б)" % (len(hb), ctx.rx.max_len)


SCENARIOS = [
    ("hb", sc_hb),
    ("precond", sc_precond),
    ("bad_checksum", sc_bad_checksum),
    ("oversize", sc_oversize),
    ("no_dollar", sc_no_dollar),
    ("resync", sc_resync),
    ("unknown_cmd", sc_unknown_cmd),
    ("bad_arg", sc_bad_arg),
    ("not_implemented", sc_not_implemented),
    ("off_controls", sc_off_controls),
    ("hello", sc_hello),
    ("hello_mismatch", sc_hello_mismatch),
    ("get", sc_get),
    ("media_usb", sc_media_usb),
    ("src_off_idem", sc_src_off_idem),
    ("src_usb", sc_src_usb),
    ("src_sd_nomedia", sc_src_sd_nomedia),
    ("shuf_rep", sc_shuf_rep),
    ("meta_escape", sc_meta_escape),
    ("final", sc_final),
]


def main(argv=None):
    ap = argparse.ArgumentParser(description="M5c4: перевірка UART-протоколу модуля (UART1, 921600 8N1)")
    ap.add_argument("port", nargs="?", help="COM-порт USB-UART адаптера, напр. COM7")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--only", help="імена сценаріїв через кому (precond додається автоматично)")
    ap.add_argument("--list", action="store_true", help="показати сценарії й вийти")
    ap.add_argument("--usb-expected", type=int, default=0, choices=(0, 1), help="очікуване MEDIA usb (0 без CH376S)")
    ap.add_argument("--expect-title", help="очікувана назва треку (імʼя файлу без .mp3) для сценарію meta_escape")
    ap.add_argument("--max-next", type=int, default=5, help="скільки разів NEXT шукати трек для meta_escape")
    args = ap.parse_args(argv)

    if args.list:
        for n, f in SCENARIOS:
            print("%-16s %s" % (n, (f.__doc__ or "").strip()))
        return 0
    if not args.port:
        ap.error("потрібен COM-порт (напр. COM7)")
    if serial is None:
        print("Потрібен pyserial:  pip install pyserial")
        return 2
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=2)
    except Exception as e:  # noqa: BLE001
        print("Не вдалось відкрити %s: %s" % (args.port, e))
        return 2

    rx = Rx(ser)
    rx.start()
    ctx = Ctx(ser, rx, args)
    only = set(args.only.split(",")) | {"precond"} if args.only else None
    fails = 0
    t0 = time.monotonic()
    try:
        for name, fn in SCENARIOS:
            if only and name not in only and name != "final":
                continue
            if name not in ("hb", "precond", "final") and not ctx.base:
                print("[SKIP] %-16s немає передумови" % name)
                continue
            try:
                st, det = fn(ctx)
            except Exception as e:  # noqa: BLE001
                st, det = FAIL, "виняток: %r" % e
            print("[%s] %-16s %s" % (st, name, det))
            sys.stdout.flush()
            if st == FAIL:
                fails += 1
                if name == "precond":
                    print("Передумова не виконана — подальші сценарії безглузді (S3 відключено? модуль у src=off?).")
                    return 2
    finally:
        try:
            send(ctx, "SRC,off")
            time.sleep(0.3)
        except Exception:  # noqa: BLE001
            pass
        rx.stop_flag = True
        rx.join(1.0)
        ser.close()
    print("\nПідсумок: %s (FAIL: %d), %.0f с; кадрів від модуля: %d, биті: %d" % (
        "УСПІХ" if not fails else "ПРОВАЛ", fails, time.monotonic() - t0, rx.count(), rx.bad))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
