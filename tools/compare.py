#!/usr/bin/env python3
"""compare.py OI_AVI_BINARY FILE.avi...: decode each file with oi-avi and with ffmpeg (as a
reference) and compare the pictures plane by plane.

Prints, per file, how many pictures match exactly and, for the first that does not, where.
ffmpeg is only run as an external reference decoder; nothing of it is linked or included.
"""
import os
import subprocess
import sys
import tempfile


def frames(path, w, h):
    cw, ch = (w + 3) // 4, (h + 3) // 4
    size = w * h + 2 * cw * ch
    data = open(path, "rb").read()
    return [data[k:k + size] for k in range(0, len(data) - size + 1, size)], (w * h, cw * ch)


def main():
    tool = sys.argv[1]
    all_ok = True
    for avi in sys.argv[2:]:
        with tempfile.TemporaryDirectory() as tmp:
            ours, ref = os.path.join(tmp, "ours.yuv"), os.path.join(tmp, "ref.yuv")
            info = subprocess.run([tool, avi, ours], capture_output=True, text=True)
            if not info.stdout.startswith("IV"):
                continue
            head = info.stdout.splitlines()[0].split()
            w, h = map(int, head[1].rstrip(",").split("x"))
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", avi, "-fps_mode", "passthrough", "-f", "rawvideo",
                            "-pix_fmt", "yuv410p", ref], check=True)
            a, (ys, cs) = frames(ours, w, h)
            b, _ = frames(ref, w, h)
            same = 0
            first = None
            for i, (x, y) in enumerate(zip(a, b)):
                if x == y:
                    same += 1
                elif first is None:
                    planes = [(0, ys, "Y", w), (ys, cs, "U", (w + 3) // 4), (ys + cs, cs, "V", (w + 3) // 4)]
                    for off, n, name, pw in planes:
                        diffs = [k for k in range(n) if x[off + k] != y[off + k]]
                        if diffs:
                            k = diffs[0]
                            md = max(abs(x[off + j] - y[off + j]) for j in diffs)
                            first = f"picture {i}: {name} differs at ({k % pw},{k // pw}), {len(diffs)} samples, max diff {md}"
                            break
            # ffmpeg can find chunks past the end of the RIFF data (padding after it) through the
            # index; pictures beyond the stream's own frame count are noted, not compared.
            ok = same == len(a) and len(b) >= len(a)
            extra = len(b) - len(a)
            all_ok &= ok
            status = "OK  " if ok else "FAIL"
            print(f"{status} {os.path.basename(avi)}: {same}/{len(b)} pictures exact (ours {len(a)})"
                  + (f"; {first}" if first else "") + (f"; ffmpeg found {extra} more past the stream" if ok and extra else "")
                  + ("" if info.returncode == 0 else "; decode errors"))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
