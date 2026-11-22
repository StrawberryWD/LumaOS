#!/usr/bin/env python3
"""
tests/day28_test_month_review.py — DAY28 (月总结) interactive verification.

DAY28 (Week 4, Day 28 of the 5.0 filesystem roadmap — the Month 1 review
day): "能通过 Shell 创建并写入文件。确保ls能展示全部文件 修复全部可能bug
复查语法 并提供delete键支持"

  ("Shell must be able to create and write files. Make sure `ls` shows
   every file. Fix every possible remaining bug, review syntax. And
   provide Delete-key support.")

Same "boot headless, drive the real Shell via QEMU monitor sendkey, dump
the VGA text buffer" technique day21/24/25/26/27's own tests already use,
via this repo's shared tests/mon_helper.py Session wrapper. Five
scenarios, matching this day's sub-requirements:

SCENARIO A — Delete key in the single-line Shell prompt
  (kernel/core/kmain.c's text_shell_run()): type a command with one
  extra stray character, move the cursor back over it with Left, press
  Delete (forward-delete — removes the character AT the cursor, the
  mirror of Backspace), then Enter. Asserts the corrected command
  actually ran (not the broken one).

SCENARIO B — Delete key inside the `write` multi-line editor
  (kernel/shell/cmd/write.c), ordinary in-line case: delete a character
  in the middle of a line, save, read back.

SCENARIO C — Delete key's cross-line forward-merge, full-merge case:
  cursor at the END of a short line, Delete pulls the ENTIRE next line
  up into it (mirror image of Backspace-at-column-0 merging the current
  line into the previous one, DAY27).

SCENARIO D — Delete key's cross-line forward-merge, OVERFLOW case: same
  data-loss trap DAY27's Backspace fix guards against (day27_test_
  truncate.py's own Scenario D2), reproduced via Delete instead: two
  lines whose combined length exceeds WRITE_MAX_LINE_LEN (76) are
  merged via Delete-at-end-of-line; asserts every character survives
  (partial pull-across, remainder kept on the next line — never
  dropped).

SCENARIO E — `create` + `write` end-to-end across several files, then
  `ls`/`read` confirm every one of them is really there with the right
  content and size ("能通过 Shell 创建并写入文件... 确保ls能展示全部
  文件").

SCENARIO F — `ls` accuracy after a create/delete/create mix: creates
  three files, removes the middle one, creates a fourth (which may well
  reuse the just-freed directory slot), then asserts `ls` lists the
  three survivors — each exactly once — and never the deleted one
  ("确保ls能展示全部文件", the "not too few, not too many, no ghosts"
  reading of that requirement).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mon_helper import Session  # noqa: E402

IMG = os.path.join(os.path.dirname(__file__), "..", "lumaos-5.0.img")
import shutil  # noqa: E402

PRISTINE = "/tmp/lumaos_day28_pristine.img"

if not os.path.exists(PRISTINE):
    shutil.copyfile(IMG, PRISTINE)


def fresh_image():
    shutil.copyfile(PRISTINE, IMG)


def scenario_a():
    print("=== SCENARIO A: Delete key fixes a mistyped command "
          "(single-line shell prompt) ===")
    fresh_image()
    sess = Session(IMG, "day28_a", boot_wait=10)
    try:
        import time
        # Deliberately mistype "echo" as "echoo" (one extra 'o'), then
        # fix it in place with Left + Delete before pressing Enter —
        # never actually submit the broken command.
        sess.type_line("echoo hello28")
        time.sleep(0.3)
        # Cursor is at the end (index 13, after "echoo hello28"). The
        # stray extra 'o' sits at index 4 ("ech|o|o hello28") — move
        # the cursor there with 9 Lefts (13 - 4 = 9). A small settle
        # delay between each Left avoids a QEMU-monitor-sendkey timing
        # artifact seen when many "sendkey left" calls are issued back
        # to back with no gap (day26_test_rm.py's own "an extra monitor
        # round-trip can perturb the very next sendkey batch's timing"
        # comment documents the same class of harness flakiness) —
        # this is a test-harness pacing concern, not a kernel bug: a
        # host-side VGA-memory probe confirmed the kernel's own
        # input_cursor/input_len state was already correct even on a
        # run where the visible result came out wrong.
        for _ in range(9):
            sess.press("left")
            time.sleep(0.15)
        time.sleep(0.2)
        sess.press("delete")
        time.sleep(0.3)
        sess.press("ret")
        screen = sess.dump_screen(0.4)
        assert "hello28" in screen, (
            f"A FAIL: corrected command's output missing\n{screen}")
        assert "Unknown: echoo" not in screen, (
            f"A FAIL: the broken 'echoo' command was submitted instead "
            f"of the corrected one\n{screen}")
        print("PASS A: Delete removed the stray character in place; "
              "the CORRECTED command ('echo hello28') is what actually "
              "ran")
    finally:
        sess.close()


def scenario_b():
    print("=== SCENARIO B: Delete key, ordinary in-line case "
          "(write editor) ===")
    fresh_image()
    sess = Session(IMG, "day28_b", boot_wait=10)
    try:
        sess.type_line("write IDE0/DELB.TXT")
        sess.press("ret")
        import time
        time.sleep(0.4)
        sess.type_line("abXcd")
        # cursor is at col 5 (end); move left 3 -> col 2, sitting right
        # before the 'X' at index 2. Small settle delay between Lefts —
        # see Scenario A's own comment on this QEMU-sendkey timing
        # artifact.
        for _ in range(3):
            sess.press("left")
            time.sleep(0.15)
        time.sleep(0.2)
        sess.press("delete")
        time.sleep(0.2)
        sess.press("ctrl-s")
        time.sleep(0.6)
        sess.dump_screen(0.3)

        sess.type_line("read IDE0/DELB.TXT")
        sess.press("ret")
        screen = sess.dump_screen(0.5)
        assert "abcd" in screen, f"B FAIL: expected 'abcd', got:\n{screen}"
        assert "(4 bytes)" in screen, (
            f"B FAIL: wrong byte count (expected 4)\n{screen}")
        print("PASS B: Delete removed exactly the character under the "
              "cursor ('X'), leaving 'abcd' intact")
    finally:
        sess.close()


def scenario_c():
    print("=== SCENARIO C: Delete key, full cross-line forward-merge "
          "===")
    fresh_image()
    sess = Session(IMG, "day28_c", boot_wait=10)
    try:
        import time
        sess.type_line("write IDE0/DELC.TXT")
        sess.press("ret")
        time.sleep(0.4)
        sess.type_line("ab")
        sess.press("ret")
        sess.type_line("cd")
        time.sleep(0.2)
        # Cursor is at row1/col2 (end of "cd"). Up moves to row0; since
        # 2 <= len("ab")==2, the column is NOT clamped and lands
        # exactly at col 2 — the END of line 0 — which is exactly the
        # position this scenario needs.
        sess.press("up")
        sess.press("delete")
        time.sleep(0.3)
        sess.press("ctrl-s")
        time.sleep(0.6)
        sess.dump_screen(0.3)

        sess.type_line("read IDE0/DELC.TXT")
        sess.press("ret")
        screen = sess.dump_screen(0.5)
        assert "abcd" in screen, f"C FAIL: expected 'abcd', got:\n{screen}"
        assert "(4 bytes)" in screen, (
            f"C FAIL: wrong byte count — the merge should have produced "
            f"ONE 4-byte line, no newline left over\n{screen}")
        print("PASS C: Delete at end-of-line fully pulled the next line "
              "up into the current one ('ab'+'cd' -> 'abcd', one line)")
    finally:
        sess.close()


def scenario_d():
    print("=== SCENARIO D: Delete key, OVERFLOW cross-line "
          "forward-merge (no data lost) ===")
    fresh_image()
    sess = Session(IMG, "day28_d", boot_wait=10)
    try:
        import time
        sess.type_line("write IDE0/DELD.TXT")
        sess.press("ret")
        time.sleep(0.4)
        # Line 0 = 60 'b's, Line 1 = 70 'a's. Typing "a"*70 leaves the
        # cursor naturally at the END of line 1 (col 70).
        sess.type_line("b" * 60)
        sess.press("ret")
        sess.type_line("a" * 70)
        time.sleep(0.2)
        # Up: cur_col was 70, line_lens[0] is 60 -> clamped DOWN to 60,
        # which is exactly the end of line 0 ("b"*60). This is the
        # position the overflow-merge case needs.
        sess.press("up")
        sess.press("delete")
        time.sleep(0.3)
        sess.press("ctrl-s")
        time.sleep(0.8)
        sess.dump_screen(0.3)

        sess.type_line("read IDE0/DELD.TXT")
        sess.press("ret")
        screen = sess.dump_screen(0.5)
        # room = 76 - 60 = 16 of the 70 'a's fit onto line 0 (now full
        # at 76 chars); the remaining 54 'a's stay on line 1 — nothing
        # dropped. Total bytes = 76 + 1 (newline) + 54 = 131, matching
        # day27_test_truncate.py's own Scenario D2 byte count exactly
        # (same 60+70=130 characters, same one newline).
        assert "(131 bytes)" in screen, (
            f"D FAIL: expected all 130 typed characters (+1 newline) "
            f"preserved, got:\n{screen}")
        total_ab = screen.count("a") + screen.count("b")
        assert total_ab >= 130, f"D FAIL: characters missing\n{screen}"
        print("PASS D: overflowing forward-merge kept all 130 "
              "characters (line 1 fills to 76 with the first 16 'a's, "
              "the remaining 54 stay on line 2) — no data lost")
    finally:
        sess.close()


def scenario_e():
    print("=== SCENARIO E: create + write across several files, "
          "ls + read confirm all of them ===")
    fresh_image()
    sess = Session(IMG, "day28_e", boot_wait=10)
    try:
        import time
        # Shell starts with the prompt on IDE2 (kmain.c's own boot-time
        # choice) — `ls`/bare-name commands act on whichever volume is
        # currently `cd`'d into, so switch to IDE0 first (matches
        # DAY25's own cd.c contract) rather than relying on the
        # "IDE0/..." prefix in each path alone, since `ls` itself takes
        # no path argument (kernel/shell/cmd/ls.c's own DAY14 comment:
        # it always lists vfs_root, whatever that currently is).
        sess.type_line("cd IDE0")
        sess.press("ret")
        time.sleep(0.3)
        files = [
            ("E1.TXT", "first file"),
            ("E2.TXT", "second file is a bit longer"),
            ("E3.TXT", "third"),
        ]
        for fname, content in files:
            sess.type_line(f"create IDE0/{fname}")
            sess.press("ret")
            time.sleep(0.3)
            sess.type_line(f"write IDE0/{fname}")
            sess.press("ret")
            time.sleep(0.4)
            sess.type_line(content)
            sess.press("ctrl-s")
            time.sleep(0.6)
            sess.dump_screen(0.2)

        sess.type_line("ls")
        sess.press("ret")
        ls_screen = sess.dump_screen(0.4)
        # Isolate just the `ls` listing itself — see Scenario F's own
        # comment on why the whole scrollback (including earlier
        # "wrote N bytes to ..." echoes) can't be searched directly
        # without risking a false-positive size match.
        ls_marker = "> ls"
        idx = ls_screen.rfind(ls_marker)
        assert idx != -1, f"E FAIL: couldn't find the 'ls' command echo\n{ls_screen}"
        listing = ls_screen[idx + len(ls_marker):]
        for fname, content in files:
            assert fname in listing, (
                f"E FAIL: {fname} missing from ls\n{listing}")
            expected_size = f"{len(content)} bytes"
            assert expected_size in listing, (
                f"E FAIL: {fname} size wrong (expected {expected_size})"
                f"\n{listing}")
        print(f"PASS E (ls): all {len(files)} created files appear with "
              f"correct sizes")

        for fname, content in files:
            sess.type_line(f"read IDE0/{fname}")
            sess.press("ret")
            screen = sess.dump_screen(0.4)
            assert content in screen, (
                f"E FAIL: {fname} content wrong, expected "
                f"'{content}'\n{screen}")
        print("PASS E (read): every file's content round-trips exactly "
              "as written")
    finally:
        sess.close()


def scenario_f():
    print("=== SCENARIO F: ls stays accurate through "
          "create/rm/create (no ghosts, no gaps) ===")
    fresh_image()
    sess = Session(IMG, "day28_f", boot_wait=10)
    try:
        import time
        sess.type_line("cd IDE0")
        sess.press("ret")
        time.sleep(0.3)
        for fname in ("F1.TXT", "F2.TXT", "F3.TXT"):
            sess.type_line(f"create IDE0/{fname}")
            sess.press("ret")
            time.sleep(0.3)

        sess.type_line("rm IDE0/F2.TXT")
        sess.press("ret")
        time.sleep(0.3)

        # F4 may well land in F2's just-freed directory slot — exactly
        # the scenario that most exercises a readdir/ls bug, if one
        # existed (a stale FAT_DIRENT_FREE skip, an off-by-one in the
        # slot-reuse scan, etc.).
        sess.type_line("create IDE0/F4.TXT")
        sess.press("ret")
        time.sleep(0.3)

        sess.type_line("ls")
        sess.press("ret")
        screen = sess.dump_screen(0.4)

        # This QEMU session's whole scrollback fits in one 25-row VGA
        # dump (nothing has scrolled off yet), so `screen` also still
        # contains every earlier "create IDE0/F1.TXT" command echo and
        # "created IDE0/F1.TXT" success line from above — counting a
        # filename's occurrences across the WHOLE screen would count
        # those too, not just the actual `ls` listing. Isolate just the
        # listing itself: everything after the last "> ls" prompt line.
        ls_marker = "> ls"
        idx = screen.rfind(ls_marker)
        assert idx != -1, f"F FAIL: couldn't find the 'ls' command echo\n{screen}"
        listing = screen[idx + len(ls_marker):]

        assert listing.count("F1.TXT") == 1, (
            f"F FAIL: F1.TXT should appear exactly once in the ls "
            f"listing\n{listing}")
        assert "F2.TXT" not in listing, (
            f"F FAIL: deleted F2.TXT still shows up in the ls listing"
            f"\n{listing}")
        assert listing.count("F3.TXT") == 1, (
            f"F FAIL: F3.TXT should appear exactly once in the ls "
            f"listing\n{listing}")
        assert listing.count("F4.TXT") == 1, (
            f"F FAIL: F4.TXT (reusing F2's freed slot) should appear "
            f"exactly once in the ls listing\n{listing}")
        print("PASS F: ls lists exactly {F1.TXT, F3.TXT, F4.TXT} — the "
              "deleted F2.TXT never reappears, the slot-reusing F4.TXT "
              "is listed once, nothing duplicated or missing")
    finally:
        sess.close()


if __name__ == "__main__":
    scenario_a()
    scenario_b()
    scenario_c()
    scenario_d()
    scenario_e()
    scenario_f()
    print("\nALL DAY28 SCENARIOS PASSED")