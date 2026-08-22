#!/usr/bin/env python3
"""Mirror one eye's distortion calibration onto the other, horizontally flipped.

Rationale: lens/display distortion is symmetric by design between the two
eyes, and a shared mirrored profile avoids inter-eye disparity in residual
error (which the visual system tolerates far worse than shared error). The
per-eye calibration path remains for when the mirror prior isn't enough.

Modes are auto-detected from the profile: every per-eye structure present is
mirrored (lattice displacement map, plain per-eye radial curves, per-axis
curves, segmented curves, center offsets). The transform is an involution:
mirroring twice restores the original exactly.

Usage:
  python tools/gxr_mirror.py                       # settings.json, left -> right
  python tools/gxr_mirror.py --from right          # right -> left
  python tools/gxr_mirror.py --file myprofile.json # a saved profile file
  python tools/gxr_mirror.py --in-place            # overwrite instead of *-mirrored
  python tools/gxr_mirror.py --self-test

Transform rules:
  lattice map   right[row][col] = (-du, +dv) from left[row][cols-1-col]
  plain radial  copied verbatim (radius is mirror-invariant)
  per-axis      leftHorizontal -> rightHorizontal etc., verbatim
  segments      dst#k' = src#k with k' = (N/2 - k - 1) mod N (angle 0 = screen
                right, increasing toward screen down, values at segment
                centers). Requires even N: for odd N a center mirrors onto a
                boundary and is not representable without resampling.
  centers       centerOffsetX{dst} = -centerOffsetX{src} (offset-from-center
                form, so horizontal mirroring is a sign flip); Y untouched.
"""
import argparse, copy, json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def find_distortion(root):
    """Return (distortion_node, streamframe_node_or_None)."""
    if isinstance(root, dict):
        sf = root.get("streamFrame")
        if isinstance(sf, dict) and isinstance(sf.get("distortion"), dict):
            return sf["distortion"], sf
        if isinstance(root.get("distortion"), dict):
            return root["distortion"], root if "centerOffsetXLeft" in root else None
        if "curves" in root or "map" in root:
            return root, None
    raise SystemExit("could not find a distortion section (streamFrame.distortion, distortion, or curves/map at root)")


def mirror_map(m, src, dst):
    cols, rows = int(m.get("cols", 0)), int(m.get("rows", 0))
    data = m.get(src)
    if not isinstance(data, list) or not data:
        return False
    if cols <= 0 or rows <= 0 or len(data) != cols * rows * 2:
        raise SystemExit(f"map.{src} length {len(data)} does not match cols*rows*2 = {cols*rows*2}")
    out = [0.0] * len(data)
    for r in range(rows):
        for c in range(cols):
            si = (r * cols + (cols - 1 - c)) * 2
            di = (r * cols + c) * 2
            out[di] = -data[si]      # du negated
            out[di + 1] = data[si + 1]  # dv kept
    m[dst] = out
    if isinstance(m.get("source"), str) and "mirror" not in m["source"]:
        m["source"] = m["source"] + f"+mirror{src[0].upper()}{dst[0].upper()}"
    return True


