#!/usr/bin/env python3
"""Compares xsyncthing's REST responses with stock Syncthing's, endpoint by endpoint, while the two share a folder:
every JSON key stock Syncthing returns (recursively, for objects; for the first element of lists) must be present
in ours too, with a value of the same JSON type. Prints what differs.

  tests/interop/api_compare.py --syncthing ST --xsyncthing XST
"""
import argparse, json, os, re, shutil, signal, subprocess, sys, time, urllib.request

ENDPOINTS = [
    "/rest/system/status", "/rest/system/version", "/rest/system/connections", "/rest/system/discovery",
    "/rest/system/error", "/rest/system/paths", "/rest/system/log", "/rest/system/loglevels", "/rest/system/ping",
    "/rest/config", "/rest/config/options", "/rest/config/gui", "/rest/config/defaults/folder",
    "/rest/config/defaults/device", "/rest/config/defaults/ignores", "/rest/config/insync",
    "/rest/config/folders", "/rest/config/devices", "/rest/config/folders/cmp",
    "/rest/stats/device", "/rest/stats/folder", "/rest/svc/lang", "/rest/svc/random/string?length=8",
    "/rest/cluster/pending/devices", "/rest/cluster/pending/folders",
    "/rest/db/status?folder=cmp", "/rest/db/completion?folder=cmp", "/rest/db/completion?folder=cmp&device={peer}",
    "/rest/db/need?folder=cmp", "/rest/db/remoteneed?folder=cmp&device={peer}", "/rest/db/localchanged?folder=cmp",
    "/rest/db/ignores?folder=cmp", "/rest/db/browse?folder=cmp", "/rest/db/file?folder=cmp&file=a.txt",
    "/rest/folder/errors?folder=cmp", "/rest/noauth/health", "/rest/events/disk?since=0&timeout=1",
    "/rest/events?since=0&timeout=1&limit=1",
]
# Keys upstream reports that xsyncthing deliberately leaves out (docs/COMPAT.md, "REST API"): files it has no
# equivalent of, and the log facility list (xsyncthing has one log).
ALLOWED_MISSING = {
    "/rest/system/paths": {"auditLog", "baseDir-userHome", "legacyDatabase", "panicLog", "logFile", "lockFile"},
    "/rest/system/loglevels": {"levels", "packages"},
}
# Maps keyed by an address or a server URL: their keys differ between any two devices by nature.
ADDRESS_KEYED = {"connectionServiceStatus", "lastDialStatus", "discoveryStatus", "discoveryErrors"}


def get(base, key, path):
    req = urllib.request.Request(base + path, headers={"X-API-Key": key})
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read() or b"null")


def kind(v):
    return "null" if v is None else type(v).__name__.replace("int", "number").replace("float", "number")


def diff(ref, ours, path=""):
    problems = []
    if isinstance(ref, dict):
        if not isinstance(ours, dict):
            return [f"{path or '/'}: object expected, got {kind(ours)}"]
        for k, v in ref.items():
            # Device IDs as keys differ between the two instances: compare the first entry to the first entry.
            if re.fullmatch(r"[A-Z2-7]{7}(-[A-Z2-7]{7}){7}", k) or path.split("/")[-1] in ADDRESS_KEYED:
                continue
            if k not in ours:
                problems.append(f"{path}/{k}: missing")
            else:
                problems += diff(v, ours[k], f"{path}/{k}")
    elif isinstance(ref, list):
        if not isinstance(ours, list):
            return [f"{path}: list expected, got {kind(ours)}"]
        if ref and ours:
            problems += diff(ref[0], ours[0], f"{path}[0]")
    elif ref is not None and ours is not None and kind(ref) != kind(ours):
        problems.append(f"{path}: {kind(ref)} expected, got {kind(ours)}")
    return problems


