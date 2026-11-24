#!/usr/bin/env python3
"""A minimal XBDM (Xbox debug monitor, TCP 730) client for running xsyncthing's titles on a JTAG/RGH console.

  xbdm.py [--host IP] info
  xbdm.py [--host IP] ls  'Hdd:\\xsyncthing'
  xbdm.py [--host IP] put local.xex 'Hdd:\\xsyncthing\\xsyncthing.xex'
  xbdm.py [--host IP] get 'Hdd:\\xsyncthing\\xsyncthing.log' local.log
  xbdm.py [--host IP] mkdir 'Hdd:\\xsyncthing'
  xbdm.py [--host IP] rm 'Hdd:\\xsyncthing\\old.log'
  xbdm.py [--host IP] launch 'Hdd:\\xsyncthing\\xsyncthing.xex'
  xbdm.py [--host IP] run local.xex 'Hdd:\\xsyncthing' [--wait-log NAME --timeout S --done REGEX] [--no-autoexit]
  xbdm.py [--host IP] stop 'Hdd:\\xsyncthing'        # asks our running title to stop (stop.txt), waits for the dashboard

The console's address defaults to $XBDM_HOST. Commands follow XBDM's text protocol: "200-" ok, "202-" multiline
until ".", "203-" binary follows, "204-" send binary. Only the standard commands are used.
"""
import argparse
import os
import re
import socket
import struct
import sys
import time


class XbdmError(Exception):
    pass


class Xbdm:
    def __init__(self, host, port=730, timeout=15):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.f = self.sock.makefile("rb")
        line = self._line()
        if not line.startswith("201"):
            raise XbdmError(f"unexpected greeting: {line}")

    def close(self):
        try:
            self.sock.sendall(b"bye\r\n")
        except OSError:
            pass
        self.sock.close()

    def _line(self):
        line = self.f.readline()
        if not line:
            raise XbdmError("connection closed")
        return line.decode("latin-1").rstrip("\r\n")

    def command(self, cmd):
        self.sock.sendall(cmd.encode("latin-1") + b"\r\n")
        line = self._line()
        if line[:1] != "2":
            raise XbdmError(f"{cmd!r}: {line}")
        return line

    def multiline(self, cmd):
        line = self.command(cmd)
        if not line.startswith("202"):
            return [line]
        lines = []
        while True:
            l = self._line()
            if l == ".":
                return lines
            lines.append(l)

    def ls(self, path):
        return self.multiline(f'dirlist name="{path}"')

    def mkdir(self, path):
        try:
            self.command(f'mkdir name="{path}"')
        except XbdmError as e:
            if "exists" not in str(e).lower() and "410" not in str(e):
                raise

    def rm(self, path):
        self.command(f'delete name="{path}"')

    def put(self, local, remote):
        data = open(local, "rb").read()
        if not data:
            # XBDM creates an empty file straight away, answering 200 rather than 204.
            line = self.command(f'sendfile name="{remote}" length=0x0')
            return 0
        line = self.command(f'sendfile name="{remote}" length=0x{len(data):x}')
        if not line.startswith("204"):
            raise XbdmError(f"sendfile: {line}")
        self.sock.sendall(data)
        line = self._line()
        if not line.startswith("200"):
            raise XbdmError(f"sendfile: {line}")
        return len(data)

    def get(self, remote, local=None):
        line = self.command(f'getfile name="{remote}"')
        if not line.startswith("203"):
            raise XbdmError(f"getfile: {line}")
        size = struct.unpack("<I", self._read(4))[0]
        data = self._read(size)
        if local:
            open(local, "wb").write(data)
        return data

    def _read(self, n):
        out = bytearray()
        while len(out) < n:
            chunk = self.f.read(n - len(out))
            if not chunk:
                raise XbdmError("connection closed during a transfer")
            out += chunk
        return bytes(out)

    def launch(self, path):
        directory = path.rsplit("\\", 1)[0]
        # magicboot starts the title as the dashboard would.
        self.sock.sendall(f'magicboot title="{path}" directory="{directory}"\r\n'.encode("latin-1"))
        try:
            line = self._line()
            if line[:1] != "2":
                raise XbdmError(f"magicboot: {line}")
        except (OSError, TimeoutError):
            pass  # the console went down to boot the title before answering; that's normal

    def running(self):
        """The path of the running title (xbeinfo running)."""
        for line in self.multiline("xbeinfo running"):
            m = re.match(r'name="(.*)"', line)
            if m:
                return m.group(1)
        return ""

    def reboot_to_dashboard(self):
        self.sock.sendall(b"magicboot COLD\r\n")
        try:
            self._line()
        except (OSError, TimeoutError, XbdmError):
            pass


def connect_when_up(host, timeout=120):
    """Waits for XBDM to answer (after a reboot or a title change) and returns a connection."""
    deadline = time.time() + timeout
    while True:
        try:
            return Xbdm(host, timeout=10)
        except (OSError, XbdmError):
            if time.time() > deadline:
                raise
            time.sleep(2)


def is_dashboard(path):
    p = path.lower()
    return p.endswith("dash.xex") or "aurora" in p or "\\fsd" in p


def wait_for_dashboard(host, timeout=180, settle=8):
    """Waits until a dashboard has been the running title for `settle` seconds: the console has finished booting.
    Launching while it boots (lhelper.xex and the like) can hang it."""
    deadline = time.time() + timeout
    since = None
    while time.time() < deadline:
        try:
            x = Xbdm(host, timeout=10)
            now = x.running()
            x.close()
        except (OSError, XbdmError):
            now, since = "", None
        if is_dashboard(now):
            since = since or time.time()
            if time.time() - since >= settle:
                return now
        else:
            since = None
        time.sleep(2)
    raise XbdmError(f"the console didn't settle on a dashboard within {timeout:.0f}s")


