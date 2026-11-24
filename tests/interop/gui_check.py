#!/usr/bin/env python3
"""Loads the stock Syncthing Web GUI from xsyncthing in a headless browser (Playwright) and checks that it renders:
the device name in the navbar, the folder and device panels, no JavaScript errors, and no failed /rest calls.
Then adds a remote device through the GUI's own dialog. Saves a screenshot.

  tests/interop/gui_check.py --xsyncthing build-host/xsyncthing [--screenshot out.png]
"""
import argparse, json, os, re, shutil, signal, subprocess, sys, time, urllib.request

from playwright.sync_api import sync_playwright

PORT = 18687
results = []


def check(name, ok):
    results.append(bool(ok))
    print(("PASS" if ok else "FAIL") + ": " + name, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xsyncthing", required=True)
    ap.add_argument("--work", default="/tmp/xst-gui")
    ap.add_argument("--screenshot", default="/tmp/xst-gui.png")
    a = ap.parse_args()
    shutil.rmtree(a.work, ignore_errors=True)
    os.makedirs(os.path.join(a.work, "docs"))
    open(os.path.join(a.work, "docs", "hello.txt"), "w").write("hello\n")
    gui = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "gui")
    p = subprocess.Popen([a.xsyncthing, "--home", a.work, "--name", "xbox360", "--gui", gui],
                         stdout=open(a.work + ".log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    time.sleep(2)
    p.send_signal(signal.SIGTERM)
    p.wait(20)
    cfgp = os.path.join(a.work, "config.json")
    c = json.load(open(cfgp))
    my_id = re.search(r"device ID ([A-Z0-9-]{63})", open(a.work + ".log").read()).group(1)
    c["gui"]["address"] = f"127.0.0.1:{PORT}"
    c["options"].update(listenAddresses=["tcp://127.0.0.1:22420"], globalAnnounceEnabled=False,
                        localAnnounceEnabled=False, relaysEnabled=False, urAccepted=-1)
    f = dict(c["defaults"]["folder"])
    f.update(id="docs-0001", label="Documents", path=os.path.join(a.work, "docs"),
             devices=[{"deviceID": my_id, "introducedBy": "", "encryptionPassword": ""}])
    c["folders"].append(f)
    json.dump(c, open(cfgp, "w"))
    p = subprocess.Popen([a.xsyncthing, "--home", a.work, "--name", "xbox360", "--gui", gui],
                         stdout=open(a.work + ".log", "a"), stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/rest/noauth/health", timeout=1)
                break
            except Exception:
                time.sleep(0.2)
        with sync_playwright() as pw:
            browser = pw.chromium.launch()
            page = browser.new_page(viewport={"width": 1280, "height": 900})
            js_errors, failed = [], []
            page.on("pageerror", lambda e: js_errors.append(str(e)))
            page.on("console", lambda m: js_errors.append(m.text) if m.type == "error" else None)
            page.on("response", lambda r: failed.append(f"{r.status} {r.url}")
                    if "/rest/" in r.url and r.status >= 400 else None)
            page.goto(f"http://127.0.0.1:{PORT}/", wait_until="domcontentloaded")
            # The GUI keeps a /rest/events long-poll open, so it never goes network-idle: wait for its content.
            page.wait_for_selector("text=Documents", timeout=30000)
            page.wait_for_timeout(3000)
            body = page.inner_text("body")
            page.screenshot(path=a.screenshot.replace(".png", "-start.png"), full_page=True)
            check("the GUI shows this device's name (xbox360)", "xbox360" in body)
            check("the folder panel lists the folder", "Documents" in body)
            # Shared with no one yet, the folder's state is "Unshared" (a real state, not "Unknown").
            check("the folder shows a real state (Unshared)", "Unshared" in body and "Unknown" not in body)
            check("the This Device panel shows the version", "xsyncthing" in body or "v0.0.1" in body)
            check(f"no JavaScript errors ({len(js_errors)})", not js_errors)
            for e in js_errors[:10]:
                print("   js:", e[:200])
            check(f"no failed /rest calls ({len(failed)})", not failed)
            for f_ in failed[:10]:
                print("   http:", f_)

            # Add a remote device through the GUI's dialog.
            peer = "FTPHMOV-CBA6IRW-7B4E5IG-ALLMAXE-VOKUCGP-PT2POKE-GTWP4GE-SLMXGAI"
            page.click("text=Add Remote Device")
            page.wait_for_timeout(800)
            page.fill("#deviceID", peer)
            page.fill("#editDevice #name", "pc via gui")
            page.click("#editDevice button.btn-primary:has-text('Save')")
            page.wait_for_timeout(2500)
            cfg = json.load(open(cfgp))
            check("a device added in the GUI's dialog is saved to config.json",
                  any(d["deviceID"] == peer and d["name"] == "pc via gui" for d in cfg["devices"]))
            page.wait_for_timeout(1000)
            check("... and shows in the Remote Devices panel", "pc via gui" in page.inner_text("body"))
            page.screenshot(path=a.screenshot, full_page=True)
            browser.close()
    finally:
        os.killpg(p.pid, signal.SIGTERM)
        t = time.time()
        try:
            p.wait(20)
            print(f"  (daemon stopped in {time.time() - t:.1f} s)")
        except subprocess.TimeoutExpired:
            print("FAIL: the daemon did not stop within 20 s")
            results.append(False)
            p.kill()
    failed_n = results.count(False)
    print(f"gui: {len(results) - failed_n} passed, {failed_n} failed (screenshot {a.screenshot})")
    sys.exit(1 if failed_n else 0)


if __name__ == "__main__":
    main()