def start(cmd, log):
    return subprocess.Popen(cmd, stdout=open(log, "w"), stderr=subprocess.STDOUT, start_new_session=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--syncthing", required=True)
    ap.add_argument("--xsyncthing", required=True)
    ap.add_argument("--work", default="/tmp/xst-cmp")
    a = ap.parse_args()
    w = a.work
    shutil.rmtree(w, ignore_errors=True)
    for d in ("st-home", "st-data", "x-home", "x-data"):
        os.makedirs(os.path.join(w, d))
    for d in ("st-data", "x-data"):
        open(os.path.join(w, d, "a.txt"), "w").write("same content\n")
    procs = []
    try:
        subprocess.run([a.syncthing, "generate", "--home", f"{w}/st-home"], check=True, capture_output=True)
        sid = subprocess.run([a.syncthing, "device-id", "--home", f"{w}/st-home"], capture_output=True,
                             text=True).stdout.strip()
        gui = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "gui")
        p = start([a.xsyncthing, "--home", f"{w}/x-home", "--name", "x", "--gui", gui], f"{w}/x.log")
        time.sleep(2)
        p.send_signal(signal.SIGTERM)
        p.wait(20)
        xid = re.search(r"device ID ([A-Z0-9-]{63})", open(f"{w}/x.log").read()).group(1)
        c = json.load(open(f"{w}/x-home/config.json"))
        c["gui"]["address"] = "127.0.0.1:18786"
        c["options"].update(listenAddresses=["tcp://127.0.0.1:22502"], globalAnnounceEnabled=False,
                            localAnnounceEnabled=False, relaysEnabled=False)
        dev = dict(c["defaults"]["device"]); dev.update(deviceID=sid, name="st", addresses=["tcp://127.0.0.1:22501"])
        c["devices"].append(dev)
        f = dict(c["defaults"]["folder"]); f.update(id="cmp", label="Cmp", path=f"{w}/x-data", devices=[
            {"deviceID": sid, "introducedBy": "", "encryptionPassword": ""},
            {"deviceID": xid, "introducedBy": "", "encryptionPassword": ""}])
        c["folders"].append(f)
        json.dump(c, open(f"{w}/x-home/config.json", "w"))
        xkey = c["gui"]["apiKey"]
        procs.append(start([a.syncthing, "serve", "--home", f"{w}/st-home", "--no-browser", "--no-upgrade",
                            "--gui-address=127.0.0.1:18785"], f"{w}/st.log"))
        skey = re.search(r"<apikey>(.*)</apikey>", open(f"{w}/st-home/config.xml").read()).group(1)
        sbase, xbase = "http://127.0.0.1:18785", "http://127.0.0.1:18786"
        for _ in range(60):
            try:
                get(sbase, skey, "/rest/system/ping"); break
            except Exception:
                time.sleep(0.5)

        def post(path, body, method="POST"):
            r = urllib.request.Request(sbase + path, method=method, data=json.dumps(body).encode(),
                                       headers={"X-API-Key": skey, "Content-Type": "application/json"})
            urllib.request.urlopen(r).read()
        post("/rest/config/options", {"listenAddresses": ["tcp://127.0.0.1:22501"], "globalAnnounceEnabled": False,
                                      "localAnnounceEnabled": False, "relaysEnabled": False, "natEnabled": False}, "PATCH")
        post("/rest/config/devices", {"deviceID": xid, "name": "x", "addresses": ["tcp://127.0.0.1:22502"]})
        post("/rest/config/folders", {"id": "cmp", "label": "Cmp", "path": f"{w}/st-data", "devices": [{"deviceID": xid}]})
        procs.append(start([a.xsyncthing, "--home", f"{w}/x-home", "--name", "x", "--gui", gui], f"{w}/x.log"))
        for _ in range(120):
            try:
                if get(sbase, skey, "/rest/system/connections")["connections"][xid]["connected"]:
                    break
            except Exception:
                pass
            time.sleep(0.5)
        time.sleep(5)
        # The same config change on both, so both have a recent ConfigSaved event to compare.
        for base, key in ((sbase, skey), (xbase, xkey)):
            r = urllib.request.Request(base + "/rest/config/options", method="PATCH",
                                       data=json.dumps({"maxRecvKbps": 7}).encode(),
                                       headers={"X-API-Key": key, "Content-Type": "application/json"})
            urllib.request.urlopen(r).read()
        time.sleep(2)
        total = 0
        for ep in ENDPOINTS:
            ref = get(sbase, skey, ep.format(peer=xid))
            if ep.startswith("/rest/events") and isinstance(ref, list):
                # Compare the same kind of event: find ours of the type stock Syncthing returned first.
                ours_all = get(xbase, xkey, ep.split("?")[0] + "?since=0&timeout=1")
                same = [e for e in ours_all if ref and e.get("type") == ref[0].get("type")]
                probs = diff(ref[:1], same[:1]) if same else [f"no {ref[0]['type']} event"] if ref else []
                print(("ok   " if not probs else "FAIL ") + ep + ("" if not probs else f" ({len(probs)})"))
                for pr in probs[:12]:
                    print("       " + pr)
                total += len(probs)
                continue
            try:
                ours = get(xbase, xkey, ep.format(peer=sid))
            except Exception as e:
                print(f"FAIL {ep}: {e}")
                total += 1
                continue
            def allowed(pr):
                parts = pr.split("/")
                top = parts[1].split("[")[0].split(":")[0] if len(parts) > 1 else ""
                return top in ALLOWED_MISSING.get(ep.split("?")[0], set())
            probs = [pr for pr in diff(ref, ours) if not allowed(pr)]
            print(("ok   " if not probs else "FAIL ") + ep + ("" if not probs else f" ({len(probs)})"))
            for pr in probs[:12]:
                print("       " + pr)
            total += len(probs)
        print(f"compare: {total} differences")
        sys.exit(1 if total else 0)
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


if __name__ == "__main__":
    main()
