#!/usr/bin/env python3
"""Non-blocking process bridge used by the Godot in-game training console.

This script intentionally depends only on Python's standard library. Godot launches
it with the selected training Python interpreter, then polls the status JSON and log
files while the bridge executes repo-owned build/test/Isaac/training commands.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Sequence


def repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def experiment_root() -> Path:
    return repo_root() / "experiments" / "locomotion_19body"


def atomic_write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(temp, path)


def append_log(path: Path, line: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", buffering=1) as handle:
        handle.write(line.rstrip() + "\n")


def command_text(command: Sequence[str]) -> str:
    return subprocess.list2cmdline([str(part) for part in command])


class Bridge:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.root = repo_root()
        self.exp = experiment_root()
        self.status_path = args.status.expanduser().resolve()
        self.log_path = args.log.expanduser().resolve()
        self.cancel_path = args.cancel.expanduser().resolve()
        self.started = time.time()
        self.step_started = self.started
        self.step_index = 0
        self.steps: list[tuple[str, list[str]]] = []
        self.current_child: subprocess.Popen[str] | None = None
        self._training_runs_before: set[Path] = set()

    def status(
        self,
        state: str,
        message: str,
        *,
        return_code: int | None = None,
        current_step: str = "",
    ) -> None:
        atomic_write_json(
            self.status_path,
            {
                "schema_version": 1,
                "task": self.args.task,
                "state": state,
                "message": message,
                "pid": os.getpid(),
                "python": sys.executable,
                "started_unix": self.started,
                "updated_unix": time.time(),
                "current_step": current_step,
                "step_index": self.step_index,
                "step_count": len(self.steps),
                "return_code": return_code,
                "log_path": str(self.log_path),
                "cancel_path": str(self.cancel_path),
            },
        )

    def canceled(self) -> bool:
        return self.cancel_path.exists()

    def _smoke_mode_from_step(self, name: str) -> str | None:
        prefix = "PhysX smoke "
        if not name.startswith(prefix):
            return None
        mode = name[len(prefix) :].strip()
        return mode if mode in ("hold", "passive") else None

    def _smoke_report_path(self, mode: str) -> Path:
        return self.exp / "runs" / "smoke" / f"latest_{mode}.json"

    def _stand_training_root(self) -> Path:
        return self.exp / "runs" / "training" / "stand"

    def _stand_manifests(self) -> set[Path]:
        root = self._stand_training_root()
        if not root.is_dir():
            return set()
        return {path.resolve() for path in root.glob("*/run_manifest.json") if path.is_file()}

    def _prepare_step_outputs(self, name: str) -> None:
        mode = self._smoke_mode_from_step(name)
        if mode is not None:
            report = self._smoke_report_path(mode)
            if report.exists():
                report.unlink()
        if name == "train foundation: stand":
            self._training_runs_before = self._stand_manifests()

    def _validate_smoke_report(self, name: str) -> tuple[bool, str]:
        mode = self._smoke_mode_from_step(name)
        if mode is None:
            return True, ""
        report_path = self._smoke_report_path(mode)
        if not report_path.is_file():
            return False, f"Smoke child returned 0 but did not create {report_path}"
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except Exception as exc:
            return False, f"Smoke report is unreadable: {type(exc).__name__}: {exc}"
        if report.get("status") != "pass":
            return False, f"Smoke report status is {report.get('status')!r}, expected 'pass'"
        if report.get("mode") != mode:
            return False, f"Smoke report mode is {report.get('mode')!r}, expected {mode!r}"
        return True, ""

    def _validate_stand_training(self, name: str) -> tuple[bool, str]:
        if name != "train foundation: stand":
            return True, ""
        current = self._stand_manifests()
        new_manifests = list(current - self._training_runs_before)
        if not new_manifests:
            return False, "Training child returned 0 but created no new stand run manifest"
        newest = max(new_manifests, key=lambda path: path.stat().st_mtime)
        try:
            report = json.loads(newest.read_text(encoding="utf-8"))
        except Exception as exc:
            return False, f"Stand run manifest is unreadable: {type(exc).__name__}: {exc}"
        status = report.get("status")
        if status != "complete":
            failure = report.get("failed_exception")
            detail = f" ({failure})" if failure else ""
            return False, f"Stand run ended with manifest status {status!r}, expected 'complete'{detail}"
        if int(report.get("max_iterations", -1)) != int(self.args.max_iterations):
            return False, (
                f"Stand run manifest iteration target is {report.get('max_iterations')!r}, "
                f"expected {self.args.max_iterations}"
            )
        append_log(self.log_path, f"Validated completed stand run: {newest.parent}")
        return True, ""

    def _validate_step_result(self, name: str) -> tuple[bool, str]:
        valid, reason = self._validate_smoke_report(name)
        if not valid:
            return valid, reason
        return self._validate_stand_training(name)

    def run_step(self, name: str, command: list[str]) -> int:
        self.step_index += 1
        self.step_started = time.time()
        self.status("running", f"Running {name}", current_step=name)
        append_log(self.log_path, "")
        append_log(self.log_path, f"===== [{self.step_index}/{len(self.steps)}] {name} =====")
        append_log(self.log_path, command_text(command))
        self._prepare_step_outputs(name)

        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with self.log_path.open("a", encoding="utf-8", buffering=1) as output:
            self.current_child = subprocess.Popen(
                command,
                cwd=self.root,
                stdout=output,
                stderr=subprocess.STDOUT,
                text=True,
                env=env,
            )
            while True:
                code = self.current_child.poll()
                if code is not None:
                    self.current_child = None
                    code = int(code)
                    if code == 0:
                        valid, reason = self._validate_step_result(name)
                        if not valid:
                            append_log(self.log_path, f"RESULT INVALID: {reason}")
                            code = 1
                    append_log(self.log_path, f"===== {name}: exit {code} =====")
                    return code
                if self.canceled():
                    append_log(self.log_path, f"Cancellation requested during {name}")
                    self.current_child.terminate()
                    try:
                        self.current_child.wait(timeout=8.0)
                    except subprocess.TimeoutExpired:
                        self.current_child.kill()
                        self.current_child.wait(timeout=5.0)
                    self.current_child = None
                    return 130
                time.sleep(0.20)

    def smoke_command(self, mode: str) -> list[str]:
        script = self.exp / "training" / "scripts" / "smoke_sim.py"
        command = [
            sys.executable,
            str(script),
            "--num-envs",
            str(self.args.num_envs),
            "--mode",
            mode,
            "--seconds",
            str(self.args.seconds),
            "--seed",
            str(self.args.seed),
            "--joint-noise-deg",
            str(self.args.joint_noise_deg),
        ]
        if self.args.headless:
            command.append("--headless")
        if self.args.device:
            command.extend(["--device", self.args.device])
        if self.args.strict_contact or (mode == "passive" and self.args.task == "preflight"):
            command.append("--strict-contact")
        return command

    def stand_training_command(self) -> list[str]:
        script = self.exp / "training" / "train_foundation.py"
        command = [
            sys.executable,
            str(script),
            "--stage",
            "stand",
            "--num-envs",
            str(self.args.num_envs),
            "--max-iterations",
            str(self.args.max_iterations),
            "--seed",
            str(self.args.seed),
        ]
        if self.args.headless:
            command.append("--headless")
        if self.args.device:
            command.extend(["--device", self.args.device])
        return command

    def build_steps(self) -> list[tuple[str, list[str]]]:
        scripts = self.exp / "training" / "scripts"
        build = [sys.executable, str(scripts / "build_articulation.py")]
        test_build = [sys.executable, str(scripts / "test_build_articulation.py")]
        test_helpers = [sys.executable, str(scripts / "test_articulation_helpers.py")]
        test_foundation = [sys.executable, str(scripts / "test_foundation_policy.py")]
        check_stack = [sys.executable, str(self.exp / "training" / "check_training_stack.py")]
        convert = [sys.executable, str(scripts / "convert_training_asset.py")]
        if self.args.headless:
            convert.append("--headless")
        if self.args.force:
            convert.append("--force")
        if self.args.device:
            convert.extend(["--device", self.args.device])

        task = self.args.task
        if task == "check_stack":
            return [("check training stack", check_stack)]
        if task == "build":
            return [("build articulation", build)]
        if task == "tests":
            return [
                ("articulation builder tests", test_build),
                ("articulation helper tests", test_helpers),
                ("foundation tensor tests", test_foundation),
            ]
        if task == "convert":
            return [("convert URDF to USD", convert)]
        if task == "smoke":
            return [(f"PhysX smoke {self.args.mode}", self.smoke_command(self.args.mode))]
        if task == "train_stand":
            return [("train foundation: stand", self.stand_training_command())]
        if task == "preflight":
            return [
                ("check training stack", check_stack),
                ("build articulation", build),
                ("articulation builder tests", test_build),
                ("articulation helper tests", test_helpers),
                ("foundation tensor tests", test_foundation),
                ("convert URDF to USD", convert),
                ("PhysX smoke hold", self.smoke_command("hold")),
                ("PhysX smoke passive", self.smoke_command("passive")),
            ]
        raise ValueError(f"Unsupported task: {task}")

    def run(self) -> int:
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path.write_text("", encoding="utf-8")
        if self.cancel_path.exists():
            self.cancel_path.unlink()
        self.steps = self.build_steps()
        self.status("running", "Bridge started")
        append_log(self.log_path, f"Asterra in-game training bridge | task={self.args.task}")
        append_log(self.log_path, f"python={sys.executable}")
        append_log(self.log_path, f"repo={self.root}")

        for name, command in self.steps:
            if self.canceled():
                self.status("canceled", "Canceled before next step", return_code=130, current_step=name)
                return 130
            code = self.run_step(name, command)
            if code == 130 and self.canceled():
                self.status("canceled", f"Canceled during {name}", return_code=130, current_step=name)
                return 130
            if code != 0:
                self.status(
                    "failed",
                    f"{name} failed with exit code {code}",
                    return_code=code,
                    current_step=name,
                )
                return code

        self.status("passed", "All requested steps passed", return_code=0)
        return 0


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Asterra Godot training-process bridge")
    parser.add_argument(
        "--task",
        choices=("check_stack", "build", "tests", "convert", "smoke", "preflight", "train_stand"),
        required=True,
    )
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--cancel", type=Path, required=True)
    parser.add_argument("--mode", choices=("hold", "passive"), default="hold")
    parser.add_argument("--num-envs", type=int, default=256)
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--max-iterations", type=int, default=1500)
    parser.add_argument("--seed", type=int, default=1467)
    parser.add_argument("--joint-noise-deg", type=float, default=0.0)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--strict-contact", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args(argv)
    if args.num_envs < 1:
        parser.error("--num-envs must be >= 1")
    if args.seconds <= 0.0:
        parser.error("--seconds must be > 0")
    if args.max_iterations < 1:
        parser.error("--max-iterations must be >= 1")
    if args.joint_noise_deg < 0.0:
        parser.error("--joint-noise-deg must be >= 0")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    bridge = Bridge(args)
    try:
        return bridge.run()
    except KeyboardInterrupt:
        bridge.status("canceled", "Interrupted", return_code=130)
        return 130
    except Exception as exc:
        append_log(bridge.log_path, f"BRIDGE ERROR: {type(exc).__name__}: {exc}")
        bridge.status(
            "failed",
            f"Bridge error: {type(exc).__name__}: {exc}",
            return_code=1,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
