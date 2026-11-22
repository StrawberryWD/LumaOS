"""
tests/lmfs_test_helpers.py — shared polling boot helper for the LMFS
day3x tests (day32/33/34/35...).

WHY THIS EXISTS (two compounding problems, not one):

  1. mon_helper.Session's boot_wait is a single fixed time.sleep()
     before a ONE-SHOT screen dump. This sandbox runs QEMU without
     hardware acceleration (no KVM), so wall-clock boot time varies a
     lot between runs depending on host CPU contention — a fixed
     boot_wait=8 (or even 20) is sometimes enough and sometimes isn't,
     for the exact same kernel image.

  2. Much more importantly: the VGA text console is 80x25 with NO
     SCROLLBACK. kmain()'s DAY32+ LMFS test sections print their
     output, and boot execution then CONTINUES past them (the DAY2
     GUI-disable block, then text_shell_run()'s own "[boot] ...
     ready"/"[shell] ... ready"/prompt lines) — that later output
     keeps scrolling the screen, and once enough of it has printed,
     the earlier DAY32-35 lines are gone from video memory FOR GOOD,
     no matter how much longer anyone waits. A single "poll until the
     marker appears, give up after N seconds" loop (this file's first
     draft) doesn't fix this and can actually make it WORSE: if the
     poll interval is coarser than the narrow window during which a
     given day's output is on-screen before later boot output pushes
     it off, every single sample can miss that window and the loop
     just burns through max_wait finding nothing, even though the
     kernel executed completely normally and correctly the whole time.

THE FIX: don't treat this as "wait for a state", treat it as "don't
miss a transient event". Poll frequently (POLL_INTERVAL, well under a
second) from very early in boot, and ACCUMULATE every distinct screen
capture seen along the way (not just the latest one) — so even if a
day's output later scrolls off-screen, it was already captured in an
earlier sample and is still present in the returned combined text.
Stop once the shell prompt has been seen (boot has reached its steady
state — nothing further will ever print) or `max_wait` is exceeded.
"""
import sys
sys.path.insert(0, "tests")
from mon_helper import Session

POLL_INTERVAL = 0.4          # seconds between samples — must be shorter
                             # than the narrow window a day's output
                             # stays on-screen before later boot output
                             # scrolls it away
SHELL_PROMPT_MARKER = "/ >"  # every mount slot's prompt ends this way
                             # ("IDE0/ >", "IDE3/ >", ...) — boot has
                             # reached its permanent steady state once
                             # this shows up; nothing prints after it


def wait_for_marker(img, sockname, marker, max_wait=120):
    """Boot `img` headless, sampling the screen as fast as the QEMU
    monitor round-trip allows (bypassing mon_helper.dump_screen()'s
    own padded fixed sleeps, which are too coarse for this — see this
    file's header comment), accumulating every distinct sample seen.
    Returns the concatenation of all distinct samples. Stops early
    once `marker` has been seen in the accumulated text, or once the
    shell prompt shows up (nothing further will ever print after
    that), whichever comes first, or after `max_wait` seconds.
    """
    import time

    sess = Session(img, sockname, boot_wait=0.5)
    try:
        seen = []
        last = None
        start = time.time()
        while time.time() - start < max_wait:
            sess.s.sendall(b"xp /4000xb 0xb8000\n")
            raw = b""
            sess.s.settimeout(2.0)
            try:
                while True:
                    chunk = sess.s.recv(65536)
                    if not chunk:
                        break
                    raw += chunk
                    if raw.endswith(b"(qemu) "):
                        break
            except OSError:
                pass
            text = raw.decode(errors="replace")
            from qemu_dump_console import parse_monitor_hexdump, render
            try:
                screen = render(parse_monitor_hexdump(text))
            except Exception:
                screen = text

            if screen != last:
                seen.append(screen)
                last = screen
            combined = "\n".join(seen)
            if marker in combined:
                break
            if SHELL_PROMPT_MARKER in screen:
                break
        return "\n".join(seen)
    finally:
        sess.close()