def mirror_curves(curves, segments_hint, src, dst):
    done = []
    if src in curves:
        curves[dst] = copy.deepcopy(curves[src]); done.append("radial")
    for axis in ("Horizontal", "Vertical"):
        if src + axis in curves:
            curves[dst + axis] = copy.deepcopy(curves[src + axis]); done.append(axis.lower())
    seg_keys = [k for k in curves if k.startswith(src + "#")]
    if seg_keys:
        idx = sorted(int(k.split("#", 1)[1]) for k in seg_keys)
        n = int(segments_hint) if segments_hint and int(segments_hint) == len(idx) else (max(idx) + 1)
        if idx != list(range(n)):
            raise SystemExit(f"segment curves {src}#* are not contiguous 0..{n-1}: {idx}")
        if n % 2 != 0:
            raise SystemExit(f"segment count {n} is odd: mirrored centers fall on boundaries; "
                             "re-save the profile with an even segment count first")
        # drop any stale destination segments so counts can never disagree
        for k in [k for k in curves if k.startswith(dst + "#")]:
            del curves[k]
        for k in range(n):
            kp = (n // 2 - k - 1) % n
            curves[f"{dst}#{kp}"] = copy.deepcopy(curves[f"{src}#{k}"])
        done.append(f"{n} segments")
    return done


def mirror(root, src):
    dst = "right" if src == "left" else "left"
    dist, sf = find_distortion(root)
    report = []
    if isinstance(dist.get("map"), dict) and mirror_map(dist["map"], src, dst):
        report.append("lattice map")
    if isinstance(dist.get("curves"), dict):
        report += mirror_curves(dist["curves"], dist.get("segments"), src, dst)
    if sf is not None:
        s, d = f"centerOffsetX{src.capitalize()}", f"centerOffsetX{dst.capitalize()}"
        if isinstance(sf.get(s), (int, float)):
            sf[d] = -sf[s]
            report.append("centerOffsetX (sign-flipped)")
    if not report:
        raise SystemExit(f"nothing to mirror: no {src}-eye data found")
    return dst, report


def self_test():
    prof = {"streamFrame": {"centerOffsetXLeft": 0.013, "centerOffsetXRight": 0.0, "distortion": {
        "segments": 8,
        "curves": {
            "left": {"k1": 0.1, "k2": 0.0, "points": [{"r": 0.5, "scale": 1.01}]},
            "leftHorizontal": {"k1": 0.2, "k2": 0.0, "points": []},
            **{f"left#{k}": {"k1": k * 0.01, "k2": 0.0, "points": [{"r": 0.4, "scale": 1 + k * 0.001}]} for k in range(8)},
        },
        "map": {"enable": True, "cols": 3, "rows": 2, "source": "graycode",
                 "left": [float(i) for i in range(12)], "right": []},
    }}}
    once = copy.deepcopy(prof); mirror(once, "left")
    # segment permutation spot checks (N=8): k'=(4-k-1) mod 8
    c = once["streamFrame"]["distortion"]["curves"]
    assert c["right#3"]["k1"] == c["left#0"]["k1"], "k=0 -> k'=3"
    assert c["right#7"]["k1"] == c["left#4"]["k1"], "k=4 -> k'=7"
    # map: first row reversed with du negated
    m = once["streamFrame"]["distortion"]["map"]
    assert m["right"][0] == -m["left"][4] and m["right"][1] == m["left"][5]
    assert once["streamFrame"]["centerOffsetXRight"] == -0.013
    # involution: mirror right back onto left, compare to original left data
    twice = copy.deepcopy(once); mirror(twice, "right")
    t, o = twice["streamFrame"], prof["streamFrame"]
    assert t["distortion"]["map"]["left"] == o["distortion"]["map"]["left"]
    for k in list(o["distortion"]["curves"]):
        assert t["distortion"]["curves"][k] == o["distortion"]["curves"][k], k
    assert t["centerOffsetXLeft"] == o["centerOffsetXLeft"]
    print("self-test passed (permutation, map flip, center sign, involution)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", default=None, help="profile or settings json (default: driver settings.json)")
    ap.add_argument("--from", dest="src", choices=("left", "right"), default="left")
    ap.add_argument("--in-place", action="store_true", help="overwrite the input file (default: write *-mirrored.json)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    path = a.file
    if path is None:
        import gxr_common
        path = gxr_common.settings_path()
    with open(path, "r", encoding="utf-8") as f:
        root = json.load(f)
    dst, report = mirror(root, a.src)
    out = path if a.in_place else (os.path.splitext(path)[0] + "-mirrored.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(root, f, indent="\t")
    print(f"mirrored {a.src} -> {dst}: " + ", ".join(report))
    print(f"wrote {out}" + ("" if a.in_place else f"  (original untouched: {path})"))


if __name__ == "__main__":
    sys.exit(main())
