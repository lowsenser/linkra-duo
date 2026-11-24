#!/usr/bin/env python3
"""A/B tuning of the CC1101 receive settings on a running Linkra Duo.

Each variant is applied to the tuning slot (tune433 / tune868) via
POST /api/rf/tune and held for --seconds. Variants are interleaved with the
baseline (A B A C A D ...) so slow changes in the RF environment average out.
Decoded events are read from the SSE stream and attributed to the variant that
was active when they arrived.

usage:
  python3 scripts/tune_matrix.py --band 868 --seconds 60 [--host HOST] [--rounds 1]
--host defaults to $LINKRA_HOST, else linkra-duo.local.
Prints, per variant, decodes per reference device, median RSSI and SNR.
"""
import argparse, json, os, statistics, threading, time, urllib.request
from collections import defaultdict

BASE_868 = dict(band="868", mod="FSK", freqMHz=868.35, bwKHz=270, bitrateKbps=17.24, devKHz=40,
                agc2=0x07, agc1=0x00, agc0=0x91, frend1=0xB6, fsctrl1=0x06)
VARIANTS_868 = [
    ("bw203",        dict(bwKHz=203)),
    ("bw325",        dict(bwKHz=325)),
    ("f868.30",      dict(freqMHz=868.30)),
    ("f868.40",      dict(freqMHz=868.40)),
    ("agcTI",        dict(agc2=0x43, agc1=0x40, agc0=0x91)),   # TI SmartRF 2-FSK recommendation
    ("agcUpstream",  dict(agc2=0xC7, agc1=0x40, agc0=0x93)),
    ("br38",         dict(bitrateKbps=38.4, minPulseUs=15)),
    ("dev50",        dict(devKHz=50)),
]
BASE_433 = dict(band="433", mod="OOK", freqMHz=433.92, bwKHz=203, bitrateKbps=3.79, devKHz=0, minPulseUs=0,
                agc2=0xC7, agc1=0x40, agc0=0x93, frend1=0xB6, fsctrl1=0x06)
VARIANTS_433 = [
    ("bw102",        dict(bwKHz=102)),
    ("bw58",         dict(bwKHz=58)),
    ("bw270",        dict(bwKHz=270)),
    ("bw325",        dict(bwKHz=325)),
    ("bw406",        dict(bwKHz=406)),
    ("f433.88",      dict(freqMHz=433.88)),
    ("f433.96",      dict(freqMHz=433.96)),
    ("agcFlipper",   dict(agc2=0x07, agc1=0x00, agc0=0x91)),
    ("agcMaxLna",    dict(agc2=0x03, agc1=0x00, agc0=0x91)),
    ("frend56",      dict(frend1=0x56)),
    ("tiOok",        dict(agc2=0x03, agc1=0x00, agc0=0x91, fsctrl1=0x08)),   # TI DN022 OOK
    ("c433.67-541",  dict(freqMHz=433.67, bwKHz=541)),   # covers 433.42 (Somfy) and 433.92
    ("c433.67-650",  dict(freqMHz=433.67, bwKHz=650)),
]


def post(host, path, body):
    req = urllib.request.Request(f"http://{host}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", "X-Linkra": "1"}, method="POST")
    with urllib.request.urlopen(req, timeout=8) as r:
        return r.read()


class Collector(threading.Thread):
    def __init__(self, host):
        super().__init__(daemon=True)
        self.host, self.label, self.events, self.lock = host, None, [], threading.Lock()

    def run(self):
        while True:
            try:
                req = urllib.request.Request(f"http://{self.host}/api/events", headers={"Accept": "text/event-stream"})
                with urllib.request.urlopen(req, timeout=30) as r:
                    for raw in r:
                        line = raw.decode("utf8", "replace").strip()
                        if not line.startswith("data: {") or '"decoded":true' not in line:
                            continue
                        try:
                            j = json.loads(line[6:])
                        except ValueError:
                            continue
                        with self.lock:
                            if self.label:
                                self.events.append((self.label, j))
            except Exception:
                time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=os.environ.get("LINKRA_HOST", "linkra-duo.local"))
    ap.add_argument("--band", choices=["433", "868"], default="868")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--rounds", type=int, default=1)
    ap.add_argument("--only", nargs="*", help="subset of variant names")
    a = ap.parse_args()
    base, variants = (BASE_868, VARIANTS_868) if a.band == "868" else (BASE_433, VARIANTS_433)
    if a.only:
        variants = [v for v in variants if v[0] in a.only]

    col = Collector(a.host); col.start(); time.sleep(2)
    plan = []
    for _ in range(a.rounds):
        for name, delta in variants:
            plan += [("BASE", {}), (name, delta)]
    plan.append(("BASE", {}))
    dwell = defaultdict(float)
    total = len(plan) * a.seconds
    print(f"{len(plan)} Fenster à {a.seconds}s = {total // 60} min, Band {a.band}")
    for i, (name, delta) in enumerate(plan):
        params = dict(base, **delta, durationMs=(a.seconds + 15) * 1000)
        post(a.host, "/api/rf/tune", params)
        time.sleep(1.5)                      # settle (RSSI averaging) before counting
        with col.lock:
            col.label = name
        t0 = time.time(); time.sleep(a.seconds); dwell[name] += time.time() - t0
        with col.lock:
            col.label = None
        print(f"  [{i + 1}/{len(plan)}] {name} fertig")
    post(a.host, "/api/rf/profile", {"rotation": True})

    tune_slot = "tune868" if a.band == "868" else "tune433"
    stats = defaultdict(lambda: defaultdict(list))
    for label, j in col.events:
        if j.get("rx_profile") != tune_slot:
            continue
        dev = f"{j.get('model')}#{j.get('id')}"
        stats[label][dev].append((j.get("rssi"), j.get("snr_dB")))
    devices = sorted({d for s in stats.values() for d in s})
    print(f"\nDecodes pro Minute (Median RSSI / SNR) je Variante, Slot {tune_slot}:")
    print(f"{'Variante':14}" + "".join(f"{d[:24]:>27}" for d in devices) + f"{'gesamt/min':>12}")
    order = ["BASE"] + [v[0] for v in variants]
    for name in order:
        minutes = dwell[name] / 60 or 1
        row, tot = f"{name:14}", 0
        for d in devices:
            v = stats[name].get(d, [])
            tot += len(v)
            rs = [x[0] for x in v if x[0] is not None]; sn = [x[1] for x in v if x[1] is not None]
            cell = f"{len(v) / minutes:5.1f} ({statistics.median(rs) if rs else '-'}/{statistics.median(sn) if sn else '-'})" if v else "    -"
            row += f"{cell:>27}"
        print(row + f"{tot / minutes:12.1f}")


if __name__ == "__main__":
    main()
