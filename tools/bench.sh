#!/bin/bash
# Log 30 s at the benchmark spot: Afterburner FPS + GPU clocks, plus the meshemu profile lines of the window.
#   tools/bench.sh <label>    -> work/fps_<label>.csv, work/clocks_<label>.txt
set -e
M="$(cd "$(dirname "$0")/.." && pwd)"
L="${1:?label}"
G="${MESHEMU_GAME:-/c/Program Files (x86)/Steam/steamapps/common/FINAL FANTASY VII REBIRTH/End/Binaries/Win64}/meshemu"   # MESHEMU_GAME: the Win64 folder
cd "$M"
echo "GPU now: $(nvidia-smi --query-gpu=pstate,clocks.sm,temperature.gpu,fan.speed,power.draw --format=csv,noheader)"
echo "converter/PSO activity in the last 2 min: $(awk -v t="$(date -d '-2 min' +%H:%M:%S)" '$1 >= t' "$G/meshemu.log" | grep -cE 'mesh PSO|late-built|converter finished|null:')"
grep -E "null: |skipped \(" "$G/meshemu.log" | tail -2 || true
T0=$(date +%H:%M:%S)
(for i in $(seq 1 15); do nvidia-smi --query-gpu=pstate,clocks.sm,temperature.gpu,power.draw,utilization.gpu,clocks_event_reasons.active,fan.speed --format=csv,noheader; sleep 2; done) > "work/clocks_$L.txt" &
python -I tools/fpslog.py "work/fps_$L.csv" 30
wait
T1=$(date +%H:%M:%S)
awk -F', ' '{c+=$2; if($3>tm)tm=$3; if(tn==""||$3<tn)tn=$3; p+=$4; f+=$7; n++; if($6!~/0x0+20$/)ok++} END{printf "GPU: avg SM %.0f MHz, temp %d-%d C, power %.0f W, fan %.0f%%, thermal slowdown in %d of %d samples\n", c/n, tn, tm, p/n, f/n, n-ok, n}' "work/clocks_$L.txt"
python - "$L" <<'PY'
import csv, statistics as st, sys
rows = list(csv.DictReader(open(f"work/fps_{sys.argv[1]}.csv")))
fps = [float(r["Framerate"]) for r in rows]; ft = [float(r["Frametime"]) for r in rows]; mem = [float(r["Memory usage"]) for r in rows]
print(f"{sys.argv[1]}: FPS avg {st.mean(fps):.1f} (se {st.stdev(fps)/len(fps)**0.5:.2f}) min {min(fps):.1f} max {max(fps):.1f}, frametime {st.mean(ft):.1f} ms, VRAM {st.mean(mem):.0f} MB")
PY
grep -E "profile:" "$G/meshemu.log" | awk -v a="$T0" -v b="$T1" '$1 >= a && $1 <= b' | python -c "
import sys,re
n=cs=dr=span=0
for ln in sys.stdin:
    m=re.search(r'(\d+) draws over ([\d.]+) ms.*cs ([\d.]+) ms \+ draw ([\d.]+) ms',ln)
    if not m: continue
    d,s,c,w=int(m[1]),float(m[2]),float(m[3]),float(m[4]); n+=d; span+=s; cs+=c; dr+=w
print(f'emulation: {n} draws in window, cs {1000*cs/n:.1f} us + draw {1000*dr/n:.1f} us per draw, {100*(cs+dr)/span:.0f}% of GPU time' if n else 'no profile lines in window')"
grep -E "draw sizes" "$G/meshemu.log" | tail -1 | sed -E 's/^[0-9:.]+ \[ *[0-9]+\] //'
