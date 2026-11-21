#!/usr/bin/env python3
"""M4c2: перевірка присутності USB-флешки (hot-plug) і неблокуючого відкриття USB на залізі. Інтерактивно: скрипт просить вставити/вийняти флешку.

Потрібно: збірка esp32dev_usb (USB_PRESENCE_MODE=1; або esp32dev_usb_poll), USB-UART адаптер на UART1 модуля (921600), pyserial.

  python tools/m4c2_hotplug.py --ctl COMx [--dbg COMy] [--cycles 10] [--unsupported] [--sd] [--bt] [--no-flash-latency]

Кроки (кожен — PASS/FAIL/INFO/SKIP):
  insert / remove   MEDIA usb=1/0 після вставлення/виймання; час від Enter (разом із реакцією оператора ≈0,3–0,5 с)
  unsupported       (--unsupported) флешка з чужою ФС: MEDIA usb лишається 0
  sd                (--sd) вставлення/виймання, поки грає SD: STATE не змінюється, HB без пропусків
  bt                (--bt) під BT з телефоном MEDIA usb не змінюється; після виходу з BT — оновлюється
  src_latency       SRC,usb → STATE,usb,scan ≤ 0,5 с; SRC,sd під час відкриття → STATE,sd ≤ 0,3 с (--cycles разів)
  cmd_latency       GET під час відкриття USB відповідає ≤ 150 мс (ціль ≤ 100 мс)
  no_flash_src      (--no-flash-latency) без флешки SRC,usb → STATE,usb,idle + ERR,no_media ≤ 0,5 с
Код виходу 0 — немає FAIL.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5c4_frames import Ctx, Rx, non_hb, send  # noqa: E402

PASS, FAIL, SKIP, INFO = "PASS", "FAIL", "SKIP", "INFO"
results = []


def rec(name, status, detail):
    results.append((name, status, detail))
    print("[%s] %s: %s" % (status, name, detail), flush=True)


def ask(text):
    return input("\n>>> %s\n    (Enter — виконано/готово) " % text)


def last_media(ctx):
    m = [f for f in ctx.rx.since(0) if f.name == "MEDIA"]
    return m[-1].args if m else None


def get_media(ctx, timeout=3.0):
    mark = ctx.rx.count()
    send(ctx, "GET")
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        for f in ctx.rx.since(mark):
            if f.name == "MEDIA":
                return f.args
        time.sleep(0.02)
    return None


def wait_frame(ctx, mark, pred, timeout):
    """Перший кадр після mark, що задовольняє pred; повертає (кадр, час його приходу) або (None, None)."""
    t0 = time.monotonic()
    seen = mark
    while time.monotonic() - t0 < timeout:
        fr = ctx.rx.since(seen)
        for f in fr:
            if pred(f):
                return f, f.t
        time.sleep(0.01)
    return None, None


def hb_gaps(ctx, mark):
    hb = [f.t for f in ctx.rx.since(mark) if f.name == "HB"]
    return max((b - a for a, b in zip(hb, hb[1:])), default=0.0)


def media_wait(ctx, want, t_ref, timeout, mark=None):
    """Час від t_ref до MEDIA usb=want. mark — позиція в потоці кадрів ДО підказки: якщо оператор вставив/вийняв флешку раніше,
    ніж натиснув Enter, кадр уже в потоці й повертається від'ємний час (подія була до Enter)."""
    if mark is None:
        mark = ctx.rx.count()
    f, t = wait_frame(ctx, mark, lambda f: f.name == "MEDIA" and len(f.args) > 1 and f.args[1] == want, timeout)
    return None if f is None else t - t_ref