def wait_for_log(host, remote_log, timeout, done_regex):
    """Polls the title's log until done_regex matches a line or timeout seconds pass; returns the log text."""
    deadline = time.time() + timeout
    text = ""
    while time.time() < deadline:
        time.sleep(3)
        try:
            x = Xbdm(host, timeout=10)
            try:
                text = x.get(remote_log).decode("utf-8", "replace")
            finally:
                x.close()
        except (OSError, XbdmError):
            continue  # the console is rebooting into the title, or the log isn't there yet
        if re.search(done_regex, text, re.M):
            return text, True
    return text, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("XBDM_HOST"))
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--wait-log")
    ap.add_argument("--extra", action="append", help="another file to copy beside the .xex (run)")
    ap.add_argument("--no-autoexit", action="store_true", help="leave the title running (run); stop it with 'stop'")
    ap.add_argument("--no-wait", action="store_true", help="return once the title is running (run)")
    ap.add_argument("--timeout", type=float, default=300)
    ap.add_argument("--done", default=r"^.*checks: \d+ passed, \d+ failed|^.*exit: status")
    a = ap.parse_args()
    if not a.host:
        sys.exit("no console address: pass --host or set XBDM_HOST")

    if a.cmd == "run":
        local, remote_dir = a.args
        name = os.path.basename(local)
        x = connect_when_up(a.host)
        now = x.running()
        x.close()
        # A title holds its .xex and log open. Ask one of ours to stop (stop.txt beside it); never reboot the console
        # under a running title, which hung it once (results/2026-11-24-console/README.md).
        if not is_dashboard(now) and now and "\\flash\\" not in now.lower():
            title_dir = "Hdd:\\" + now.split("Partition1\\", 1)[-1].rsplit("\\", 1)[0] if "Partition1" in now else None
            if not title_dir:
                sys.exit(f"{now} is running; end it from the console first")
            print(f"asking {now} to stop")
            stamp = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".stop.txt")
            open(stamp, "w").write("1\n")
            x = connect_when_up(a.host)
            x.put(stamp, f"{title_dir}\\stop.txt")
            x.close()
            os.remove(stamp)
        print(f"dashboard: {wait_for_dashboard(a.host)}")
        x = connect_when_up(a.host)
        try:
            x.mkdir(remote_dir)
            log_name = a.wait_log or (os.path.splitext(name)[0] + ".log")
            try:
                x.rm(f"{remote_dir}\\{log_name}")
            except XbdmError:
                pass
            # Tell the title to end by itself once done, so its log closes and can be fetched.
            try:
                x.rm(f"{remote_dir}\\autoexit.txt")
            except XbdmError:
                pass
            if not a.no_autoexit:
                stamp = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".autoexit.txt")
                open(stamp, "w").write("1\n")
                x.put(stamp, f"{remote_dir}\\autoexit.txt")
                os.remove(stamp)
            for extra in a.extra or []:
                x.put(extra, f"{remote_dir}\\{os.path.basename(extra)}")
            n = x.put(local, f"{remote_dir}\\{name}")
            print(f"sent {n} bytes to {remote_dir}\\{name}")
            x.launch(f"{remote_dir}\\{name}")
            print("launched")
            # magicboot reboots into the title; confirm it's the one running.
            x.close()
            time.sleep(5)
            deadline = time.time() + 90
            target = name.lower()
            while time.time() < deadline:
                try:
                    x = connect_when_up(a.host, timeout=30)
                    now = x.running()
                    has_log = any(f'name="{log_name}"'.lower() in l.lower() for l in x.ls(remote_dir))
                    x.close()
                    if now.lower().endswith(target):
                        print(f"running {now}")
                        break
                    if has_log:
                        print("the title ran (its log is there)")
                        break
                except (OSError, XbdmError):
                    pass
                time.sleep(2)
            else:
                sys.exit("the title didn't start")
        finally:
            try:
                x.close()
            except (OSError, ValueError):
                pass
        if a.no_wait:
            return
        text, done = wait_for_log(a.host, f"{remote_dir}\\{log_name}", a.timeout, a.done)
        sys.stdout.write(text)
        if not done:
            sys.exit(f"\n(timed out after {a.timeout:.0f}s waiting for the log to finish)")
        sys.exit(1 if re.search(r"^.*FAIL", text, re.M) else 0)

    x = Xbdm(a.host)
    try:
        if a.cmd == "info":
            print(x.command("dbgname"))
            for l in x.multiline("systeminfo"):
                print(l)
        elif a.cmd == "ls":
            for l in x.ls(a.args[0]):
                print(l)
        elif a.cmd == "mkdir":
            x.mkdir(a.args[0])
        elif a.cmd == "rm":
            x.rm(a.args[0])
        elif a.cmd == "put":
            print(x.put(a.args[0], a.args[1]), "bytes")
        elif a.cmd == "get":
            data = x.get(a.args[0], a.args[1] if len(a.args) > 1 else None)
            if len(a.args) < 2:
                sys.stdout.write(data.decode("utf-8", "replace"))
        elif a.cmd == "stop":
            # Asks one of our titles (in the folder given) to stop, then waits for the dashboard.
            stamp = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".stop.txt")
            open(stamp, "w").write("1\n")
            x.put(stamp, f"{a.args[0]}\\stop.txt")
            os.remove(stamp)
            x.close()
            print(f"dashboard: {wait_for_dashboard(a.host, timeout=120)}")
            return
        elif a.cmd == "launch":
            x.launch(a.args[0])
        else:
            sys.exit(f"unknown command {a.cmd}")
    finally:
        x.close()


if __name__ == "__main__":
    main()
