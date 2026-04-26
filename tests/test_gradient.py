#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run gradient example and compare produced frames against golden images with FLIP."
    )
    parser.add_argument("--example", required=True, help="Path to the gradient example executable")
    parser.add_argument("--golden-dir", required=True, help="Directory containing golden frame_*.png images")
    parser.add_argument("--output-dir", required=True, help="Directory where rendered frames will be generated")
    parser.add_argument(
        "--max-flip",
        type=float,
        default=0.0,
        help="Maximum allowed FLIP score per frame (default: 0.0)",
    )
    return parser.parse_args()


def _run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        capture_output=True,
        text=True,
        check=False,
    )


def _extract_flip_score(text: str) -> float | None:
    patterns = [
        r"(?:mean|avg|average)\s*(?:flip)?\s*[:=]\s*([-+]?(?:\d+\.\d+|\d+)(?:[eE][-+]?\d+)?)",
        r"\bflip\s*[:=]\s*([-+]?(?:\d+\.\d+|\d+)(?:[eE][-+]?\d+)?)",
    ]
    for pattern in patterns:
        match = re.search(pattern, text, flags=re.IGNORECASE)
        if match:
            return float(match.group(1))

    # Conservative fallback: if FLIP prints a single standalone number, use it.
    numbers = re.findall(r"[-+]?(?:\d+\.\d+|\d+)(?:[eE][-+]?\d+)?", text)
    if len(numbers) == 1:
        return float(numbers[0])
    return None


def run_flip(reference: Path, test: Path) -> float:
    # FLIP CLI variants differ slightly between package versions.
    candidates = [
        ["flip-cuda-cli", "-r", str(reference), "-t", str(test), "-v"],
        ["flip-cuda-cli", str(reference), str(test), "-v"],
        ["flip", "-r", str(reference), "-t", str(test), "-v"],
        ["flip", str(reference), str(test), "-v"],
    ]
    errors: list[str] = []

    for cmd in candidates:
        proc = _run(cmd)
        if proc.returncode != 0:
            errors.append(
                f"command failed: {' '.join(cmd)}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
            continue

        merged = f"{proc.stdout}\n{proc.stderr}"
        score = _extract_flip_score(merged)
        if score is None:
            errors.append(
                f"could not parse FLIP score from command: {' '.join(cmd)}\noutput:\n{merged}"
            )
            continue
        return score

    raise RuntimeError(
        "unable to run FLIP successfully for image comparison.\n" + "\n\n".join(errors)
    )


def main() -> int:
    args = parse_args()
    example = Path(args.example).resolve()
    golden_dir = Path(args.golden_dir).resolve()
    output_dir = Path(args.output_dir).resolve()

    if shutil.which("flip-cuda-cli") is None and shutil.which("flip") is None:
        raise RuntimeError("neither `flip-cuda-cli` nor `flip` command was found in PATH.")
    if not example.is_file():
        raise RuntimeError(f"gradient executable does not exist: {example}")
    if not golden_dir.is_dir():
        raise RuntimeError(f"golden directory does not exist: {golden_dir}")

    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    example_run = _run([str(example)], cwd=output_dir)
    if example_run.returncode != 0:
        raise RuntimeError(
            "failed to run gradient example.\n"
            f"stdout:\n{example_run.stdout}\n"
            f"stderr:\n{example_run.stderr}"
        )

    rendered_dir = output_dir / "frames_out"
    if not rendered_dir.is_dir():
        raise RuntimeError(f"example did not produce expected output directory: {rendered_dir}")

    golden_frames = sorted(golden_dir.glob("frame_*.png"))
    if not golden_frames:
        raise RuntimeError(f"no golden frames found in {golden_dir}")

    failures: list[str] = []
    for ref in golden_frames:
        out = rendered_dir / ref.name
        if not out.is_file():
            failures.append(f"missing output frame: {out}")
            continue

        score = run_flip(ref, out)
        if score > args.max_flip:
            failures.append(
                f"{ref.name}: FLIP score {score:.8f} exceeded threshold {args.max_flip:.8f}"
            )

    if failures:
        raise AssertionError("gradient frame comparison failed:\n" + "\n".join(failures))

    print(f"validated {len(golden_frames)} frame(s) with FLIP <= {args.max_flip:.8f}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