def step_insert_remove(ctx, args):
    send(ctx, "SRC,off")
    time.sleep(1.0)
    m = get_media(ctx)
    rec("start", INFO, "MEDIA %s (початково флешки в гнізді не має бути; фонова ініціалізація чіпа — через ≈2–3 с після старту)" % (m,))
    ask("Вийміть флешку з гнізда CH376S (якщо вона там), зачекайте 3 с")
    m = get_media(ctx)
    if m is None or m[1] != "0":
        rec("insert", FAIL, "перед вставленням MEDIA usb=%s, очікувалось 0 (DBG,usb,pres у консолі)" % (m,))
        return
    for i in range(args.repeat):
        mark = ctx.rx.count()
        ask("Вставте флешку (FAT32/exFAT) і натисніть Enter У МОМЕНТ вставлення [%d/%d]" % (i + 1, args.repeat))
        t_ref = time.monotonic()
        dt = media_wait(ctx, "1", t_ref, 8.0, mark)
        extra = non_hb(ctx.rx.since(mark))
        stray = [f.text() for f in extra if f.name in ("STATE", "ERR")]
        if dt is None:
            rec("insert", FAIL, "MEDIA usb=1 не з'явилось за 8 с (INT# не доходить? DBG,usb,pres / DBG,usb,int)")
        elif dt < 0:
            rec("insert", PASS, "MEDIA usb=1 прийшло за %.2f с ДО Enter (флешку вставлено раніше за Enter): час реакції не виміряно, повторіть, натискаючи Enter одночасно з вставленням" % -dt)
        elif stray:
            rec("insert", FAIL, "MEDIA usb=1 через %.2f с, але з'явились зайві кадри: %s" % (dt, stray))
        else:
            rec("insert", PASS if dt <= 2.5 else FAIL, "MEDIA usb=1 через %.2f с від Enter (з реакцією оператора; ціль ≤ 1–1.5 с + ≈0,4)" % dt)
        mark = ctx.rx.count()
        ask("Вийміть флешку і натисніть Enter У МОМЕНТ виймання [%d/%d]" % (i + 1, args.repeat))
        t_ref = time.monotonic()
        dt = media_wait(ctx, "0", t_ref, 8.0, mark)
        if dt is None:
            rec("remove", FAIL, "MEDIA usb=0 не з'явилось за 8 с")
        elif dt < 0:
            rec("remove", PASS, "MEDIA usb=0 прийшло за %.2f с ДО Enter (флешку вийнято раніше за Enter): час реакції не виміряно" % -dt)
        else:
            rec("remove", PASS if dt <= 2.0 else FAIL, "MEDIA usb=0 через %.2f с від Enter" % dt)


def step_unsupported(ctx, args):
    if not args.unsupported:
        rec("unsupported", SKIP, "без --unsupported")
        return
    mark = ctx.rx.count()
    ask("Вставте флешку з НЕпідтримуваною ФС (NTFS/порожня без розділів) і натисніть Enter")
    time.sleep(6.0)
    ups = [f.text() for f in ctx.rx.since(mark) if f.name == "MEDIA" and f.args[1] == "1"]
    m = get_media(ctx)
    rec("unsupported", PASS if not ups and m and m[1] == "0" else FAIL, "MEDIA після вставлення: %s (очікувалось usb=0, без MEDIA usb=1)" % (m,))
    ask("Вийміть флешку")


def step_sd(ctx, args):
    if not args.sd:
        rec("sd", SKIP, "без --sd")
        return
    send(ctx, "SRC,sd")
    time.sleep(5.0)
    mark = ctx.rx.count()
    ask("SD грає? Вставте флешку, зачекайте 3 с, вийміть її, зачекайте 3 с (потім Enter)")
    fr = ctx.rx.since(mark)
    states = [f.text() for f in fr if f.name == "STATE"]
    medias = [f.text() for f in fr if f.name == "MEDIA"]
    gap = hb_gaps(ctx, mark)
    ok = not states and gap <= 1.3 and any("MEDIA,0,1" in x or ",1" in x for x in medias)
    rec("sd", PASS if ok else FAIL, "STATE за час: %s; MEDIA: %s; найбільша пауза між HB %.2f с (≤ 1,3)" % (states or "немає", medias, gap))
    send(ctx, "SRC,off")


def step_bt(ctx, args):
    if not args.bt:
        rec("bt", SKIP, "без --bt")
        return
    send(ctx, "SRC,bt")
    ask("Підключіть телефон до BT і запустіть музику; переконайтесь, що вона грає. Флешки в гнізді немає")
    mark = ctx.rx.count()
    ask("Вставте флешку; чекаємо 6 с без дій")
    time.sleep(6.0)
    ch = [f.text() for f in ctx.rx.since(mark) if f.name == "MEDIA"]
    rec("bt/defer", PASS if not ch else FAIL, "під BT з телефоном MEDIA не мало змінюватись (heap ≈17 КБ < 20 КБ, обмін відкладено): %s" % (ch or "тиша"))
    mark = ctx.rx.count()
    send(ctx, "SRC,off")
    t_ref = time.monotonic()
    dt = media_wait(ctx, "1", t_ref, 12.0)
    rec("bt/after", PASS if dt is not None and dt <= 6.0 else FAIL,
        "MEDIA usb=1 через %s після виходу з BT" % ("%.2f с" % dt if dt is not None else "— (не з'явилось за 12 с)"))
    ask("Вийміть флешку (потім Enter)")


