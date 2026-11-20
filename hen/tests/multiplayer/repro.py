#!/usr/bin/env python3
"""The repeat-session test: two instances of the desktop test build in one room play
Mario Kart 7's local multiplayer through SESSIONS sessions (create, join, start, disband), and
each time the joining side must list the host's game. repro.py [SESSIONS] [swap]
swap: the two sides change roles every session. Exit status 0: every session was listed."""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctl
ROOT = ctl.ROOT
TOOLS = os.path.dirname(os.path.abspath(__file__))
sessions = int(sys.argv[1]) if len(sys.argv) > 1 else 3
swap = "swap" in sys.argv

def log(inst):
    return f"{ROOT}/{inst}/Library/Application Support/Azahar/log/azahar_log.txt"
def count(inst, text):
    try:
        return open(log(inst), "rb").read().count(text.encode())
    except FileNotFoundError:
        return 0
def press(inst, *keys):
    for k in keys:
        ctl.send(inst, f"press {k} 120")
def settle(ms, inst="A"):
    ctl.wait(inst, ms)

subprocess.run(["sh", f"{TOOLS}/stop.sh"])
for inst in "AB":
    for f in ("cmd", "status"):
        try: os.remove(f"{ROOT}/{inst}/ctl/{f}")
        except FileNotFoundError: pass
    subprocess.run(["sh", f"{TOOLS}/start.sh", inst])
for inst in "AB":
    for _ in range(600):
        if int(ctl.status(inst).get("ms", 0)) > 15000: break
        time.sleep(0.1)
    else:
        print(f"instance {inst} did not start: see {ROOT}/{inst}/run.log"); sys.exit(2)
PORT = os.environ.get("KAIZHAR_TEST_PORT", "24999")
ctl.send("A", f"hostroom {PORT}"); settle(1500)
ctl.send("A", f"room PlayerA 127.0.0.1 {PORT}"); ctl.send("B", f"room PlayerB 127.0.0.1 {PORT}")
settle(2500)
def until(inst, text, before, seconds):
    """Waits for a new line with text in the instance's log. True if it came."""
    t0 = time.time()
    while time.time() - t0 < seconds:
        if count(inst, text) > before:
            return True
        time.sleep(0.2)
    return False

def enter_local(inst):
    """From the main menu into Local Multiplayer (the room list): the client starts scanning."""
    for attempt in range(4):
        before = count(inst, "Starting scan worker")
        press(inst, "DOWN", "A")
        if until(inst, "Starting scan worker", before, 12):
            return True
        press(inst, "B")          # (a dialog or another menu in the way: back, and again)
        settle(3000, inst)
    return False

ok = True
for n in range(1, sessions + 1):
    host, guest = ("B", "A") if swap and n % 2 == 0 else ("A", "B")
    if not (enter_local(host) and enter_local(guest)):
        print(f"session {n}: the script could not reach Local Multiplayer (see {ROOT}/shots)"); ctl.pair(f"s{n}-stuck"); ok = False; break
    settle(3000)
    hosted = count(host, "StartHosting:")
    press(host, "DOWN", "DOWN", "DOWN", "DOWN", "A")  # Create a Room
    if not until(host, "StartHosting:", hosted, 15):
        print(f"session {n}: the script could not create a room"); ctl.pair(f"s{n}-stuck"); ok = False; break
    deleted = count(guest, "DeleteScanInfo:")
    sessions_before = count(guest, "StartSession:")
    got = count(guest, "Got title info")
    listed = until(guest, "Got title info", got, 25)
    settle(4000, guest)
    ctl.pair(f"s{n}-list")
    press(guest, "A")                            # join the first room listed
    joined = until(guest, "StartSession:", sessions_before, 12)
    discarded = count(guest, "DeleteScanInfo:") - deleted
    print(f"session {n}: host {host}, guest {guest}: {'listed and joined' if joined else 'NOT LISTED' if not listed or discarded else 'listed, NOT JOINED'}"
          f" (the game discarded the host {discarded} times)", flush=True)
    if not joined:
        ok = False
        break
    settle(7000, host)
    passes = count(guest, "GetWirelessRebootPassphrase:")
    press(host, "A")                             # Start
    started = until(guest, "GetWirelessRebootPassphrase:", passes, 30)
    settle(14000, host)
    ctl.pair(f"s{n}-started")
    if not started:
        print(f"session {n}: the game did not start"); ok = False; break
    press(host, "B"); settle(4000, host); press(host, "A")   # back: Disband the group? Yes
    settle(30000, host)
    ctl.pair(f"s{n}-after")
print("PASS" if ok else "FAIL")
if "keep" not in sys.argv:
    subprocess.run(["sh", f"{TOOLS}/stop.sh"])
sys.exit(0 if ok else 1)
