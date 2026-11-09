# Judges every recorded turn in results\*-frames.csv for the three failures #19 asks about.
#
# Per frame: match_old / match_new = share of the pixels that differ between the old and new Spread
# which look like the old / new Spread in that frame. A plain Curl moves smoothly from (1, 0) to (0, 1).
#
#   flash     : the new Spread was on screen, then got covered again (new rises to >= 0.6, then drops by
#               0.15 or more), or more than half of it shows in the first 40% of the Curl
#   old back  : once the Curl has started, the old Spread comes back (old rises by 0.10 or more)
#   gap       : once the new Spread is fully on screen, a later frame isn't (new < 0.985)
#
#   py analyze.py [results-dir]
import csv, glob, os, sys
from collections import defaultdict

CURL_MS = 700.0
res = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "results")

for path in sorted(glob.glob(os.path.join(res, "*-frames.csv"))):
    turns = defaultdict(list)
    kinds = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            k = (r["run"], int(r["turn"]))
            kinds[k] = r["kind"]
            turns[k].append((float(r["ms_after_tap"]), int(r["accumulated"]), float(r["match_old"]), float(r["match_new"])))
    tot = defaultdict(int)
    lines = []
    for k, fr in sorted(turns.items()):
        flash = old_back = gap = False
        peak_new, min_old, started, settled = 0.0, 1.0, False, False
        missed = 0
        first_new_full = None
        for t, acc, o, n in fr:
            missed += max(0, acc - 1)
            if n >= 0.6:
                peak_new = max(peak_new, n)
            if peak_new > 0 and n < peak_new - 0.15 and not settled:
                flash = True
            if t < CURL_MS * 0.4 and n > 0.5:
                flash = True
            if o < 0.98:
                started = True
            if started and not settled:
                if o > min_old + 0.10:
                    old_back = True
                min_old = min(min_old, o)
            if settled and n < 0.985 and t > CURL_MS:
                gap = True
            if n >= 0.985 and not settled:
                settled = True
                first_new_full = t
        tot["turns"] += 1
        tot["flash"] += flash
        tot["old_back"] += old_back
        tot["gap"] += gap
        tot["missed"] += missed
        tot["unsettled"] += not settled
        lines.append("  turn %2d %-8s new fully shown at %6s ms | %2d frames, %d missed | %s%s%s%s" % (
            k[1], kinds[k], "%.0f" % first_new_full if first_new_full else "never", len(fr), missed,
            "FLASH " if flash else "", "OLD-BACK " if old_back else "", "GAP " if gap else "",
            "" if settled else "NEVER-SETTLED"))
    run = os.path.basename(path)[: -len("-frames.csv")]
    print("%s: %d turns, flash %d, old Spread back %d, gap at removal %d, never settled %d, frames the recorder missed %d"
          % (run, tot["turns"], tot["flash"], tot["old_back"], tot["gap"], tot["unsettled"], tot["missed"]))
    if "-v" in sys.argv:
        print("\n".join(lines))
