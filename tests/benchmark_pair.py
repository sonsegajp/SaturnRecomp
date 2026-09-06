"""No-game tests for paired benchmark orchestration and measurement validity."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("paired_runtime_tool", Path(__file__).resolve().parents[1] / "tools/benchmark_pair.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class BenchmarkPairTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="saturn-pair-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("baseline.exe", "candidate.exe", "game with spaces.toml"):
            (self.root / name).write_text(name)
        self.base = ["--baseline", str(self.root / "baseline.exe"),
                     "--candidate", str(self.root / "candidate.exe"),
                     "--game", str(self.root / "game with spaces.toml"),
                     "--tag", "case", "--powershell", "powershell.exe"]

    def args(self, *extra):
        return benchmark.parser().parse_args(self.base + list(extra))

    def test_alternates_complete_pairs(self):
        self.assertEqual(list(benchmark.run_order(3)), [
            (1, "baseline"), (1, "candidate"), (2, "candidate"),
            (2, "baseline"), (3, "baseline"), (3, "candidate")])

    def test_rejects_invalid_ranges_and_conflicting_inputs(self):
        for options in (("--repeats", "0"), ("--repeats", "21"), ("--frames", "0"),
                        ("--present-hz", "59"), ("--internal-scale", "5"),
                        ("--pad-sequence", "1:0", "--pad-sequence-file", "inputs.txt")):
            with self.subTest(options=options), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    self.args(*options)
                self.assertEqual(failure.exception.code, 2)

    def test_command_keeps_arguments_separate_and_disables_instrumentation(self):
        args = self.args("--pad-sequence", "1000:8,1100:0", "--present-hz", "120", "--model-smoothing")
        command = benchmark.build_command(args, "PowerShell with spaces.exe", self.root / "script.ps1", "candidate", "test-tag")
        self.assertEqual(command[0], "PowerShell with spaces.exe")
        self.assertEqual(command[command.index("-Game") + 1], str(args.game))
        self.assertEqual(command[command.index("-PadSequence") + 1], "1000:8,1100:0")
        self.assertIn("-NoProfileCounters", command)
        self.assertIn("-NoFrameCapture", command)
        self.assertNotIn("-CaptureAudio", command)
        self.assertIn("-ModelSmoothing", command)

    def test_median_resists_outlier_and_counts_optional_cpu_samples(self):
        runs = []
        for pair, (baseline, candidate) in enumerate(((10, 5), (100, 7), (12, 6)), 1):
            runs += [{"pair": pair, "label": "baseline", "result": {"seconds": baseline, "cpuSeconds": None if pair == 2 else pair}},
                     {"pair": pair, "label": "candidate", "result": {"seconds": candidate, "cpuSeconds": None}}]
        result = benchmark.summarize(runs)
        self.assertEqual(result["baseline"]["medianWallSeconds"], 12)
        self.assertEqual(result["candidate"]["medianWallSeconds"], 6)
        self.assertEqual(result["wallSpeedup"], 2)
        self.assertEqual(result["medianPairedWallSpeedup"], 2)
        self.assertEqual(result["baseline"]["cpuSamples"], 2)
        self.assertEqual(result["baseline"]["medianCpuSeconds"], 2)
        self.assertIsNone(result["candidate"]["medianCpuSeconds"])

    def test_refuses_failed_mismatched_or_invalid_measurements(self):
        valid = {"exit": 0, "sha256": "abc", "seconds": 1.0, "cpuSeconds": None,
                 "profileCounters": False, "audioCapture": False, "frameCapture": False,
                 "halted": False, "renderErrors": [], "finalPc": "0x06001234", "masterCycles": 100,
                 "fields": 5, "completedFields": 5, "completedCycles": 100}
        expected = {"profileCounters": False, "audioCapture": False, "frameCapture": False, "fields": 5}
        benchmark.validate_run(valid, expected, "abc")
        benchmark.validate_run(dict(valid, completedFields=None, completedCycles=None), expected, "abc")
        for bad in ({"exit": 7}, {"profileCounters": True}, {"audioCapture": True},
                    {"frameCapture": True}, {"sha256": "def"}, {"exeChangedDuringRun": True},
                    {"seconds": 0}, {"seconds": float("nan")}, {"cpuSeconds": -1},
                    {"halted": True}, {"renderErrors": ["[video] Vulkan frame failed"]},
                    {"finalPc": None}, {"masterCycles": None}, {"completedFields": 4}, {"completedCycles": 101}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                benchmark.validate_run(dict(valid, **bad), expected, "abc")

    def test_powershell_halt_parser_recognizes_actual_slave_marker(self):
        shell = shutil.which("powershell.exe") or shutil.which("pwsh")
        if not shell:
            self.skipTest("PowerShell is unavailable")
        script = str(Path(__file__).resolve().parents[1] / "tools/benchmark_runtime.ps1").replace("'", "''")
        command = """
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile('__SCRIPT__',[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Benchmark PowerShell syntax error'}
$assignment=$ast.Find({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$halted'},$true)
if(-not $assignment){throw 'Halt-status parser missing'}
$check=[scriptblock]::Create($assignment.Extent.Text)
foreach($case in @(@('normal footer','[slave halt] illegal instruction',$true),@('HALTED: invalid opcode','',$true),@('normal footer','[runtime] fields 5 cycles 10',$false))){
    $stdoutText=$case[0];$stderrText=$case[1]
    . $check
    if($halted -ne $case[2]){throw 'Incorrect halt status'}
}
""".replace("__SCRIPT__", script)
        result = subprocess.run([shell, "-NoProfile", "-Command", command], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def fake_runner(self, calls, fail_at=None, mutate_game=False):
        def run(command, root):
            calls.append(command)
            if len(calls) == fail_at:
                return 7
            report = json.loads((root / "out/performance/case-summary.json").read_text())
            tag = command[command.index("-Tag") + 1]
            label = "candidate" if tag.endswith("candidate") else "baseline"
            result = dict(report["settings"], exit=0, sha256=report["binaries"][label]["sha256"],
                          seconds=4 if label == "baseline" else 2, cpuSeconds=1,
                          halted=False, renderErrors=[], finalPc="0x06001234", masterCycles=5000000,
                          completedFields=None if label == "baseline" else report["settings"]["fields"],
                          completedCycles=None if label == "baseline" else 5000000)
            (root / "out/performance" / f"{tag}.json").write_text(json.dumps(result))
            if mutate_game:
                (root / "game with spaces.toml").write_text("modified while benchmarking")
            return 0
        return run

    def test_complete_comparison_writes_order_settings_hashes_and_medians(self):
        calls = []
        with mock.patch.object(benchmark, "run_process", self.fake_runner(calls)), contextlib.redirect_stdout(io.StringIO()):
            path = benchmark.execute(self.args(), self.root)
        report = json.loads(path.read_text())
        self.assertEqual(report["status"], "complete")
        self.assertEqual([(run["pair"], run["label"]) for run in report["runs"]], list(benchmark.run_order(3)))
        self.assertEqual(report["summary"]["wallSpeedup"], 2)
        self.assertEqual(report["legacyRunsWithoutFieldMarker"], 3)
        self.assertFalse(report["settings"]["profileCounters"])
        self.assertEqual(len(report["binaries"]["baseline"]["sha256"]), 64)
        self.assertEqual((self.root / "out/performance/case-seed.bin").read_bytes(), benchmark.DEFAULT_SMPC)
        self.assertEqual(len(calls), 6)

    def test_failure_preserves_partial_evidence_without_reporting_a_gain(self):
        calls = []
        with mock.patch.object(benchmark, "run_process", self.fake_runner(calls, fail_at=3)), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "exit 7"):
                benchmark.execute(self.args(), self.root)
        report = json.loads((self.root / "out/performance/case-summary.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(report["runs"]), 2)
        self.assertEqual(report["activeRun"]["label"], "candidate")
        self.assertEqual(report["activeRun"]["pair"], 2)
        self.assertNotIn("summary", report)

    def test_two_legacy_builds_cannot_validate_matching_early_exits(self):
        calls = []
        run = self.fake_runner(calls)
        def legacy_only(command, root):
            code = run(command, root)
            tag = command[command.index("-Tag") + 1]
            path = root / "out/performance" / f"{tag}.json"
            data = json.loads(path.read_text())
            data["completedFields"] = data["completedCycles"] = None
            path.write_text(json.dumps(data))
            return code
        with mock.patch.object(benchmark, "run_process", legacy_only), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "no run verified the requested field count"):
                benchmark.execute(self.args(), self.root)
        report = json.loads((self.root / "out/performance/case-summary.json").read_text())
        self.assertEqual(len(report["runs"]), 6)
        self.assertEqual(report["status"], "failed")
        self.assertNotIn("summary", report)

    def test_changed_game_configuration_stops_before_the_next_run(self):
        calls = []
        with mock.patch.object(benchmark, "run_process", self.fake_runner(calls, mutate_game=True)), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "game configuration changed"):
                benchmark.execute(self.args(), self.root)
        self.assertEqual(len(calls), 1)

    def test_different_final_guest_work_cannot_produce_a_speedup(self):
        calls = []
        run = self.fake_runner(calls)
        def different_work(command, root):
            code = run(command, root)
            if len(calls) == 2:
                path = root / "out/performance/case-r01-candidate.json"
                data = json.loads(path.read_text())
                data["masterCycles"] = data["completedCycles"] = 4999999
                path.write_text(json.dumps(data))
            return code
        with mock.patch.object(benchmark, "run_process", different_work), contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(ValueError, "different final guest work"):
                benchmark.execute(self.args(), self.root)
        report = json.loads((self.root / "out/performance/case-summary.json").read_text())
        self.assertEqual(report["status"], "failed")
        self.assertNotIn("summary", report)

    def test_dry_run_creates_no_outputs_and_launches_nothing(self):
        with mock.patch.object(benchmark, "run_process") as launch, contextlib.redirect_stdout(io.StringIO()) as text:
            self.assertIsNone(benchmark.execute(self.args("--dry-run"), self.root))
        self.assertEqual(len(json.loads(text.getvalue())["commands"]), 6)
        launch.assert_not_called()
        self.assertFalse((self.root / "out").exists())

    def test_existing_prefix_and_path_escape_are_rejected(self):
        directory = self.root / "out/performance"
        directory.mkdir(parents=True)
        (directory / "case-r01-baseline.json").write_text("prior evidence")
        with mock.patch.object(benchmark, "run_process") as launch:
            with self.assertRaisesRegex(ValueError, "already exists"):
                benchmark.execute(self.args(), self.root)
            with self.assertRaisesRegex(ValueError, "tag must"):
                benchmark.execute(self.args("--tag", "../escape"), self.root)
            launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
