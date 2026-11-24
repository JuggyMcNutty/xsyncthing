# Sets up a stock Syncthing on this PC (192.168.1.125:22301, GUI 127.0.0.1:18585) sharing folder "lan" with the console.
import json, os, re, shutil, subprocess, time, urllib.request, sys
ST = "/tmp/xr/st-dl/syncthing-linux-amd64-v2.1.7/syncthing"
W = "/tmp/xr/lan"
XBOX = sys.argv[1]
shutil.rmtree(W, ignore_errors=True); os.makedirs(W + "/home"); os.makedirs(W + "/data")
subprocess.run([ST, "generate", "--home", W + "/home"], check=True, capture_output=True)
pid = subprocess.run([ST, "device-id", "--home", W + "/home"], capture_output=True, text=True).stdout.strip()
p = subprocess.Popen([ST, "serve", "--home", W + "/home", "--no-browser", "--no-upgrade", "--gui-address=127.0.0.1:18585"],
                     stdout=open(W + "/st.log", "w"), stderr=subprocess.STDOUT, start_new_session=True,
                     env=dict(os.environ, STNODEFAULTFOLDER="1"))
open(W + "/pid", "w").write(str(p.pid))
key = re.search(r"<apikey>(.*)</apikey>", open(W + "/home/config.xml").read()).group(1)
open(W + "/key", "w").write(key)
def api(m, path, body=None):
    r = urllib.request.Request("http://127.0.0.1:18585" + path, method=m, data=None if body is None else json.dumps(body).encode(),
                               headers={"X-API-Key": key, "Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=10) as f:
        return f.read()
for _ in range(60):
    try: api("GET", "/rest/system/ping"); break
    except Exception: time.sleep(0.5)
api("PATCH", "/rest/config/options", {"listenAddresses": ["tcp://0.0.0.0:22301"], "globalAnnounceEnabled": False,
     "localAnnounceEnabled": False, "relaysEnabled": False, "natEnabled": False})
api("POST", "/rest/config/devices", {"deviceID": XBOX, "name": "xbox360", "addresses": ["tcp://192.168.1.118:22000"]})
api("POST", "/rest/config/folders", {"id": "lan", "label": "LAN test", "path": W + "/data", "rescanIntervalS": 10,
     "fsWatcherEnabled": True, "fsWatcherDelayS": 1, "devices": [{"deviceID": XBOX}]})
print(pid)
