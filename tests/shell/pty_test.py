#!/usr/bin/env python3
"""drives odin's interactive mode through a real terminal (pty), pressing keys like a person.
usage: pty_test.py path/to/odin      (mac / linux only, windows has no pty)"""
import os, pty, re, select, subprocess, sys, tempfile, time

odin = os.path.abspath(sys.argv[1])
here = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp()
results = []

def check(name, ok, dump=""):
    results.append(ok)
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok and dump: print(dump[-1500:])

def session(script, env=None):
    """script is a list of (keys, seconds_to_wait). returns everything odin printed"""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update({"TERM": "xterm-256color", "NO_PROXY": "*", "no_proxy": "*"}, **(env or {}))
        os.execv(odin, [odin])
    out = b""
    def pump(t):
        nonlocal out
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try: out += os.read(fd, 65536)
                except OSError: return
    pump(1.0)
    for keys, wait in script:
        if callable(keys): keys(fd, pump, lambda: out)
        else:
            os.write(fd, keys.encode()); pump(wait)
    _, status = os.waitpid(pid, 0)
    return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", out).decode("utf-8", "replace"), os.WEXITSTATUS(status)

# --- editing, history, ctrl+c at the prompt, dragged paths, long lines, ctrl+d ---
os.makedirs(os.path.join(tmp, "drag me"))
sample = os.path.join(tmp, "drag me", "IMG 1.png")
with open(sample, "wb") as f:  # tiny png with an author text chunk
    import struct, zlib
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
            + chunk(b"tEXt", b"Author\x00pty-test-author") + chunk(b"IEND", b""))
UP, HOME, RIGHT, BS = "\x1b[A", "\x1b[H", "\x1b[C", "\x7f"
txt, code = session([
    ("versoin" + BS * 3 + "ion\r", 0.6),            # typo fixed with backspace
    ("vrsion" + HOME + RIGHT + "e\r", 0.6),          # home, right arrow, insert a letter
    (UP + UP + "\r", 0.6),                           # history
    ("throw this away\x03", 0.6),                    # ctrl+c at the prompt
    ("meta -qf " + sample.replace(" ", "\\ ") + "\r", 1.5),  # mac-style dragged path
    ("odin user " + "x" * 120 + " --list\r", 1.2),   # wraps past the screen edge
    ("\x04", 0.8),                                   # ctrl+d
])
check("typing, backspace, arrows, history", txt.count("odin 0.") >= 3, txt)
check("ctrl+c drops the line", "^C" in txt and "no idea what" not in txt, txt)
check("dragged path with an escaped space", "pty-test-author" in txt, txt)
check("long wrapped line still runs", "sites" in txt and "GitHub" in txt, txt)
check("ctrl+d leaves cleanly", "later" in txt and code == 0, txt)

# --- ctrl+c during a scan stops it and comes back to the prompt ---
port = 20000 + os.getpid() % 20000
srv = subprocess.Popen([sys.executable, os.path.join(here, "..", "user", "fake_sites.py"), str(port)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
sites = os.path.join(tmp, "slow.txt")
with open(sites, "w") as f:
    f.write("".join(f"Slow{i} | http://127.0.0.1:{port}/slow/{{}} | status\n" for i in range(6)))
took = [99]
def interrupt(fd, pump, out):
    mark, t0 = len(out()), time.time()
    os.write(fd, b"\x03")
    while time.time() - t0 < 5 and b"stopped" not in out()[mark:]: pump(0.05)
    took[0] = time.time() - t0
    pump(0.3)
try:
    txt, code = session([(f"user -t 30 --sites {sites} someone\r", 1.0), (interrupt, 0),
                         ("version\r", 0.8), ("exit\r", 0.8)])
finally:
    srv.kill()
check("ctrl+c stops a scan fast", "stopped" in txt and took[0] < 1.5, txt)
check("stopped sites are counted, not listed", "6 stopped" in txt and "Slow0" not in txt, txt)
check("back at the prompt after", "odin 0." in txt.split("6 stopped")[-1] and code == 0, txt)

print(f"{sum(results)}/{len(results)} interactive checks passed")
sys.exit(0 if all(results) else 1)
