"""Call every Orbit MCP tool and backing RPC method against disposable Studio state.

This is a manual, real-GPU Windows smoke test, not a CTest unit test:
    python tools/tests/mcp_live_smoke.py --exe Orbit.exe
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import queue
import sqlite3
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "examples" / "mcp-smoke"
ADAPTER = ROOT / "tools" / "mcp_server" / "orbit_editor_mcp_server.py"


def mcp_catalog() -> tuple[list[str], list[str]]:
    tree = ast.parse(ADAPTER.read_text(encoding="utf-8"))
    tools: list[str] = []
    methods: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if any(
                isinstance(decorator, ast.Call)
                and isinstance(decorator.func, ast.Attribute)
                and isinstance(decorator.func.value, ast.Name)
                and decorator.func.value.id == "mcp"
                and decorator.func.attr == "tool"
                for decorator in node.decorator_list
            ):
                tools.append(node.name)
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "_rpc"
            and node.args
            and isinstance(node.args[0], ast.Constant)
            and isinstance(node.args[0].value, str)
        ):
            methods.add(node.args[0].value)
    return sorted(tools), sorted(methods)


def free_port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def validate_fixture() -> None:
    manifest = FIXTURE / "Project.orbit.toml"
    if not manifest.is_file():
        raise RuntimeError(f"Example project manifest is missing: {manifest}")
    world = FIXTURE / "Worlds" / "Main.orbitworld"
    with sqlite3.connect(world) as database:
        version = database.execute("SELECT version FROM orbit_schema").fetchone()
        world_id = database.execute(
            "SELECT value FROM world_metadata WHERE key='world_id'"
        ).fetchone()
        names = [
            row[0]
            for row in database.execute("SELECT name FROM objects")
        ]
        types = {
            row[0]
            for row in database.execute("SELECT type_id FROM objects")
        }
    required_names = {
        "MCP Smoke System", "Asterra Primary", "Asterra", "Luna",
        "Atmosphere", "Cloud Layer", "Ocean", "Photosphere",
        "Radiative Emitter", "Terrain Surface",
    }
    required_types = {
        "4f524249-5454-4552-5241-494e00000001",  # Terrain Surface
        "4f524249-5443-454c-4f43-4e4341500001",  # Ocean capability
    }
    if (
        version != (2,)
        or not world_id
        or not required_names.issubset(set(names))
        or not required_types.issubset(types)
        or names.count("Terrain Surface") < 2
    ):
        raise RuntimeError(f"Example world is incomplete or invalid: {world}")


def read_lines(stream: Any, output: queue.Queue[str | None], log_path: Path | None = None) -> None:
    try:
        for line in iter(stream.readline, ""):
            if log_path is not None:
                with log_path.open("a", encoding="utf-8") as log:
                    log.write(line)
            output.put(line)
    finally:
        output.put(None)


class McpStdioClient:
    def __init__(self, process: subprocess.Popen[str], timeout: float = 15.0) -> None:
        self.process = process
        self.timeout = timeout
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.reader = threading.Thread(
            target=read_lines, args=(process.stdout, self.lines), daemon=True
        )
        self.reader.start()
        self.next_id = 1

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        message: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            message["params"] = params
        assert self.process.stdin is not None
        self.process.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty as error:
                raise TimeoutError(f"MCP {method} timed out") from error
            if line is None:
                raise RuntimeError(f"MCP adapter exited during {method}")
            response = json.loads(line)
            if response.get("id") == request_id:
                if "error" in response:
                    raise RuntimeError(f"MCP {method} protocol error: {response['error']}")
                return response.get("result", {})
        raise TimeoutError(f"MCP {method} timed out")

    def notify(self, method: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(
            json.dumps({"jsonrpc": "2.0", "method": method}, separators=(",", ":"))
            + "\n"
        )
        self.process.stdin.flush()


def rpc_call(port: int, method: str, params: dict[str, Any], timeout: float = 15.0) -> dict[str, Any]:
    request_id = str(uuid.uuid4())
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params},
        separators=(",", ":"),
    ).encode("utf-8") + b"\n"
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.settimeout(timeout)
        client.sendall(payload)
        buffer = bytearray()
        while b"\n" not in buffer:
            chunk = client.recv(65536)
            if not chunk:
                raise RuntimeError(f"Studio closed RPC connection during {method}")
            buffer.extend(chunk)
        for line in bytes(buffer).splitlines():
            response = json.loads(line)
            if response.get("id") == request_id:
                return response
    raise RuntimeError(f"Studio did not reply to {method}")


def close_window_for_pid(pid: int) -> None:
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    windows: list[int] = []

    def visit(hwnd: int, _lparam: int) -> bool:
        window_pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(window_pid))
        if window_pid.value == pid and user32.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True

    user32.EnumWindows(enum_callback(visit), 0)
    for hwnd in windows:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Orbit.exe")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--keep-run", action="store_true")
    args = parser.parse_args()
    executable = args.exe.resolve()
    if not executable.is_file():
        parser.error(f"Studio executable not found: {executable}")
    validate_fixture()

    tools, methods = mcp_catalog()
    run_root = ROOT / "build" / "mcp-live-smoke" / uuid.uuid4().hex
    project_root = run_root / "project"
    local_data = run_root / "localappdata"
    run_root.mkdir(parents=True)
    shutil.copytree(FIXTURE, project_root)
    port = free_port()
    environment = os.environ.copy()
    environment.update(
        {
            "LOCALAPPDATA": str(local_data),
            "ORBIT_RPC_HOST": "127.0.0.1",
            "ORBIT_RPC_PORT": str(port),
            "ORBIT_RPC_TIMEOUT": str(args.timeout),
        }
    )
    studio_log = run_root / "studio.log"
    adapter_log = run_root / "mcp-adapter.log"
    studio_output = studio_log.open("w", encoding="utf-8")
    studio = subprocess.Popen(
        [str(executable), str(project_root / "Project.orbit.toml")],
        cwd=ROOT,
        env=environment,
        stdout=studio_output,
        stderr=subprocess.STDOUT,
        text=True,
    )
    adapter: subprocess.Popen[str] | None = None
    failures: list[str] = []
    mcp_calls = 0
    rpc_calls = 0
    mcp_error_results = 0
    rpc_error_results = 0
    try:
        deadline = time.monotonic() + 60.0
        while time.monotonic() < deadline:
            if studio.poll() is not None:
                raise RuntimeError(f"Studio exited during startup ({studio.returncode}); see {studio_log}")
            try:
                info = rpc_call(port, "project.info", {}, timeout=1.0)
                if "result" in info:
                    expected = str(project_root).replace("\\", "/").lower()
                    actual = str(info["result"].get("root", "")).replace("\\", "/").lower()
                    if actual != expected:
                        raise RuntimeError(f"Studio opened unexpected project root: {actual}")
                    break
            except (OSError, TimeoutError, RuntimeError):
                time.sleep(0.25)
        else:
            raise TimeoutError(f"Studio RPC did not become ready; see {studio_log}")

        adapter = subprocess.Popen(
            [sys.executable, str(ADAPTER)],
            cwd=ROOT,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        threading.Thread(
            target=read_lines, args=(adapter.stderr, queue.Queue(), adapter_log), daemon=True
        ).start()
        client = McpStdioClient(adapter, args.timeout)
        client.request(
            "initialize",
            {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "orbit-live-smoke", "version": "1.0"},
            },
        )
        client.notify("notifications/initialized")
        listed = client.request("tools/list")
        advertised = listed.get("tools", [])
        advertised_names = {tool["name"] for tool in advertised}
        if advertised_names != set(tools):
            raise RuntimeError(
                f"MCP tool catalog mismatch: source={len(tools)}, live={len(advertised_names)}"
            )

        print(f"Calling {len(advertised)} MCP tools and {len(methods)} backing RPC methods.")
        # Empty argument objects intentionally exercise required-argument error
        # paths as well as no-argument/defaulted operations. All mutations land
        # in the copied fixture and its disposable app-data directory.
        ordered_tools = sorted(
            advertised,
            key=lambda tool: (tool["name"] == "orbit_world_close", tool["name"]),
        )
        for tool in ordered_tools:
            if studio.poll() is not None:
                failures.append(f"Studio exited before MCP tool {tool['name']}")
                break
            try:
                result = client.request(
                    "tools/call",
                    {"name": tool["name"], "arguments": {}},
                )
            except Exception as error:  # protocol/transport failure is not an expected tool error result
                failures.append(f"MCP {tool['name']}: {error}")
                break
            mcp_calls += 1
            if result.get("isError"):
                mcp_error_results += 1

        # The wrappers' own input-schema validator can reject missing required
        # arguments before RPC. Exercise every unique engine dispatch method as
        # well, accepting structured JSON-RPC errors but never a dropped link.
        for method in methods:
            if studio.poll() is not None:
                failures.append(f"Studio exited before RPC method {method}")
                break
            try:
                response = rpc_call(port, method, {}, timeout=args.timeout)
            except Exception as error:
                failures.append(f"RPC {method}: {error}")
                break
            rpc_calls += 1
            if "error" in response:
                rpc_error_results += 1

        if studio.poll() is not None:
            failures.append(f"Studio exited during smoke sweep ({studio.returncode})")
    except Exception as error:
        failures.append(str(error))
    finally:
        if adapter is not None:
            adapter.terminate()
            try:
                adapter.wait(timeout=5)
            except subprocess.TimeoutExpired:
                adapter.kill()
        close_window_for_pid(studio.pid)
        try:
            studio.wait(timeout=10)
        except subprocess.TimeoutExpired:
            studio.terminate()
            try:
                studio.wait(timeout=5)
            except subprocess.TimeoutExpired:
                studio.kill()
        studio_output.close()
        if args.keep_run or failures:
            print(f"Run data retained at {run_root}")
        else:
            shutil.rmtree(run_root, ignore_errors=True)

    if failures:
        print("MCP smoke failed:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(
        f"Passed: {mcp_calls} MCP tools ({mcp_error_results} expected tool errors), "
        f"{rpc_calls} RPC methods ({rpc_error_results} expected RPC errors); "
        "Studio stayed responsive."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
