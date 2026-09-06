"""Alternate baseline/candidate runtime timings; all artifacts stay under out/."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SMPC = bytes((83, 0, 0, 0, 0, 1, 0, 0))


def bounded_integer(low: int, high: int):
    def convert(text: str) -> int:
        value = int(text)
        if not low <= value <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return value
    return convert


def refresh_rate(text: str) -> int:
    value = int(text)
    if value != 0 and not 60 <= value <= 240:
        raise argparse.ArgumentTypeError("must be 0 (off), or between 60 and 240")
    return value


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--baseline", required=True, type=Path)
    result.add_argument("--candidate", required=True, type=Path)
    result.add_argument("--game", required=True, type=Path, help="Local game.toml")
    result.add_argument("--repeats", type=bounded_integer(1, 20), default=3,
                        help="Number of pairs; each pair runs both executables (default: 3)")
    result.add_argument("--frames", type=bounded_integer(1, 1000000), default=7200)
    inputs = result.add_mutually_exclusive_group()
    inputs.add_argument("--pad-sequence", default="", help="cycle:low[:high] entries separated by commas")
    inputs.add_argument("--pad-sequence-file", type=Path, help="Same entries separated by commas or newlines")
    result.add_argument("--smpc-file", type=Path, help="Initial console-state seed; copied separately for every run")
    result.add_argument("--present-hz", type=refresh_rate, default=0)
    result.add_argument("--internal-scale", type=bounded_integer(1, 4), default=1)
    result.add_argument("--texture-filter", type=int, choices=(0, 1), default=0)
    result.add_argument("--antialiasing", type=int, choices=(0, 1), default=0)
    result.add_argument("--model-smoothing", action="store_true")
    result.add_argument("--paced", action="store_true", help="Use real-time pacing instead of measuring uncapped throughput")
    result.add_argument("--tag", help="Unused output prefix containing letters, digits, underscores or hyphens")
    result.add_argument("--powershell", help="PowerShell executable; defaults to powershell.exe, then pwsh")
    result.add_argument("--dry-run", action="store_true", help="Validate inputs and print the plan without launching or writing files")
    return result


def sha256(path: Path) -> str:
    with path.open("rb") as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_order(repeats: int):
    for pair in range(1, repeats + 1):
        for label in (("baseline", "candidate") if pair % 2 else ("candidate", "baseline")):
            yield pair, label


def build_command(args, powershell: str, script: Path, label: str, tag: str) -> list[str]:
    command = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
               "-Exe", str(getattr(args, label)), "-Game", str(args.game), "-Tag", tag,
               "-Frames", str(args.frames), "-PresentHz", str(args.present_hz),
               "-InternalScale", str(args.internal_scale), "-TextureFilter", str(args.texture_filter),
               "-Antialiasing", str(args.antialiasing), "-NoProfileCounters", "-NoFrameCapture"]
    if args.pad_sequence:
        command += ["-PadSequence", args.pad_sequence]
    if args.pad_sequence_file:
        command += ["-PadSequenceFile", str(args.pad_sequence_file)]
    if args.smpc_file:
        command += ["-SmpcFile", str(args.smpc_file)]
    if args.model_smoothing:
        command.append("-ModelSmoothing")
    if args.paced:
        command.append("-Paced")
    return command


def run_process(command: list[str], cwd: Path) -> int:
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    with subprocess.Popen(command, cwd=cwd, creationflags=flags) as process:
        try:
            return process.wait()
        except KeyboardInterrupt:
            # PowerShell owns a child runtime. Stop only this invocation's
            # process tree, so an interrupted comparison leaves no GPU job.
            if process.poll() is None:
                if sys.platform == "win32":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                else:
                    process.terminate()
                process.wait()
            raise


def validate_run(data: dict, expected: dict, binary_hash: str) -> None:
    if data.get("exit") != 0:
        raise ValueError(f"runtime exited with {data.get('exit')}")
    if data.get("exeChangedDuringRun") or data.get("sha256", "").lower() != binary_hash:
        raise ValueError("executable changed during the comparison")
    if data.get("halted") is not False or data.get("renderErrors") != []:
        raise ValueError("runtime halted, reported a rendering failure, or has no checked shutdown status")
    if not isinstance(data.get("finalPc"), str) or not re.fullmatch(r"0x[0-9A-Fa-f]{8}", data["finalPc"]):
        raise ValueError("runtime has no valid final PC")
    if not isinstance(data.get("masterCycles"), int) or data["masterCycles"] <= 0:
        raise ValueError("runtime has no valid final master-cycle count")
    if data.get("completedFields") is not None:
        if data["completedFields"] != expected["fields"]:
            raise ValueError(f"runtime completed {data['completedFields']} fields, expected {expected['fields']}")
        if data.get("completedCycles") != data["masterCycles"]:
            raise ValueError("completion marker and final master cycles disagree")
    for key, value in expected.items():
        actual = data.get(key)
        if key.endswith("Sha256") and isinstance(actual, str):
            actual = actual.lower()
        if actual != value:
            raise ValueError(f"run used unexpected {key}: {actual!r} (expected {value!r})")
    seconds = data.get("seconds")
    if not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("run has no valid wall-clock measurement")
    cpu = data.get("cpuSeconds")
    if cpu is not None and (not isinstance(cpu, (int, float)) or not math.isfinite(cpu) or cpu < 0):
        raise ValueError("run has an invalid process CPU measurement")


def summarize(runs: list[dict]) -> dict:
    result = {}
    for label in ("baseline", "candidate"):
        wall = [run["result"]["seconds"] for run in runs if run["label"] == label]
        cpu = [run["result"]["cpuSeconds"] for run in runs
               if run["label"] == label and run["result"].get("cpuSeconds") is not None]
        result[label] = {"runs": len(wall), "medianWallSeconds": statistics.median(wall),
                         "medianCpuSeconds": statistics.median(cpu) if cpu else None,
                         "cpuSamples": len(cpu)}
    baseline, candidate = result["baseline"], result["candidate"]
    result["wallSpeedup"] = baseline["medianWallSeconds"] / candidate["medianWallSeconds"]
    result["wallTimeReductionPercent"] = 100 * (1 - candidate["medianWallSeconds"] / baseline["medianWallSeconds"])
    paired = {}
    for run in runs:
        paired.setdefault(run["pair"], {})[run["label"]] = run["result"]["seconds"]
    result["medianPairedWallSpeedup"] = statistics.median(
        pair["baseline"] / pair["candidate"] for pair in paired.values())
    return result


def execute(args, root: Path = ROOT) -> Path | None:
    for name in ("baseline", "candidate", "game", "pad_sequence_file", "smpc_file"):
        value = getattr(args, name)
        if value is not None:
            value = value.resolve(strict=True)
            if not value.is_file():
                raise ValueError(f"{name} must be a file: {value}")
            setattr(args, name, value)
    tag = args.tag or f"pair-{datetime.now(timezone.utc):%Y%m%d-%H%M%S}-{uuid.uuid4().hex[:8]}"
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", tag):
        raise ValueError("tag must contain 1-80 letters, digits, underscores or hyphens")
    output = root / "out" / "performance"
    if output.exists() and any(output.glob(f"{tag}-*")):
        raise ValueError(f"output prefix already exists; choose another --tag: {tag}")
    powershell = args.powershell or shutil.which("powershell.exe") or shutil.which("pwsh")
    if not powershell:
        raise ValueError("PowerShell was not found; supply --powershell")
    binaries = {name: {"path": str(getattr(args, name)), "sha256": sha256(getattr(args, name))}
                for name in ("baseline", "candidate")}
    game_hash = sha256(args.game)
    seed = args.smpc_file.read_bytes() if args.smpc_file else DEFAULT_SMPC
    input_bytes = args.pad_sequence_file.read_bytes() if args.pad_sequence_file else args.pad_sequence.encode("utf-8")
    expected = {"fields": args.frames, "gameSha256": game_hash,
                "smpcSeedSha256": hashlib.sha256(seed).hexdigest(),
                "padSequenceSha256": hashlib.sha256(input_bytes).hexdigest(),
                "presentHz": args.present_hz, "internalScale": args.internal_scale,
                "textureFilter": args.texture_filter, "antialiasing": args.antialiasing,
                "modelSmoothing": args.model_smoothing, "uncapped": not args.paced,
                "profileCounters": False, "audioCapture": False, "frameCapture": False,
                "physicalAudio": "dummy", "dsp": "enabled"}
    report = {"schemaVersion": 1, "tag": tag, "status": "planned", "repeats": args.repeats,
              "startedUtc": datetime.now(timezone.utc).isoformat(), "binaries": binaries,
              "game": str(args.game), "padSequence": args.pad_sequence,
              "padSequenceFile": str(args.pad_sequence_file) if args.pad_sequence_file else None,
              "padSequenceSha256": hashlib.sha256(input_bytes).hexdigest(),
              "smpcSeedSource": str(args.smpc_file) if args.smpc_file else "default",
              "settings": expected, "runs": []}
    script = root / "tools" / "benchmark_runtime.ps1"
    plan = [(pair, label, f"{tag}-r{pair:02d}-{label}") for pair, label in run_order(args.repeats)]
    if args.dry_run:
        report["commands"] = [build_command(args, powershell, script, label, run_tag)
                              for _, label, run_tag in plan]
        print(json.dumps(report, indent=2))
        return None
    output.mkdir(parents=True, exist_ok=True)
    # Freeze input and console seeds once; each runtime receives its own copy
    # of the same initial console state, independent of earlier runs.
    args.smpc_file = output / f"{tag}-seed.bin"
    args.smpc_file.write_bytes(seed)
    if args.pad_sequence_file:
        args.pad_sequence_file = output / f"{tag}-inputs.txt"
        args.pad_sequence_file.write_bytes(input_bytes)
    summary = output / f"{tag}-summary.json"
    def save_report():
        summary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    report["status"] = "running"
    save_report()
    try:
        for pair, label, run_tag in plan:
            if sha256(args.game) != game_hash or sha256(getattr(args, label)) != binaries[label]["sha256"]:
                raise ValueError("game configuration or executable changed between runs")
            print(f"Pair {pair}/{args.repeats}: {label}", flush=True)
            report["activeRun"] = {"pair": pair, "label": label, "tag": run_tag}
            save_report()
            code = run_process(build_command(args, powershell, script, label, run_tag), root)
            result_path = output / f"{run_tag}.json"
            if code != 0:
                raise ValueError(f"{label} run failed with exit {code}; inspect {run_tag}.err")
            # Accept UTF-8 both with and without Windows PowerShell's BOM.
            with result_path.open(encoding="utf-8-sig") as source:
                data = json.load(source)
            if sha256(args.game) != game_hash:
                raise ValueError("game configuration changed during the run")
            validate_run(data, expected, binaries[label]["sha256"])
            terminal = {"finalPc": f"0x{int(data['finalPc'], 16):08X}", "masterCycles": data["masterCycles"]}
            if report.get("terminalState", terminal) != terminal:
                raise ValueError(f"different final guest work: {terminal}; expected {report['terminalState']}")
            report["terminalState"] = terminal
            report["runs"].append({"pair": pair, "label": label, "resultFile": str(result_path), "result": data})
            del report["activeRun"]
            save_report()
        if not any(run["result"].get("completedFields") is not None for run in report["runs"]):
            raise ValueError("no run verified the requested field count; compare with at least one build that emits [runtime] fields")
        report["summary"] = summarize(report["runs"])
        report["legacyRunsWithoutFieldMarker"] = sum(run["result"].get("completedFields") is None for run in report["runs"])
        report["status"] = "complete"
    except (Exception, KeyboardInterrupt) as error:
        report["status"] = "failed"
        report["error"] = str(error) or "Interrupted"
        raise
    finally:
        report["finishedUtc"] = datetime.now(timezone.utc).isoformat()
        save_report()
    metrics = report["summary"]
    print(f"Median wall: baseline {metrics['baseline']['medianWallSeconds']:.3f}s, "
          f"candidate {metrics['candidate']['medianWallSeconds']:.3f}s "
          f"({metrics['wallSpeedup']:.3f}x baseline/candidate)")
    print(f"Results: {summary}")
    return summary


def main() -> int:
    try:
        execute(parser().parse_args())
        return 0
    except (OSError, ValueError, KeyboardInterrupt) as error:
        print(f"Benchmark failed: {error or 'Interrupted'}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
