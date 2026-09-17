#!/usr/bin/env python3
"""Console driver for the Nexus 3172TQ terminal server.

Built rather than reused because con.py cannot do the one thing this needs:
the loader autoboots about two seconds after its prompt appears, so the prompt
has to be caught by a program that is already connected and already spamming
Ctrl-L -- not by a human, and not by a second connection.

Two other traps this handles, both recorded in the RE notes:

  * The console server REPLAYS A BACKLOG on connect. A stale "loader>" in that
    backlog matches instantly and fires the command into a running NX-OS. So
    matching only starts after --after has been seen on this connection.
  * Telnet option negotiation has to be answered or the server waits on us.

Usage:
  nxcon.py listen [--secs N]
  nxcon.py expect --after MARK --want PAT [--spam HEX] [--send LINE ...]
"""
import argparse, socket, sys, time, re

IAC, DONT, WONT, WILL, DO, SB, SE = 255, 254, 252, 251, 253, 250, 240


class Console:
    def __init__(self, host, port, log):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.setblocking(False)
        self.out = bytearray()
        self.log = open(log, "ab") if log else None

    def _feed(self, d):
        i = 0
        while i < len(d):
            c = d[i]
            if c == IAC and i + 1 < len(d):
                cmd = d[i + 1]
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(d):
                    opt = d[i + 2]
                    reply = WONT if cmd == DO else (DONT if cmd == WILL else None)
                    if reply:
                        try:
                            self.s.sendall(bytes([IAC, reply, opt]))
                        except Exception:
                            pass
                    i += 3
                    continue
                if cmd == SB:
                    j = d.find(bytes([IAC, SE]), i)
                    i = (j + 2) if j != -1 else len(d)
                    continue
                if cmd == IAC:
                    self.out.append(IAC)
                    i += 2
                    continue
                i += 2
                continue
            self.out.append(c)
            i += 1

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                d = self.s.recv(4096)
                if not d:
                    return
                self._feed(d)
                if self.log:
                    self.log.write(d)
                    self.log.flush()
            except BlockingIOError:
                time.sleep(0.02)
            except (TimeoutError, socket.timeout):
                time.sleep(0.02)

    def text(self):
        return self.out.decode("utf-8", "replace")

    def send(self, line, chardelay=0.006):
        for ch in line.encode():
            self.s.sendall(bytes([ch]))
            time.sleep(chardelay)
        self.s.sendall(b"\r")

    def raw(self, b):
        self.s.sendall(b)

    def wait_for(self, pattern, timeout, spam=None, spam_every=0.05, start=0):
        """Read until pattern appears at or after offset `start`."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        last = 0.0
        while time.time() < end:
            if spam and time.time() - last >= spam_every:
                try:
                    self.raw(spam)
                except Exception:
                    pass
                last = time.time()
            self.pump(0.05)
            m = rx.search(self.text(), start)
            if m:
                return m.end()
        return -1


ap = argparse.ArgumentParser()
ap.add_argument("mode", choices=["listen", "expect"])
ap.add_argument("-H", default="10.10.25.2")
ap.add_argument("-p", type=int, default=2030)
ap.add_argument("--log")
ap.add_argument("--secs", type=float, default=20)
ap.add_argument("--from-now", action="store_true",
                help="ignore the backlog the terminal server replays on connect, and "
                     "match only what arrives after this connection settles. Safer than "
                     "--after, which needs a marker that is certain to appear.")
ap.add_argument("--after", help="only match --want after this pattern is seen")
ap.add_argument("--after-timeout", type=float, default=180)
ap.add_argument("--want")
ap.add_argument("--want-timeout", type=float, default=180)
ap.add_argument("--spam", help="hex byte to send repeatedly while waiting for --want")
ap.add_argument("--send-before", action="append", default=[],
                help="line to send BEFORE waiting for --want, so one connection can "
                     "trigger a reboot and then catch the loader prompt it produces. "
                     "Two connections cannot: the window is about two seconds.")
ap.add_argument("--send", action="append", default=[], help="line to send once --want matches")
ap.add_argument("--tail", type=float, default=15, help="keep reading after sending")
a = ap.parse_args()

c = Console(a.H, a.p, a.log)
c.pump(1.0)

if a.mode == "listen":
    c.pump(a.secs)
    print(c.text())
    sys.exit(0)

for ln in a.send_before:
    if ln.startswith("raw:"):
        c.raw(bytes.fromhex(ln[4:]))
    else:
        c.send(ln)
    c.pump(0.7)

start = 0
if a.from_now:
    start = len(c.text())
    print(f"[nxcon] ignoring {start} bytes of replayed backlog", file=sys.stderr)
if a.after:
    n = len(c.text())
    print(f"[nxcon] waiting for marker {a.after!r} (ignoring {n} bytes of backlog)", file=sys.stderr)
    e = c.wait_for(a.after, a.after_timeout, start=n)
    if e < 0:
        print(c.text()[-3000:])
        print(f"[nxcon] FAILED: never saw marker {a.after!r}", file=sys.stderr)
        sys.exit(2)
    start = e
    print(f"[nxcon] marker seen at offset {e}", file=sys.stderr)

spam = bytes.fromhex(a.spam) if a.spam else None
if a.want:
    print(f"[nxcon] waiting for {a.want!r}" + (f", spamming {a.spam}" if spam else ""), file=sys.stderr)
    e = c.wait_for(a.want, a.want_timeout, spam=spam, start=start)
    if e < 0:
        print(c.text()[-4000:])
        print(f"[nxcon] FAILED: never saw {a.want!r}", file=sys.stderr)
        sys.exit(3)
    print(f"[nxcon] matched at offset {e}", file=sys.stderr)

for ln in a.send:
    if ln.startswith("raw:"):
        c.raw(bytes.fromhex(ln[4:]))
    else:
        c.send(ln)
    c.pump(0.7)

c.pump(a.tail)
print(c.text())
