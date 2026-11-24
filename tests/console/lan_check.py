# Console <-> PC LAN sync check, run while xsyncthing.xex runs on the console. Prints PASS/FAIL lines.
import json, os, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools")); import xbdm
HOST = "192.168.1.118"; X = sys.argv[1]; D = "/tmp/xr/lan/data"; R = "Hdd:\\xsyncthing\\folders\\lan"
key = open("/tmp/xr/lan/key").read().strip()
def api(path):
    r = urllib.request.Request("http://127.0.0.1:18585" + path, headers={"X-API-Key": key})
    return json.loads(urllib.request.urlopen(r, timeout=10).read())
def wait(fn, t):
    end = time.time() + t
    while time.time() < end:
        try:
            if fn(): return True
        except Exception: pass
        time.sleep(2)
    return False
def console_get(name):
    x = xbdm.Xbdm(HOST, timeout=20)
    try: return x.get(R + "\\" + name)
    finally: x.close()
def console_ls(sub=""):
    x = xbdm.Xbdm(HOST, timeout=20)
    try: return " ".join(x.ls(R + ("\\" + sub if sub else "")))
    finally: x.close()
res = []
def check(n, ok): res.append(ok); print(("PASS" if ok else "FAIL") + ": " + n, flush=True)
t0 = time.time()
def fresh():
    c = api("/rest/system/connections")["connections"][X]
    return c["connected"] and c.get("startedAt", "") >= time.strftime("%Y-%m-%dT%H:%M", time.localtime(t0 - 60))
check("the console connects (a fresh connection)", wait(fresh, 120))
stamp = time.strftime("%H:%M:%S")
open(D + "/hello.txt", "w").write("PC edit at " + stamp + "\n")
check("an edit on the PC reaches the console", wait(lambda: console_get("hello.txt").decode().strip() == "PC edit at " + stamp, 90))
open("/tmp/xr/xb.txt", "w").write("console file " + stamp + "\n")
x = xbdm.Xbdm(HOST, timeout=20); x.put("/tmp/xr/xb.txt", R + "\\from-xbox-2.txt"); x.close()
check("a new file on the console reaches the PC", wait(lambda: open(D + "/from-xbox-2.txt").read() == "console file " + stamp + "\n", 90))
check("the FATX-long names are still on the PC", all(os.path.exists(os.path.join(D, n)) for n in os.listdir(D)) and
      any(len(n) > 42 for n in os.listdir(D)))
check("completion 100% as the PC sees it", wait(lambda: api(f"/rest/db/completion?folder=lan&device={X}")["completion"] == 100, 60))
st = api(f"/rest/db/status?folder=lan")
print(f"  PC folder: {st['globalFiles']} files, {st['globalBytes']} bytes, state {st['state']}")
# Clean stop: the PC must see the connection close (EOF / Close), not wait for its 5-minute timeout.
subprocess.run([sys.executable, "tools/xbdm.py", "--host", HOST, "stop", "Hdd:\\xsyncthing"], capture_output=True, timeout=200)
check("after a clean stop the PC sees the console disconnect within 30 s",
      wait(lambda: not api("/rest/system/connections")["connections"][X]["connected"], 30))
print(f"lan: {res.count(True)} passed, {res.count(False)} failed")
