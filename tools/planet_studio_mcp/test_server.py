"""Run with python -B -m unittest discover -s tools/planet_studio_mcp -v."""
import json
import socket
import subprocess
import sys
import threading
import unittest
from pathlib import Path

import server


class FakeBridge:
    def __init__(self):
        self.calls = []

    def call(self, name, arguments):
        self.calls.append((name, arguments))
        return {"ok": True}


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.bridge = FakeBridge()
        self.server = server.Server(self.bridge)

    def request(self, method, params=None):
        return self.server.dispatch({"jsonrpc": "2.0", "id": 7, "method": method, "params": params or {}})

    def initialize(self):
        result = self.request("initialize", {"protocolVersion": "2099-01-01", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}})
        self.assertEqual(result["result"]["protocolVersion"], server.PROTOCOL_VERSION)
        self.server.dispatch({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def test_lifecycle(self):
        self.assertEqual(self.request("tools/list")["error"]["code"], -32002)
        self.initialize()
        tools = self.request("tools/list")["result"]["tools"]
        self.assertEqual(len(tools), 12)
        self.assertEqual(len({item["name"] for item in tools}), 12)
        self.assertEqual(self.request("ping")["result"], {})

    def test_unknown_and_malformed_requests(self):
        self.initialize()
        for value in ([], None, 4, {"jsonrpc": "1.0", "method": "ping"}):
            self.assertEqual(self.server.dispatch(value)["error"]["code"], -32600)
        self.assertEqual(self.request("unknown")["error"]["code"], -32601)
        self.assertEqual(self.request("tools/call", {"name": "eval"})["error"]["code"], -32602)
        self.assertIsNone(self.server.dispatch({"jsonrpc": "2.0", "method": "notifications/cancelled"}))

    def test_invalid_arguments_never_reach_game(self):
        self.initialize()
        cases = [
            ("studio_category", {"category": "SHELL"}),
            ("studio_control", {"id": "1", "action": "select", "extra": True}),
            ("studio_control", {"action": "press"}),
            ("studio_input", {"events": []}),
            ("studio_input", {"events": [{"type": "button", "button": 50}]}),
            ("studio_inspect", {"depth": True}),
            ("studio_inspect", {"depth": 7}),
            ("studio_status", []),
            ("studio_control", {"id": "1", "action": "set", "value": float("nan")}),
            ("studio_camera", {"action": "rotate", "degrees": [1, 2]}),
            ("studio_camera", {"action": "slew", "duration_s": 31}),
            ("studio_time", {"action": "rate", "rate": -1e8}),
            ("studio_view", {"action": "full_bright", "enabled": "yes"}),
        ]
        for name, arguments in cases:
            with self.subTest(name=name, arguments=arguments):
                result = self.request("tools/call", {"name": name, "arguments": arguments})
                self.assertTrue(result["result"]["isError"])
        self.assertEqual(self.bridge.calls, [])

    def test_unicode_and_tool_errors(self):
        self.initialize()
        arguments = {"id": "123", "action": "set", "value": "Planète océan"}
        self.request("tools/call", {"name": "studio_control", "arguments": arguments})
        self.assertEqual(self.bridge.calls[-1][1], arguments)
        self.bridge.call = lambda *_: {"error": "Stale control"}
        self.assertTrue(self.request("tools/call", {"name": "studio_ui"})["result"]["isError"])

    def test_image_result(self):
        self.initialize()
        self.bridge.call = lambda *_: {"image": "YWJj", "mimeType": "image/png", "capture": {"intent": "Inspect pole seam"}}
        result = self.request("tools/call", {"name": "studio_screenshot"})
        self.assertEqual(result["result"]["content"][0]["type"], "image")
        self.assertEqual(json.loads(result["result"]["content"][1]["text"])["capture"]["intent"], "Inspect pole seam")

    def test_stdio_framing_parse_error_and_notifications(self):
        messages = [b"{broken\n", json.dumps({"jsonrpc": "2.0", "id": "init", "method": "initialize", "params": {"protocolVersion": server.PROTOCOL_VERSION, "capabilities": {}, "clientInfo": {}}}).encode() + b"\n",
                    b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n',
                    b'{"jsonrpc":"2.0","id":2,"method":"tools/list"}\n']
        result = subprocess.run([sys.executable, "-B", str(Path(server.__file__))], input=b"".join(messages), capture_output=True, timeout=10, check=True)
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(replies), 3)
        self.assertEqual(replies[0]["error"]["code"], -32700)
        self.assertEqual(replies[1]["id"], "init")
        self.assertEqual(len(replies[2]["result"]["tools"]), 12)
        self.assertEqual(result.stderr, b"")

    def test_socket_fragmented_response_and_authentication(self):
        received = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()

            def serve():
                connection, _ = listener.accept()
                with connection, connection.makefile("rb") as stream:
                    received.append(json.loads(stream.readline()))
                    connection.sendall(b'{"ok":')
                    connection.sendall(b'true}\n')

            worker = threading.Thread(target=serve)
            worker.start()
            bridge = server.Bridge(listener.getsockname()[1], "test-secret-123456", 2)
            self.assertEqual(bridge.call("studio_status", {}), {"ok": True})
            worker.join(2)
        self.assertEqual(received[0]["token"], "test-secret-123456")
        self.assertEqual(received[0]["tool"], "studio_status")

    def test_no_secret_no_connection(self):
        with self.assertRaisesRegex(ValueError, "ASTERRA_MCP_TOKEN"):
            server.Bridge(9876, "", 1).call("studio_status", {})


if __name__ == "__main__":
    unittest.main()
