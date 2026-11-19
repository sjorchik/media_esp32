#!/usr/bin/env python3
"""M5c6: сценарій «резюме в хвості треку» для SD і USB (одна плата, один порт консолі).
Для кожної комбінації REP 0/1/2 x SHUF 0/1 x хвіст (мс): SRC,<src> -> play -> DBG,resume,tail,<N> -> SRC,off -> SRC,<src> -> очікуємо STATE,<src>,play за <= --limit с.
Приклад: python tools\\m5c6_resume_tail.py --port COM5 --src sd
         python tools\\m5c6_resume_tail.py --port COM5 --src usb --limit 9
Потрібна одна доріжка >= 10 с на картці/флешці (краще єдиний трек). Порт: консоль плати (UART0; без S3 приймає і SRC/REP/SHUF)."""
import argparse, re, sys, time
import serial  # pip install pyserial

STATE = re.compile(r"STATE,(\w+),(\w+)")


class Dev:
    def __init__(self, port, baud):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.dtr = False
        self.s.rts = False
        self.log = []

    def send(self, line):
        self.s.write((line + "\n").encode())

    def pump(self, secs):
        end = time.time() + secs
        out = []
        while time.time() < end:
            ln = self.s.readline().decode("utf-8", "replace").strip()
            if ln:
                out.append(ln)
                self.log.append(ln)
        return out

    def wait(self, pred, timeout):
        t0 = time.time()
        seen = []
        while time.time() - t0 < timeout:
            for ln in self.pump(0.1):
                seen.append(ln)
                if pred(ln):
                    return time.time() - t0, seen
        return None, seen


def want_state(src, st):
    def p(ln):
        m = STATE.search(ln)
        return bool(m) and m.group(1) == src and m.group(2) == st
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=460800)
    ap.add_argument("--src", choices=["sd", "usb"], default="sd")
    ap.add_argument("--limit", type=float, default=2.0, help="макс. с від SRC до STATE,<src>,play (USB: скан ≈3 с + відкриття ≈1,3 с)")
    ap.add_argument("--tails", default="0,200,1000,3000")
    a = ap.parse_args()
    d = Dev(a.port, a.baud)
    tails = [int(x) for x in a.tails.split(",")]
    fails, total = [], 0
    for rep in (0, 1, 2):
        for shuf in (0, 1):
            for tail in tails:
                total += 1
                tag = f"REP={rep} SHUF={shuf} tail={tail}"
                d.send("SRC,off"); d.pump(1.0)
                d.send(f"REP,{rep}"); d.send(f"SHUF,{shuf}"); d.pump(0.3)
                d.send(f"SRC,{a.src}")
                t, _ = d.wait(want_state(a.src, "play"), 40)
                if t is None:
                    fails.append(f"{tag}: перший play не настав"); continue
                d.pump(3.0)  # тривалість треку стає відомою після першого кадру
                d.send(f"DBG,resume,tail,{tail}")
                _, seen = d.wait(lambda l: "resume,tail:" in l, 4)
                if not any("записано" in l for l in seen):
                    fails.append(f"{tag}: DBG,resume,tail не записав ({' | '.join(seen[-2:])})"); continue
                d.send("SRC,off")
                d.wait(want_state("off", "stop"), 5)
                d.pump(0.5)
                d.send(f"SRC,{a.src}")
                t, seen = d.wait(want_state(a.src, "play"), a.limit + 6)
                stall = any("сторожа" in l for l in d.log[-200:])
                if t is None or t > a.limit or stall:
                    fails.append(f"{tag}: play за {t if t is not None else 'ніколи'} с (ліміт {a.limit}){' + сторожа' if stall else ''}")
                    d.send("SRC,off"); d.pump(2.0)
                else:
                    print(f"ok   {tag}: play за {t:.2f} с")
                d.pump(1.0)
    d.send("SRC,off")
    print(f"\nПІДСУМОК: {total - len(fails)}/{total} ok")
    for f in fails:
        print("FAIL:", f)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
