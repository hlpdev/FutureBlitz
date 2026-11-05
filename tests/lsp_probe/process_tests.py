"""fb-lsp as a process, over real pipes, for what its tests in-process can't see: it ends when it's
told to, or when its client goes, whatever it's doing, and with a client that isn't reading."""
import contextlib
import json
import os
import pathlib
import subprocess
import tempfile
import threading
import time
import unittest

LSP = os.environ["FB_LSP"]
LSP_SOURCES = pathlib.Path(__file__).resolve().parents[2] / "lsp" / "src"
# A check that takes minutes, in a release build too: each Const runs until compile-time code's limit
# of steps, half a second there, and differs, as a call with the same arguments isn't run again.
SLOW = """Import std.core
Const Function Spin(n: Int): Int
    Local t = 0
    For i = 1 To n
        t = t + 1
    End For
    Return t
End Function
Function Twice(n: Int): Int => n * 2
""" + "".join(f"Const Slow{i} = Spin({1000000 + i})\n" for i in range(300)) + """Print(Twice(Slow0))
"""
TWICE_LINE = 8
QUICK = "Import std.core\nFunction Twice(n: Int): Int => n * 2\nPrint(Twice(1))\n"
# How long a process that isn't stuck has to end, on the slowest machine, past the 6 seconds one
# message can hold it and the 5 seconds it gives what's written. These tests say it ends, not how
# fast: how a queue costs is inbox_t's own test, by what it looks through.
ENDS = 60