def step_src_latency(ctx, args):
    m = get_media(ctx)
    if not m or m[1] != "1":
        ask("Вставте флешку з mp3 і зачекайте 3 с")
    worst_scan = worst_back = 0.0
    bad = []
    for i in range(args.cycles):
        send(ctx, "SRC,off")
        time.sleep(1.0)
        mark = ctx.rx.count()
        t0 = time.monotonic()
        send(ctx, "SRC,usb")
        f, t = wait_frame(ctx, mark, lambda f: f.name == "STATE" and f.args and f.args[0] == "usb", 3.0)
        if f is None:
            bad.append("цикл %d: немає STATE,usb" % (i + 1))
            continue
        d1 = t - t0
        worst_scan = max(worst_scan, d1)
        time.sleep(0.3 + (i % 4) * 0.25)  # перервати відкриття на різних кроках
        mark = ctx.rx.count()
        t1 = time.monotonic()
        send(ctx, "SRC,sd")
        f, t = wait_frame(ctx, mark, lambda f: f.name == "STATE" and f.args and f.args[0] == "sd", 3.0)
        if f is None:
            bad.append("цикл %d: немає STATE,sd" % (i + 1))
            continue
        worst_back = max(worst_back, t - t1)
    send(ctx, "SRC,off")
    ok = not bad and worst_scan <= 0.5 and worst_back <= 0.3
    rec("src_latency", PASS if ok else FAIL,
        "SRC,usb→STATE,usb %.0f мс (≤ 500); SRC,sd під час відкриття→STATE,sd %.0f мс (≤ 300; звільнення USB ≤ 100 мс дивіться в UART0 «ресурси звільнено за N мс») %s"
        % (worst_scan * 1000, worst_back * 1000, bad or ""))


def step_cmd_latency(ctx, args):
    worst = 0.0
    n = 0
    for rep in range(max(2, args.cycles // 3)):
        send(ctx, "SRC,off")
        time.sleep(1.0)
        send(ctx, "SRC,usb")
        t_end = time.monotonic() + 1.5
        while time.monotonic() < t_end:
            mark = ctx.rx.count()
            t0 = time.monotonic()
            send(ctx, "GET")
            f, t = wait_frame(ctx, mark, lambda f: f.name == "MEDIA", 1.0)
            if f is not None:
                worst = max(worst, t - t0)
                n += 1
            time.sleep(0.04)
    send(ctx, "SRC,off")
    rec("cmd_latency", PASS if n and worst <= 0.15 else FAIL,
        "GET під час відкриття USB: %d відповідей, найдовша %.0f мс (ціль ≤ 100, допуск ≤ 150)" % (n, worst * 1000))


def step_no_flash(ctx, args):
    if not args.no_flash_latency:
        rec("no_flash_src", SKIP, "без --no-flash-latency")
        return
    ask("Вийміть флешку і зачекайте 3 с")
    worst = 0.0
    for i in range(3):
        send(ctx, "SRC,off")
        time.sleep(1.0)
        mark = ctx.rx.count()
        t0 = time.monotonic()
        send(ctx, "SRC,usb")
        time.sleep(1.2)
        fr = non_hb(ctx.rx.since(mark))
        names = [f.text() for f in fr]
        ok = any(f.name == "STATE" and f.args[:2] == ["usb", "idle"] for f in fr) and any(f.name == "ERR" and f.args[:1] == ["no_media"] for f in fr)
        if not ok:
            rec("no_flash_src", FAIL, "очікувалось STATE,usb,idle + ERR,no_media, отримано %s" % names)
            return
        worst = max(worst, fr[-1].t - t0)
    rec("no_flash_src", PASS if worst <= 0.5 else FAIL, "SRC,usb без флешки → idle + ERR,no_media за %.0f мс (раніше ≈3 с)" % (worst * 1000))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ctl", required=True, help="COM-порт USB-UART адаптера на UART1 модуля (921600)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--repeat", type=int, default=3, help="скільки разів вставити/вийняти (за замовч. 3)")
    ap.add_argument("--cycles", type=int, default=10, help="циклів SRC,usb/SRC,sd (за замовч. 10)")
    ap.add_argument("--unsupported", action="store_true")
    ap.add_argument("--sd", action="store_true")
    ap.add_argument("--bt", action="store_true")
    ap.add_argument("--no-flash-latency", action="store_true")
    args = ap.parse_args()
    import serial  # noqa: PLC0415

    ser = serial.Serial(args.ctl, args.baud, timeout=0.05)
    rx = Rx(ser)
    rx.start()
    ctx = Ctx(ser, rx, args)
    try:
        time.sleep(1.5)
        step_insert_remove(ctx, args)
        step_unsupported(ctx, args)
        step_sd(ctx, args)
        step_bt(ctx, args)
        step_no_flash(ctx, args)
        step_src_latency(ctx, args)
        step_cmd_latency(ctx, args)
    finally:
        try:
            send(ctx, "SRC,off")
        except Exception:  # noqa: BLE001
            pass
        rx.stop_flag = True
        ser.close()
    bad = [r for r in results if r[1] == FAIL]
    print("\n==== Підсумок: %d PASS, %d FAIL, %d SKIP ====" % (sum(r[1] == PASS for r in results), len(bad), sum(r[1] == SKIP for r in results)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
