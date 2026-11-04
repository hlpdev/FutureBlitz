"""fb-lsp as a process, over real pipes, for what its tests in-process can't see: it ends when it's
told to, or when its client goes, whatever it's doing, and with a client that isn't reading."""
import json
import os
import pathlib
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.request import pathname2url

LSP = os.environ["FB_LSP"]
LSP_SOURCES = pathlib.Path(__file__).resolve().parents[2] / "lsp" / "src"
# A Const that takes minutes in a debug build, and long enough in a release one.
SLOW = """Import std.core
Const Function Spin(n: Int): Int
    Local t = 0
    For i = 1 To n
        t = t + 1
    End For
    Return t
End Function
Const Slow = Spin(30000000)
Function Twice(n: Int): Int => n * 2
Print(Twice(Slow))
"""
QUICK = "Import std.core\nFunction Twice(n: Int): Int => n * 2\nPrint(Twice(1))\n"


class Client:
    def __init__(self, folder, text, read=True):
        self.folder = pathlib.Path(folder)
        self.uri = "file:" + pathname2url(str(self.folder / "main.fb"))
        (self.folder / "main.fb").write_text(text)
        self.process = subprocess.Popen([LSP], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, cwd=folder)
        self.answers = {}
        self.arrived = threading.Condition()
        if read:
            threading.Thread(target=self._read, daemon=True).start()
        self.send({"id": 1, "method": "initialize", "params": {"capabilities": {}}})
        self.send({"method": "initialized", "params": {}})
        self.send({"method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": self.uri, "languageId": "futureblitz", "version": 1, "text": text}}})

    def send(self, message):
        body = json.dumps({"jsonrpc": "2.0", **message}).encode()
        self.process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.process.stdin.flush()

    def _read(self):
        stream = self.process.stdout
        while True:
            length = None
            while True:
                line = stream.readline()
                if not line:
                    return
                if line == b"\r\n":
                    break
                length = int(line.split(b":")[1])
            message = json.loads(stream.read(length))
            if "id" in message and "method" not in message:
                with self.arrived:
                    self.answers[message["id"]] = message
                    self.arrived.notify_all()

    def answer(self, id, timeout):
        with self.arrived:
            self.arrived.wait_for(lambda: id in self.answers, timeout)
            return self.answers.get(id)

    def references(self, id):
        self.send({"id": id, "method": "textDocument/references", "params": {
            "textDocument": {"uri": self.uri}, "position": {"line": 9, "character": 10},
            "context": {"includeDeclaration": True}}})


class ProcessTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.addCleanup(self.folder.cleanup)
        self.client = None
        self.addCleanup(self._kill)

    def _kill(self):
        if self.client is not None and self.client.process.poll() is None:
            self.client.process.kill()
        if self.client is not None:
            self.client.process.wait()
            self.client.process.stdin.close()
            self.client.process.stdout.close()

    def _ends_within(self, seconds):
        begun = time.monotonic()
        code = self.client.process.wait(timeout=seconds)
        return code, time.monotonic() - begun

    def test_shutdown_and_exit_end_it_with_a_check_running(self):
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        begun = time.monotonic()
        self.client.send({"id": 3, "method": "shutdown"})
        self.assertIsNotNone(self.client.answer(3, 30), "shutdown wasn't answered")
        self.assertLess(time.monotonic() - begun, 15)
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        self._ends_within(15)

    def test_its_client_going_ends_it_with_a_check_running(self):
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        self.client.process.stdin.close()
        self._ends_within(15)

    def test_a_client_that_isnt_reading_doesnt_keep_it(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        for id in range(10, 3000):
            self.client.send({"id": id, "method": "textDocument/documentSymbol", "params": {
                "textDocument": {"uri": self.client.uri}}})
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        self._ends_within(15)

    def test_ping_is_answered_with_the_server_busy(self):
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        self.client.send({"id": 3, "method": "fb/ping"})
        self.assertIn("result", self.client.answer(3, 5) or {})

    def test_nothing_but_the_outbox_writes_messages(self):
        for source in LSP_SOURCES.glob("*.cpp"):
            code = "\n".join(line for line in source.read_text(encoding="utf-8").splitlines()
                             if not line.lstrip().startswith("//"))
            self.assertNotIn("std::cout", code, source.name)
            self.assertNotIn("printf(", code, source.name)


if __name__ == "__main__":
    unittest.main()
