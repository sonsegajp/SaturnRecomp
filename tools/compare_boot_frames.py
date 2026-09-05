"""Compare Ymir and SaturnRecomp boot captures one emulated frame at a time."""
from __future__ import annotations

import argparse
import csv
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageStat


def load(path: Path) -> Image.Image:
    return Image.open(path).convert("RGB").resize((320, 224), Image.Resampling.NEAREST)


def stats(image: Image.Image) -> tuple[int, float]:
    px = np.asarray(image, dtype=np.uint8)
    nonblack = int(np.count_nonzero(np.any(px != 0, axis=2)))
    mean_luma = float(np.mean(px[..., 0] * 0.299 + px[..., 1] * 0.587 + px[..., 2] * 0.114))
    return nonblack, mean_luma


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("ymir", type=Path)
    ap.add_argument("saturnrecomp", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--frames", type=int, default=560)
    ap.add_argument("--pairs", type=Path,
                    help="optional directory for labelled side-by-side frame images")
    args = ap.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.pairs:
        args.pairs.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as f:
        out = csv.writer(f)
        out.writerow(("frame", "ymir_nonblack", "saturnrecomp_nonblack",
                      "ymir_mean_luma", "saturnrecomp_mean_luma", "mean_abs_rgb_error"))
        for frame in range(args.frames):
            yp = args.ymir / f"frame_{frame:04d}.bmp"
            # SaturnRecomp numbers fields from one; align its first completed
            # field with Ymir frame zero.
            sp = args.saturnrecomp / f"frame_{frame + 1:04d}.png"
            if not sp.exists():
                # SATURN_FRAME_SHOTS appends the field number directly to the
                # configured prefix. Accept both prefix="frame_" and the older
                # prefix="frame" suites so a completed capture is reusable.
                sp = args.saturnrecomp / f"frame{frame + 1:04d}.png"
            y, s = load(yp), load(sp)
            yn, yl = stats(y)
            sn, sl = stats(s)
            err = sum(ImageStat.Stat(ImageChops.difference(y, s)).mean) / 3.0
            out.writerow((frame, yn, sn, f"{yl:.3f}", f"{sl:.3f}", f"{err:.3f}"))
            if args.pairs:
                pair = Image.new("RGB", (640, 244), "black")
                pair.paste(y, (0, 20))
                pair.paste(s, (320, 20))
                draw = ImageDraw.Draw(pair)
                draw.text((4, 4), f"Ymir frame {frame:04d}", fill="white")
                draw.text((324, 4), f"SaturnRecomp frame {frame:04d}", fill="white")
                pair.save(args.pairs / f"compare_{frame:04d}.png")


if __name__ == "__main__":
    main()