class Client:
    def __init__(self, folder, text, read=True):
        self.folder = pathlib.Path(folder)
        # Not pathname2url, whose file:/x before Python 3.14 fb-lsp didn't take: Linux's CI checked
        # nothing here, as no document was open.
        self.uri = (self.folder / "main.fb").as_uri()
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

    def next_message(self):
        """The next message from it, or None at the end of them."""
        stream = self.process.stdout
        length = None
        while True:
            line = stream.readline()
            if not line:
                return None
            if line == b"\r\n":
                break
            length = int(line.split(b":")[1])
        return json.loads(stream.read(length))

    def _read(self):
        while True:
            message = self.next_message()
            if message is None:
                return
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
            "textDocument": {"uri": self.uri}, "position": {"line": TWICE_LINE, "character": 10},
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
            # What a send to a process that's gone left in its buffer can't be written.
            with contextlib.suppress(OSError):
                self.client.process.stdin.close()
            self.client.process.stdout.close()

    def _ends_within(self, seconds):
        begun = time.monotonic()
        code = self.client.process.wait(timeout=seconds)
        return code, time.monotonic() - begun

    def test_the_document_is_one_it_has_open(self):
        # What the rest of these depend on: a request of a document it didn't have would be null.
        self.client = Client(self.folder.name, QUICK)
        self.client.send({"id": 2, "method": "textDocument/documentSymbol", "params": {
            "textDocument": {"uri": self.client.uri}}})
        self.assertTrue((self.client.answer(2, 30) or {}).get("result"))

    def test_shutdown_and_exit_end_it_with_a_check_running(self):
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        begun = time.monotonic()
        self.client.send({"id": 3, "method": "shutdown"})
        self.assertIsNotNone(self.client.answer(3, 30), "shutdown wasn't answered")
        self.assertLess(time.monotonic() - begun, 15)
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        self._ends_within(ENDS)

    def test_shutdown_is_answered_though_exit_came_right_behind_it(self):
        # The reader sees exit first, and what ends the process for a message thread that's stuck
        # mustn't end it while shutdown's waiting its time, or its answer's lost.
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        self.client.send({"id": 3, "method": "shutdown"})
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 0)
        self.assertIsNotNone(self.client.answer(3, 5), "shutdown wasn't answered")

    def test_its_client_going_ends_it_with_shutdown_waiting_on_a_check(self):
        self.client = Client(self.folder.name, SLOW)
        self.client.references(2)
        self.client.send({"id": 3, "method": "shutdown"})
        self.client.process.stdin.close()
        self._ends_within(ENDS)

    def _queue_requests(self):
        for id in range(10, 12000):
            self.client.send({"id": id, "method": "textDocument/documentSymbol", "params": {
                "textDocument": {"uri": self.client.uri}}})

    def test_a_client_that_isnt_reading_doesnt_keep_it(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        self._queue_requests()
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        self._ends_within(ENDS)

    def test_a_queue_of_quick_requests_doesnt_keep_it_once_its_client_goes(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        self._queue_requests()
        self.client.process.stdin.close()
        self._ends_within(ENDS)

    def test_a_queue_of_quick_requests_behind_shutdown_is_quick_to_answer(self):
        # Each looks for its own $/cancelRequest in what's waiting, which took the queue's length
        # squared.
        self.client = Client(self.folder.name, QUICK, read=False)
        self._queue_requests()
        self.client.send({"id": 3, "method": "shutdown"})
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 0)

    def test_a_cancel_behind_a_queue_of_quick_requests_doesnt_keep_it(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        self._queue_requests()
        self.client.send({"method": "$/cancelRequest", "params": {"id": 1000000}})
        self.client.send({"id": 3, "method": "shutdown"})
        self.client.send({"method": "exit"})
        self.client.process.stdin.close()
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 0)

    def _send_behind_a_held_message_thread(self, last):
        """Requests the message thread answers itself, behind one that holds it for longer than the
        reader takes to read them all, and then `last`, all at once. Says how many there are."""
        count = 200
        big = "Local  n = 1\n" * 20000
        big_uri = (pathlib.Path(self.folder.name) / "big.fb").as_uri()
        formatting = {"method": "textDocument/formatting", "params": {
            "textDocument": {"uri": big_uri}, "options": {"tabSize": 4, "insertSpaces": True}}}
        self.client.send({"method": "textDocument/didOpen", "params": {"textDocument": {
            "uri": big_uri, "languageId": "futureblitz", "version": 1, "text": big}}})
        # Those behind the big one are quick: only their order counts.
        small = {"method": "textDocument/formatting", "params": {
            "textDocument": {"uri": self.client.uri},
            "options": {"tabSize": 4, "insertSpaces": True}}}
        messages = [{"id": 10, **formatting}]
        messages += [{"id": id, **small} for id in range(11, 11 + count)]
        messages += last
        data = b""
        for message in messages:
            body = json.dumps({"jsonrpc": "2.0", **message}).encode()
            data += b"Content-Length: %d\r\n\r\n" % len(body) + body
        self.client.process.stdin.write(data)
        self.client.process.stdin.flush()
        return count

    def _answers_of_the_small_ones(self):
        self.client.process.stdin.close()
        answered = 0
        while (message := self.client.next_message()) is not None:
            if message.get("id", 0) > 10 and "method" not in message:
                answered += 1
        self._ends_within(ENDS)
        return answered

    def test_exit_without_shutdown_drops_what_waits_for_the_message_thread(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        count = self._send_behind_a_held_message_thread([{"method": "exit"}])
        self.assertLess(self._answers_of_the_small_ones(), count)

    def test_going_without_shutdown_drops_what_waits_for_the_message_thread(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        count = self._send_behind_a_held_message_thread([])
        self.assertLess(self._answers_of_the_small_ones(), count)

    def test_what_waits_behind_shutdown_is_answered(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        count = self._send_behind_a_held_message_thread(
            [{"id": 3, "method": "shutdown"}, {"method": "exit"}])
        # Shutdown's own id is less than theirs: it counts only theirs.
        self.assertEqual(self._answers_of_the_small_ones(), count)

    def test_a_client_that_closes_what_it_reads_after_shutdown_is_done(self):
        # Not "isn't reading", nor code 1: a late message's write fails with the client done.
        self.client = Client(self.folder.name, QUICK, read=False)
        self.client.send({"id": 3, "method": "shutdown"})
        while (message := self.client.next_message()) is not None:
            if message.get("id") == 3:
                break
        self.client.process.stdout.close()
        self.client.send({"id": 4, "method": "textDocument/documentSymbol", "params": {
            "textDocument": {"uri": self.client.uri}}})
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 0)

    def test_a_client_that_closed_what_it_reads_ends_it(self):
        self.client = Client(self.folder.name, QUICK, read=False)
        self.client.process.stdout.close()
        # It may already have ended, on writing initialize's answer.
        with contextlib.suppress(OSError):
            self.client.send({"id": 2, "method": "textDocument/documentSymbol", "params": {
                "textDocument": {"uri": self.client.uri}}})
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 1)

    def test_a_length_no_message_has_ends_it(self):
        # Not by aborting, which is what an exception on the reader's thread does.
        self.client = Client(self.folder.name, QUICK)
        self.client.process.stdin.write(b"Content-Length: 9223372036854775807\r\n\r\n")
        self.client.process.stdin.flush()
        code, _ = self._ends_within(ENDS)
        self.assertEqual(code, 1)

    def test_ping_is_answered_with_the_message_thread_held(self):
        # Shutdown holds the message thread for as long as it waits on the references, so the ping
        # is answered first, however slow the machine.
        self.client = Client(self.folder.name, SLOW)
        self.assertIsNotNone(self.client.answer(1, 60), "initialize wasn't answered")
        self.client.references(2)
        self.client.send({"id": 3, "method": "shutdown"})
        self.client.send({"id": 4, "method": "fb/ping"})
        self.assertIn("result", self.client.answer(4, 30) or {})
        with self.client.arrived:
            answered = list(self.client.answers)
        if 3 in answered:
            self.assertLess(answered.index(4), answered.index(3))

    def test_nothing_but_the_outbox_writes_messages(self):
        for source in LSP_SOURCES.glob("*.cpp"):
            code = "\n".join(line for line in source.read_text(encoding="utf-8").splitlines()
                             if not line.lstrip().startswith("//"))
            self.assertNotIn("std::cout", code, source.name)
            self.assertNotIn("printf(", code, source.name)


if __name__ == "__main__":
    unittest.main()
