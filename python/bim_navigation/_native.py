"""One synchronous request stream to the native navigation simulator."""

import json
from pathlib import Path
from queue import Empty, Queue
import subprocess
import tempfile
from threading import Thread


_MAX_RESPONSE_BYTES = 1024 * 1024


class NavigationProtocolError(RuntimeError):
    """The native simulator connection failed or returned a malformed response."""


class NavigationRequestError(RuntimeError):
    """A well-formed simulator rejection leaves the connection available."""


def _read_responses(stream, responses):
    try:
        while True:
            line = stream.readline(_MAX_RESPONSE_BYTES + 1)
            if not line:
                return
            responses.put(line)
            if len(line) > _MAX_RESPONSE_BYTES:
                return
    finally:
        responses.put(None)


def _reject_nonfinite(token):
    raise ValueError(f"Invalid non-finite JSON value {token}")


class NativeNavigation:
    def __init__(self, executable, scene, *, timeout):
        executable = Path(executable).resolve(strict=True)
        scene = Path(scene).resolve(strict=True)
        self._timeout = timeout
        self._responses = Queue()
        self._stderr = tempfile.TemporaryFile()
        try:
            self._process = subprocess.Popen(
                [str(executable), str(scene)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=self._stderr,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except OSError:
            self._stderr.close()
            raise
        self._reader = Thread(target=_read_responses, args=(self._process.stdout, self._responses), daemon=True)
        self._reader.start()

    def _exchange(self, operation, args, timeout):
        process = self._process
        if process is None:
            raise NavigationProtocolError("The native navigation simulator is closed")
        request = json.dumps({"op": operation, **args}, allow_nan=False).encode("utf-8") + b"\n"
        try:
            process.stdin.write(request)
            process.stdin.flush()
            line = self._responses.get(timeout=timeout)
        except (BrokenPipeError, OSError) as error:
            raise NavigationProtocolError(f"Simulator connection broke during {operation}") from error
        except Empty as error:
            raise NavigationProtocolError(f"Simulator timed out during {operation} after {timeout:g} s") from error
        if line is None:
            raise NavigationProtocolError(f"Simulator ended without a {operation} response")
        if len(line) > _MAX_RESPONSE_BYTES or not line.endswith(b"\n"):
            raise NavigationProtocolError(f"Simulator returned an oversized or incomplete {operation} response")
        try:
            response = json.loads(line, parse_constant=_reject_nonfinite)
        except (UnicodeError, ValueError) as error:
            raise NavigationProtocolError(f"Simulator returned malformed JSON during {operation}") from error
        if not isinstance(response, dict) or type(response.get("ok")) is not bool:
            raise NavigationProtocolError(f"Simulator returned an invalid {operation} response")
        if not response["ok"]:
            raise NavigationRequestError(f"Simulator rejected {operation}: {response.get('error') or 'unknown error'}")
        return response

    def request(self, operation, **args):
        try:
            return self._exchange(operation, args, self._timeout)
        except NavigationProtocolError as error:
            detail = self._stop()
            if detail:
                raise NavigationProtocolError(f"{error}\nNative simulator: {detail}") from error
            raise

    def _stop(self):
        process, self._process = self._process, None
        if process is None:
            return ""
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        self._reader.join()
        process.stdout.close()
        log_size = self._stderr.seek(0, 2)
        self._stderr.seek(max(0, log_size - 2048))
        detail = self._stderr.read().decode("utf-8", errors="replace").strip()
        self._stderr.close()
        return detail

    def close(self):
        if self._process is not None and self._process.poll() is None:
            try:
                self._exchange("close", {}, min(self._timeout, 2))
            except (NavigationProtocolError, NavigationRequestError):
                pass
        self._stop()
