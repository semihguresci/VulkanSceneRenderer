"""Bounded request/reply transport for the native BIM simulator."""

import json
from queue import Empty, Full, Queue
import subprocess
import sys
import tempfile
from threading import Thread


_MAX_REPLY_BYTES = 1024 * 1024
_SCHEMA = "container.bim-drive-stream"


def native_input_path(executable, path):
    resolved = str(path.resolve(strict=True))
    if sys.platform != "linux" or executable.suffix.lower() != ".exe":
        return resolved
    # WSL executes the Windows simulator with Windows path semantics.
    try:
        result = subprocess.run(["wslpath", "-w", resolved], capture_output=True, text=True, timeout=5)
    except FileNotFoundError as error:
        raise ValueError("Use a Linux simulator executable or run the Windows executable from WSL") from error
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"Timed out converting the WSL path {resolved}") from error
    converted = result.stdout.strip()
    if result.returncode != 0 or not converted:
        raise ValueError(f"Could not convert the WSL path {resolved}: {result.stderr.strip()}")
    return converted


def _reject_constant(value):
    raise ValueError(f"non-finite JSON constant {value}")


def _read_replies(stream, inbox):
    try:
        with stream:
            while line := stream.readline(_MAX_REPLY_BYTES + 1):
                inbox.put_nowait(line)
                if len(line) > _MAX_REPLY_BYTES:
                    break
    except (Full, OSError):
        pass
    finally:
        try:
            inbox.put_nowait(None)
        except Full:
            pass


class DriveStream:
    def __init__(self, command):
        self._stderr = tempfile.TemporaryFile()
        self._inbox = Queue(maxsize=2)
        try:
            self._process = subprocess.Popen(
                command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError:
            self._stderr.close()
            raise
        self._reader = Thread(target=_read_replies, args=(self._process.stdout, self._inbox), daemon=True)
        self._reader.start()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _exchange(self, operation, timeout, fields):
        data = json.dumps({"op": operation, **fields}, allow_nan=False).encode("utf-8") + b"\n"
        self._process.stdin.write(data)
        self._process.stdin.flush()
        try:
            line = self._inbox.get(timeout=timeout)
        except Empty as error:
            raise ValueError(f"no reply within {timeout:g} seconds") from error
        if line is None:
            raise ValueError("process ended before replying")
        if len(line) > _MAX_REPLY_BYTES or not line.endswith(b"\n"):
            raise ValueError("oversized or incomplete reply")
        response = json.loads(line, parse_constant=_reject_constant)
        if (not isinstance(response, dict) or response.get("schema") != _SCHEMA or
                type(response.get("version")) is not int or response["version"] != 1 or
                response.get("op") != operation or type(response.get("ok")) is not bool):
            raise ValueError("invalid stream reply envelope")
        if not response["ok"]:
            raise ValueError(str(response.get("error") or "native request rejected"))
        return response

    def request(self, operation, *, timeout=3.0, **fields):
        if self._process is None:
            raise RuntimeError("Native BIM simulator connection is closed")
        try:
            return self._exchange(operation, timeout, fields)
        except (OSError, ValueError) as error:
            detail = self._stop()
            suffix = f"\nNative BIM simulator output: {detail}" if detail else ""
            raise RuntimeError(f"Native BIM simulator {operation} failed: {error}{suffix}") from error

    def _stop(self):
        process, self._process = self._process, None
        if process is None:
            return ""
        try:
            try:
                process.stdin.close()
            except OSError:
                pass
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
            self._reader.join(timeout=1)
            end = self._stderr.seek(0, 2)
            self._stderr.seek(max(0, end - 2048))
            return self._stderr.read().decode("utf-8", errors="replace").strip()
        finally:
            self._stderr.close()

    def close(self):
        if self._process is not None and self._process.poll() is None:
            try:
                self._exchange("close", 1.0, {})
            except (OSError, ValueError):
                pass
        self._stop()
