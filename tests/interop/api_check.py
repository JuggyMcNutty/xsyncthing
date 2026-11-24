#!/usr/bin/env python3
"""Checks xsyncthing's REST API the way the stock Web GUI uses it: CSRF cookie from the page, the token header on
/rest calls, the API key, the GUI's startup calls, a config round trip, and /rest/events long-polling.

  tests/interop/api_check.py --xsyncthing build-host/xsyncthing
"""
import argparse, http.cookiejar, json, os, re, shutil, signal, subprocess, sys, threading, time, urllib.error, urllib.request

PORT = 18686
results = []


def check(name, ok):
    results.append(bool(ok))
    print(("PASS" if ok else "FAIL") + ": " + name, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xsyncthing", required=True)
    ap.add_argument("--work", default="/tmp/xst-api")
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(a.work)
    gui = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "gui")
    # First run: make the config, then point the GUI at our port.
    p = subprocess.Popen([a.xsyncthing, "--home", a.work, "--name", "api-test", "--gui", gui],
                         stdout=open(a.work + ".log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    time.sleep(2)
    p.send_signal(signal.SIGTERM)
    p.wait(20)
    cfgp = os.path.join(a.work, "config.json")
    c = json.load(open(cfgp))
    c["gui"]["address"] = f"127.0.0.1:{PORT}"
    c["options"].update(listenAddresses=["tcp://127.0.0.1:22410"], globalAnnounceEnabled=False,
                        localAnnounceEnabled=False, relaysEnabled=False)
    json.dump(c, open(cfgp, "w"))
    key = c["gui"]["apiKey"]
    my_id = re.search(r"device ID ([A-Z0-9-]{63})", open(a.work + ".log").read()).group(1)
    p = subprocess.Popen([a.xsyncthing, "--home", a.work, "--name", "api-test", "--gui", gui],
                         stdout=open(a.work + ".log", "a"), stderr=subprocess.STDOUT, start_new_session=True)
    base = f"http://127.0.0.1:{PORT}"
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(base + "/rest/noauth/health", timeout=1)
                break
            except Exception:
                time.sleep(0.2)
        jar = http.cookiejar.CookieJar()
        browser = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))

        # 1. The page: index.html and assets, and a CSRF cookie.
        r = browser.open(base + "/")
        html = r.read().decode()
        check("GET / serves the stock GUI's index.html", r.status == 200 and "ng-app=\"syncthing\"" in html)
        short = my_id[:7]
        csrf = next((ck.value for ck in jar if ck.name == f"CSRF-Token-{short}"), None)
        check("the page sets the CSRF cookie", csrf is not None)
        check("/meta.js names our device", my_id in browser.open(base + "/meta.js").read().decode())
        css = browser.open(base + "/theme-assets/default/assets/css/theme.css")
        check("theme assets resolve (default theme)", css.status == 200)
        check("vendor JS is served", browser.open(base + "/vendor/angular/angular.js").status == 200)

        # 2. /rest without a token is refused; with the token header it works.
        try:
            browser.open(base + "/rest/system/status")
            check("/rest without the CSRF header is refused", False)
        except urllib.error.HTTPError as e:
            check("/rest without the CSRF header is refused (403)", e.code == 403)

        def rest(path, method="GET", body=None, headers=None):
            h = {f"X-CSRF-Token-{short}": csrf, "Content-Type": "application/json"}
            h.update(headers or {})
            req = urllib.request.Request(base + path, method=method, headers=h,
                                         data=None if body is None else json.dumps(body).encode())
            with browser.open(req, timeout=70) as resp:
                data = resp.read()
                return resp.status, (json.loads(data) if data and data[:1] in b"[{" else data)

        # 3. What the GUI fetches at startup.
        for path, keys in [("/rest/system/status", ["myID", "startTime", "connectionServiceStatus", "discoveryStatus"]),
                           ("/rest/system/version", ["version", "longVersion", "os"]),
                           ("/rest/config", ["version", "folders", "devices", "gui", "options", "defaults"]),
                           ("/rest/system/connections", ["connections", "total"]),
                           ("/rest/system/discovery", []), ("/rest/system/error", ["errors"]),
                           ("/rest/stats/device", []), ("/rest/stats/folder", []),
                           ("/rest/cluster/pending/devices", []), ("/rest/cluster/pending/folders", []),
                           ("/rest/config/insync", ["configInSync"]), ("/rest/svc/report", ["version"]),
                           ("/rest/system/upgrade", ["running"]), ("/rest/system/paths", ["config"]),
                           ("/rest/system/log", ["messages"]), ("/rest/svc/lang", []),
                           ("/rest/config/defaults/folder", ["path", "type"]),
                           ("/rest/config/defaults/device", ["addresses", "compression"]),
                           ("/rest/config/defaults/ignores", ["lines"]),
                           ("/rest/svc/random/string?length=32", ["random"]),
                           (f"/rest/svc/deviceid?id={my_id}", ["id"])]:
            st, j = rest(path)
            check(f"GET {path}", st == 200 and all(k in j for k in keys))

        # 4. Add a device and a folder the way the GUI does, and read them back.
        peer = "FTPHMOV-CBA6IRW-7B4E5IG-ALLMAXE-VOKUCGP-PT2POKE-GTWP4GE-SLMXGAI"
        st, cfg = rest("/rest/config")
        dev = dict(cfg["defaults"]["device"]); dev.update(deviceID=peer, name="peer")
        cfg["devices"].append(dev)
        fol = dict(cfg["defaults"]["folder"])
        fol.update(id="abcd-1234", label="Docs", path=os.path.join(a.work, "docs"),
                   devices=[{"deviceID": my_id}, {"deviceID": peer}])
        cfg["folders"].append(fol)
        st, _ = rest("/rest/config", "PUT", cfg)
        check("PUT /rest/config (add a device and a folder)", st == 200)
        time.sleep(1.5)
        st, cfg2 = rest("/rest/config")
        check("... they're in the config", any(d["deviceID"] == peer for d in cfg2["devices"]) and
              any(f["id"] == "abcd-1234" for f in cfg2["folders"]))
        st, s = rest("/rest/db/status?folder=abcd-1234")
        check("GET /rest/db/status for the new folder", st == 200 and "state" in s and "globalFiles" in s)
        st, comp = rest(f"/rest/db/completion?folder=abcd-1234&device={peer}")
        check("GET /rest/db/completion", st == 200 and "completion" in comp)
        st, ig = rest("/rest/db/ignores?folder=abcd-1234", "POST", {"ignore": ["*.tmp", "// note"]})
        check("POST /rest/db/ignores", st == 200 and ig["ignore"] == ["*.tmp", "// note"])
        st, _ = rest("/rest/db/scan?folder=abcd-1234", "POST")
        check("POST /rest/db/scan", st == 200)
        st, _ = rest("/rest/config/options", "PATCH", {"maxSendKbps": 123})
        check("PATCH /rest/config/options", st == 200 and rest("/rest/config/options")[1]["maxSendKbps"] == 123)
        st, _ = rest(f"/rest/config/devices/{peer}", "DELETE")
        check("DELETE /rest/config/devices/<id> (also unshares)", st == 200 and
              all(d["deviceID"] != peer for f in rest("/rest/config")[1]["folders"] for d in f["devices"]))

        # 5. Events: the poll returns what happened, and a long-poll waits for the next one.
        st, evs = rest("/rest/events?since=0&timeout=1")
        check("GET /rest/events returns events", st == 200 and isinstance(evs, list) and len(evs) > 0)
        last = evs[-1]["id"]
        got = {}

        def poll():
            got["r"] = rest(f"/rest/events?since={last}&timeout=30")
        t = threading.Thread(target=poll)
        t0 = time.time()
        t.start()
        time.sleep(1)
        rest("/rest/db/scan?folder=abcd-1234", "POST")
        t.join(40)
        check(f"a long-poll wakes on a new event ({time.time() - t0:.1f} s)",
              "r" in got and len(got["r"][1]) > 0 and time.time() - t0 < 20)

        # 6. The API key works without CSRF; a wrong key doesn't.
        req = urllib.request.Request(base + "/rest/system/ping", headers={"X-API-Key": key})
        check("X-API-Key works without a CSRF token", json.loads(urllib.request.urlopen(req).read())["ping"] == "pong")
        try:
            urllib.request.urlopen(urllib.request.Request(base + "/rest/system/ping", headers={"X-API-Key": "nope"}))
            check("a wrong API key is refused", False)
        except urllib.error.HTTPError as e:
            check("a wrong API key is refused", e.code == 403)

        # 7. GUI user and password: the password is stored hashed, and then the API asks for a login.
        rest("/rest/config/gui", "PATCH", {"user": "admin", "password": "secret pass"})
        time.sleep(1)
        g = json.load(open(cfgp))["gui"]
        check("the GUI password is stored as bcrypt", g["password"].startswith("$2a$10$"))
        fresh = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        try:
            fresh.open(base + "/rest/system/status")
            check("with a password set, /rest needs a login", False)
        except urllib.error.HTTPError as e:
            check("with a password set, /rest needs a login (403)", e.code == 403)
        jar2 = http.cookiejar.CookieJar()
        b2 = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar2))
        b2.open(base + "/")
        req = urllib.request.Request(base + "/rest/noauth/auth/password", method="POST",
                                     data=json.dumps({"username": "admin", "password": "secret pass"}).encode(),
                                     headers={"Content-Type": "application/json"})
        st = b2.open(req).status
        check("POST /rest/noauth/auth/password logs in (204, session cookie)",
              st == 204 and any(c.name.startswith("sessionid-") for c in jar2))
    finally:
        os.killpg(p.pid, signal.SIGTERM)
        p.wait(20)
    failed = results.count(False)
    print(f"api: {len(results) - failed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
