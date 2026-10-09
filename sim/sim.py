#!/usr/bin/env python3
"""Terminal stand-in for the bracelets you have not built yet.

  python sim/sim.py                 three pretend bracelets, no hardware needed
  python sim/sim.py --list          show serial ports
  python sim/sim.py --port COM5     one real bracelet on USB + two pretend ones
  (macOS/Linux ports look like /dev/cu.usbmodem1101 or /dev/ttyACM0)

Type  <who> <action>  and press Enter, for example  "1 talk"  or  "2 a".
"""
import argparse
import os
import queue
import random
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from core import COMMANDS, Core  # noqa: E402

HELP = """
  who:     1, 2, ...  a pretend bracelet        r  the real bracelet (over USB)
  action:  talk     snap onto the talking queue
           topic    snap onto the new-topic queue
           pause    snap onto "I need a break" (remembered after you move on)
           off      unsnap (snap hanging loose)
           a        agree            d  disagree
           c        comment on the current topic (= double press)
           unpause  take back the break wish (= hold both buttons)
  other:   s  status of everyone     h  this help     q  quit
  example: "1 talk", "2 talk", "r a", "1 off"
"""

FEEL = {
    "AGREE": ("bzz", "someone agrees with you"),
    "DISAGREE": ("bzz bzz", "someone disagrees with you"),
    "COMMENT": ("bzz bzz bzz", "someone has a comment on this topic"),
    "YOUR_TURN": ("BZZZZZZZ", "your turn to speak"),
    "TOPIC_TURN": ("BZZZZ BZZZZ", "nobody is waiting: introduce your new topic (move snap to talk)"),
    "BREAK": ("bz-bz-bz-bz BZZZZZZZZZ", "more than half the group wants a break"),
    "ACK": ("tick", "break wish withdrawn"),
}


def now_ms():
    return int(time.monotonic() * 1000)


class LineTransport:
    """Reads lines from the real bracelet in a background thread."""

    def __init__(self, readline, write):
        self._write = write
        self.inbox = queue.Queue()
        self.alive = True

        def pump():
            while self.alive:
                try:
                    raw = readline()
                except Exception as exc:  # port unplugged etc.
                    self.inbox.put("# connection lost: %s" % exc)
                    break
                if raw:
                    self.inbox.put(raw.decode("utf-8", "replace").strip())

        threading.Thread(target=pump, daemon=True).start()

    def send(self, line):
        try:
            self._write((line + "\n").encode())
        except Exception:
            pass


def open_serial(port):
    import serial  # pip install pyserial

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.dtr = False  # do not reset the board when the port opens
    ser.rts = False
    ser.open()
    return LineTransport(ser.readline, lambda b: (ser.write(b), ser.flush()))


class Hub:
    """Runs the pretend bracelets and passes packets between them and the real one."""

    def __init__(self, ids, transport=None, emit=print, show_packets=False):
        self.transport = transport
        self.emit = emit
        self.show_packets = show_packets
        self.lock = threading.Lock()
        t = now_ms()
        self.cores = {i: Core(i, t, random.randrange(1, 60000)) for i in ids}
        self.running = True

    def step(self):
        with self.lock:
            now = now_ms()
            if self.transport:
                while not self.transport.inbox.empty():
                    line = self.transport.inbox.get()
                    if line.startswith("W26 "):
                        if self.show_packets:
                            self.emit("      real -> %s" % line)
                        for c in self.cores.values():
                            c.on_line(line, now)
                    elif line.startswith("# vibe "):
                        self._feel("real", line[7:].strip())
                    elif line:
                        self.emit("  [real] %s" % line.lstrip("# "))
            for c in self.cores.values():
                c.tick(now)
            self._route(now)

    def _route(self, now):
        moved = True
        while moved:  # replies can trigger more packets; go until quiet
            moved = False
            for c in self.cores.values():
                for line in c.pop_lines():
                    moved = True
                    if self.show_packets:
                        self.emit("      %s -> %s" % (c.id, line))
                    for other in self.cores.values():
                        if other is not c:
                            other.on_line(line, now)
                    if self.transport:
                        self.transport.send(line)
                for p in c.pop_patterns():
                    self._feel(str(c.id), p)

    def _feel(self, who, pattern):
        buzz, meaning = FEEL.get(pattern, (pattern, ""))
        self.emit("  >> bracelet %s feels: %-22s (%s)" % (who, buzz, meaning))

    def command(self, who, action):
        with self.lock:
            if who == "r":
                if not self.transport:
                    return "no real bracelet connected (start with --port)"
                self.transport.send("!" + action)
                return None
            try:
                core = self.cores[int(who)]
            except (ValueError, KeyError):
                return "unknown bracelet '%s' (have: %s)" % (who, ", ".join(map(str, self.cores)))
            if action not in COMMANDS:
                return "unknown action '%s' (h for help)" % action
            now = now_ms()
            COMMANDS[action](core, now)
            self._route(now)
            return None

    def status(self):
        with self.lock:
            for c in self.cores.values():
                self.emit("  " + c.status())
            if self.transport:
                self.transport.send("!status")

    def run_forever(self):
        while self.running:
            self.step()
            time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port", help="serial port of the real bracelet")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--pretend", type=int, help="number of pretend bracelets (default 2 with --port, else 3)")
    ap.add_argument("--packets", action="store_true", help="also show every radio packet")
    args = ap.parse_args()

    if args.list:
        from serial.tools import list_ports

        for p in list_ports.comports():
            print(p.device, "-", p.description)
        return

    transport = open_serial(args.port) if args.port else None
    count = args.pretend or (2 if transport else 3)
    hub = Hub(range(1, count + 1), transport, show_packets=args.packets)
    threading.Thread(target=hub.run_forever, daemon=True).start()

    print("Pretend bracelets: %s%s" % (", ".join(map(str, hub.cores)),
                                       "   + real bracelet 'r' on " + args.port if transport else ""))
    print(HELP)
    while True:
        try:
            text = input().strip().lower()
        except (EOFError, KeyboardInterrupt):
            break
        if not text:
            continue
        if text in ("q", "quit", "exit"):
            break
        if text in ("h", "help", "?"):
            print(HELP)
        elif text in ("s", "status"):
            hub.status()
        else:
            parts = text.split()
            err = hub.command(parts[0], parts[1]) if len(parts) == 2 else "type: <who> <action>, e.g. 1 talk"
            if err:
                print("  ! " + err)
    hub.running = False


if __name__ == "__main__":
    main()
