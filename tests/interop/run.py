#!/usr/bin/env python3
"""Interop tests: the xsyncthing host build against stock Syncthing.

  tests/interop/run.py --syncthing PATH --xsyncthing PATH [--work DIR] [--fatx]

Starts a stock Syncthing (A) and xsyncthing (B) on loopback with discovery and relays off, shares one send-receive
folder, and checks, each within a deadline:
  1. files and directories created on A appear on B, byte for byte; and the other way round
  2. modifications propagate both ways
  3. deletions propagate both ways
  4. a concurrent edit leaves one winner on both sides and a .sync-conflict- copy
  5. A's REST API reports B's folder completion as 100%
Prints PASS/FAIL per check; exits non-zero on any failure. --fatx runs B with the console's FATX rules.
"""
import argparse
import json
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request

A_GUI = 18485
A_PORT = 22201
B_PORT = 22202


def http(key, method, path, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{A_GUI}{path}", method=method,
                                 data=None if body is None else json.dumps(body).encode(),
                                 headers={"X-API-Key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        data = r.read()
        return json.loads(data) if data else None


def wait_for(what, fn, timeout=60):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = fn()
            if last:
                return True
        except Exception as e:  # not up yet, file mid-write, ...
            last = e
        time.sleep(0.5)
    print(f"  (timed out waiting for {what}: {last})")
    return False


def same(a, b):
    try:
        return open(a, "rb").read() == open(b, "rb").read()
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--syncthing", required=True)
    ap.add_argument("--xsyncthing", required=True)
    ap.add_argument("--work", default="/tmp/xst-interop")
    ap.add_argument("--fatx", action="store_true")
    a = ap.parse_args()

    w = a.work
    shutil.rmtree(w, ignore_errors=True)
    for d in ("a-home", "b-home", "a-data", "b-data"):
        os.makedirs(os.path.join(w, d))
    a_data, b_data = os.path.join(w, "a-data"), os.path.join(w, "b-data")
    procs = []
    results = []

    def check(name, ok):
        results.append(ok)
        print(("PASS" if ok else "FAIL") + ": " + name, flush=True)

    try:
        subprocess.run([a.syncthing, "generate", "--home", os.path.join(w, "a-home")], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        aid = subprocess.run([a.syncthing, "device-id", "--home", os.path.join(w, "a-home")], check=True,
                             capture_output=True, text=True).stdout.strip()

        env = dict(os.environ)
        if a.fatx:
            env["XST_FS_RULES"] = "fatx"
        # B's first run makes its identity and config.
        b_log = open(os.path.join(w, "b.log"), "w")
        p = subprocess.Popen([a.xsyncthing, "--home", os.path.join(w, "b-home"), "--name", "xst-b", "--no-api"],
                             stdout=b_log, stderr=subprocess.STDOUT, env=env)
        ok = wait_for("B's config", lambda: os.path.exists(os.path.join(w, "b-home", "config.json")), 20)
        time.sleep(1)
        p.send_signal(signal.SIGTERM)
        p.wait(20)
        bid = re.search(r"device ID ([A-Z0-9-]{63})", open(os.path.join(w, "b.log")).read()).group(1)
        check("B starts and makes an identity", ok and bool(bid))

        cfg_path = os.path.join(w, "b-home", "config.json")
        c = json.load(open(cfg_path))
        c["options"].update(listenAddresses=[f"tcp://127.0.0.1:{B_PORT}"], globalAnnounceEnabled=False,
                            localAnnounceEnabled=False, relaysEnabled=False)
        dev = dict(c["defaults"]["device"])
        dev.update(deviceID=aid, name="stock-a", addresses=[f"tcp://127.0.0.1:{A_PORT}"])
        c["devices"].append(dev)
        f = dict(c["defaults"]["folder"])
        f.update(id="interop", label="Interop", path=b_data, rescanIntervalS=3,
                 devices=[{"deviceID": aid, "introducedBy": "", "encryptionPassword": ""},
                          {"deviceID": bid, "introducedBy": "", "encryptionPassword": ""}])
        c["folders"].append(f)
        json.dump(c, open(cfg_path, "w"), indent=1)

        a_log = open(os.path.join(w, "a.log"), "w")
        aenv = dict(os.environ, STNODEFAULTFOLDER="1", STNOUPGRADE="1")
        pa = subprocess.Popen([a.syncthing, "serve", "--home", os.path.join(w, "a-home"), "--no-browser",
                               "--no-upgrade", f"--gui-address=127.0.0.1:{A_GUI}"],
                              stdout=a_log, stderr=subprocess.STDOUT, env=aenv, start_new_session=True)
        procs.append(pa)
        key = re.search(r"<apikey>(.*)</apikey>", open(os.path.join(w, "a-home", "config.xml")).read()).group(1)
        check("stock Syncthing's API is up", wait_for("A's API", lambda: http(key, "GET", "/rest/system/ping"), 30))
        http(key, "PATCH", "/rest/config/options",
             {"listenAddresses": [f"tcp://127.0.0.1:{A_PORT}"], "globalAnnounceEnabled": False,
              "localAnnounceEnabled": False, "relaysEnabled": False, "natEnabled": False})
        http(key, "POST", "/rest/config/devices",
             {"deviceID": bid, "name": "xst-b", "addresses": [f"tcp://127.0.0.1:{B_PORT}"]})
        http(key, "POST", "/rest/config/folders",
             {"id": "interop", "label": "Interop", "path": a_data, "rescanIntervalS": 3, "fsWatcherEnabled": True,
              "fsWatcherDelayS": 1, "devices": [{"deviceID": bid}]})

        b_log = open(os.path.join(w, "b.log"), "a")
        pb = subprocess.Popen([a.xsyncthing, "--home", os.path.join(w, "b-home"), "--name", "xst-b", "--no-api"],
                              stdout=b_log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        procs.append(pb)
        check("A and B connect", wait_for(
            "a connection", lambda: http(key, "GET", "/rest/system/connections")["connections"][bid]["connected"], 60))

        def A(*p):
            return os.path.join(a_data, *p)

        def B(*p):
            return os.path.join(b_data, *p)

        # 1. Creation, A -> B and B -> A.
        os.makedirs(A("docs", "deep"))
        open(A("hello.txt"), "w").write("hello from stock syncthing\n")
        rnd = random.Random(7)
        open(A("docs", "deep", "random.bin"), "wb").write(bytes(rnd.getrandbits(8) for _ in range(1_500_000)))
        open(A("empty.txt"), "w").close()
        check("new files and directories A -> B", wait_for("A's files on B", lambda: same(A("hello.txt"), B("hello.txt"))
              and same(A("docs", "deep", "random.bin"), B("docs", "deep", "random.bin"))
              and os.path.exists(B("empty.txt")), 90))
        os.makedirs(B("from-b"))
        open(B("from-b", "note.txt"), "w").write("written on xsyncthing\n")
        check("new files and directories B -> A",
              wait_for("B's files on A", lambda: same(B("from-b", "note.txt"), A("from-b", "note.txt")), 90))

        # 2. Modifications.
        time.sleep(2)
        open(A("hello.txt"), "a").write("a second line from A\n")
        check("modification A -> B", wait_for("A's edit on B", lambda: same(A("hello.txt"), B("hello.txt")), 90))
        time.sleep(2)
        open(B("from-b", "note.txt"), "a").write("edited on B\n")
        check("modification B -> A",
              wait_for("B's edit on A", lambda: same(B("from-b", "note.txt"), A("from-b", "note.txt")), 90))

        # 3. Deletions.
        os.remove(A("empty.txt"))
        check("deletion A -> B", wait_for("deletion on B", lambda: not os.path.exists(B("empty.txt")), 90))
        shutil.rmtree(B("from-b"))
        check("deletion B -> A", wait_for("deletion on A", lambda: not os.path.exists(A("from-b")), 90))

        # 4. A conflict: pause B's connection by stopping B, edit both, restart B.
        pb.send_signal(signal.SIGTERM)
        pb.wait(30)
        procs.remove(pb)
        open(A("hello.txt"), "w").write("A's concurrent version\n")
        time.sleep(4)  # let A rescan and record its version
        open(B("hello.txt"), "w").write("B's concurrent version, written later\n")
        future = time.time() + 60
        os.utime(B("hello.txt"), (future, future))  # make B's the newer one, so it wins
        b_log = open(os.path.join(w, "b.log"), "a")
        pb = subprocess.Popen([a.xsyncthing, "--home", os.path.join(w, "b-home"), "--name", "xst-b", "--no-api"],
                              stdout=b_log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
        procs.append(pb)

        def conflict_resolved():
            ca = [n for n in os.listdir(a_data) if ".sync-conflict-" in n]
            cb = [n for n in os.listdir(b_data) if ".sync-conflict-" in n]
            return same(A("hello.txt"), B("hello.txt")) and (ca or cb) and \
                open(A("hello.txt")).read().startswith("B's")
        check("concurrent edits: newer wins on both sides, a conflict copy is kept",
              wait_for("the conflict to settle", conflict_resolved, 120))

        # 5. Completion as stock Syncthing sees it.
        check("A reports B 100% complete", wait_for("completion", lambda: http(
            key, "GET", f"/rest/db/completion?folder=interop&device={bid}")["completion"] == 100, 60))
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except OSError:
                pass
        for p in procs:
            try:
                p.wait(20)
            except Exception:
                p.kill()
    failed = results.count(False)
    print(f"interop: {len(results) - failed} passed, {failed} failed (logs in {w})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
