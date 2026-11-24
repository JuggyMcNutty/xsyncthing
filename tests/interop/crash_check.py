#!/usr/bin/env python3
"""Recovery after a crash or power cut: stock Syncthing (A) shares a large folder; xsyncthing (B) is killed with
SIGKILL in the middle of pulling it, several times, then started again. Checks that each restart opens the index
database and config, removes the temp files the killed pull left, and that the sync finishes byte for byte.

  tests/interop/crash_check.py --syncthing ST --xsyncthing XST [--kills 3]
"""
import argparse, json, os, random, re, shutil, signal, subprocess, sys, time, urllib.request

A_GUI, A_PORT, B_PORT = 18585 + 400, 22801, 22802
results = []


def check(name, ok):
    results.append(bool(ok))
    print(("PASS" if ok else "FAIL") + ": " + name, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--syncthing", required=True)
    ap.add_argument("--xsyncthing", required=True)
    ap.add_argument("--work", default="/tmp/xst-crash")
    ap.add_argument("--kills", type=int, default=3)
    ap.add_argument("--files", type=int, default=3000)
    a = ap.parse_args()
    w = a.work
    shutil.rmtree(w, ignore_errors=True)
    for d in ("a-home", "b-home", "a-data", "b-data"):
        os.makedirs(os.path.join(w, d))
    rnd = random.Random(1)
    for i in range(a.files):
        d = os.path.join(w, "a-data", f"d{i // 100}")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, f"f{i}.bin"), "wb").write(rnd.randbytes(rnd.randint(1, 300_000)))
    procs = []

    def start_b():
        p = subprocess.Popen([a.xsyncthing, "--home", f"{w}/b-home", "--name", "xst-b", "--no-api"],
                             stdout=open(f"{w}/b.log", "a"), stderr=subprocess.STDOUT, start_new_session=True)
        procs.append(p)
        return p

    try:
        subprocess.run([a.syncthing, "generate", "--home", f"{w}/a-home"], check=True, capture_output=True)
        aid = subprocess.run([a.syncthing, "device-id", "--home", f"{w}/a-home"], capture_output=True, text=True).stdout.strip()
        p = start_b()
        time.sleep(2)
        p.send_signal(signal.SIGTERM)
        p.wait(20)
        procs.remove(p)
        bid = re.search(r"device ID ([A-Z0-9-]{63})", open(f"{w}/b.log").read()).group(1)
        c = json.load(open(f"{w}/b-home/config.json"))
        c["options"].update(listenAddresses=[f"tcp://127.0.0.1:{B_PORT}"], globalAnnounceEnabled=False,
                            localAnnounceEnabled=False, relaysEnabled=False)
        dev = dict(c["defaults"]["device"]); dev.update(deviceID=aid, name="a", addresses=[f"tcp://127.0.0.1:{A_PORT}"])
        c["devices"].append(dev)
        f = dict(c["defaults"]["folder"]); f.update(id="crash", label="Crash", path=f"{w}/b-data", rescanIntervalS=5,
            devices=[{"deviceID": aid, "introducedBy": "", "encryptionPassword": ""},
                     {"deviceID": bid, "introducedBy": "", "encryptionPassword": ""}])
        c["folders"].append(f)
        json.dump(c, open(f"{w}/b-home/config.json", "w"))
        pa = subprocess.Popen([a.syncthing, "serve", "--home", f"{w}/a-home", "--no-browser", "--no-upgrade",
                               f"--gui-address=127.0.0.1:{A_GUI}"], stdout=open(f"{w}/a.log", "w"),
                              stderr=subprocess.STDOUT, start_new_session=True, env=dict(os.environ, STNODEFAULTFOLDER="1"))
        procs.append(pa)
        key = re.search(r"<apikey>(.*)</apikey>", open(f"{w}/a-home/config.xml").read()).group(1)

        def api(m, path, body=None):
            r = urllib.request.Request(f"http://127.0.0.1:{A_GUI}{path}", method=m,
                                       data=None if body is None else json.dumps(body).encode(),
                                       headers={"X-API-Key": key, "Content-Type": "application/json"})
            with urllib.request.urlopen(r, timeout=10) as resp:
                d = resp.read()
                return json.loads(d) if d else None
        for _ in range(60):
            try:
                api("GET", "/rest/system/ping"); break
            except Exception:
                time.sleep(0.5)
        api("PATCH", "/rest/config/options", {"listenAddresses": [f"tcp://127.0.0.1:{A_PORT}"], "globalAnnounceEnabled": False,
                                               "localAnnounceEnabled": False, "relaysEnabled": False, "natEnabled": False})
        api("POST", "/rest/config/devices", {"deviceID": bid, "name": "b", "addresses": [f"tcp://127.0.0.1:{B_PORT}"]})
        api("POST", "/rest/config/folders", {"id": "crash", "label": "Crash", "path": f"{w}/a-data", "devices": [{"deviceID": bid}]})

        def b_files():
            n = 0
            for root, _, files in os.walk(f"{w}/b-data"):
                if "/.stfolder" in root:
                    continue
                n += sum(1 for x in files if not x.startswith((".syncthing.", ".st~")))
            return n

        def temps():
            return sum(1 for root, _, files in os.walk(f"{w}/b-data") for x in files if x.startswith((".syncthing.", ".st~")))

        for k in range(a.kills):
            p = start_b()
            # Wait until the pull is well under way, then pull the plug.
            target = (k + 1) * a.files // (a.kills + 2)
            t0 = time.time()
            deadline = t0 + 120
            while b_files() < target and time.time() < deadline and p.poll() is None:
                time.sleep(0.05)
            have = b_files()
            print(f"  (kill {k + 1} after {time.time() - t0:.1f} s; B {'running' if p.poll() is None else 'exited'})")
            p.send_signal(signal.SIGKILL)
            p.wait(10)
            procs.remove(p)
            check(f"kill {k + 1}: B killed mid-pull at {have}/{a.files} files ({temps()} temp files left behind)",
                  0 < have < a.files)

        p = start_b()
        done = False
        deadline = time.time() + 300
        while time.time() < deadline:
            try:
                if api("GET", f"/rest/db/completion?folder=crash&device={bid}")["completion"] == 100 and b_files() == a.files:
                    done = True
                    break
            except Exception:
                pass
            time.sleep(1)
        log = open(f"{w}/b.log").read()
        if not done:
            print("  completion:", api("GET", f"/rest/db/completion?folder=crash&device={bid}"), "B files:", b_files())
        check("after the last restart the sync completes (A reports 100%)", done)
        check("no temp files remain", temps() == 0)
        check("the restarts removed leftover temp files", "leftover temporary file" in log)
        bad = []
        for root, _, files in os.walk(f"{w}/a-data"):
            if "/.stfolder" in root:
                continue  # Syncthing's own marker, never synced
            for x in files:
                pa_ = os.path.join(root, x)
                pb_ = pa_.replace("a-data", "b-data")
                if not os.path.exists(pb_) or open(pa_, "rb").read() != open(pb_, "rb").read():
                    bad.append(pb_)
        check(f"every file is byte-identical ({a.files - len(bad)}/{a.files})", not bad)
        check("no database or config errors on restart",
              not re.search(r"corrupt|malformed|sqlite (prepare|step)|config\.json:", log))
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
    print(f"crash: {len(results) - failed} passed, {failed} failed (logs in {w})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
