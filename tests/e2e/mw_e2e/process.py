from __future__ import annotations

import os
import signal
import subprocess
from pathlib import Path
from typing import IO, Mapping, Sequence


class ProcessError(RuntimeError):
    pass


class ManagedProcess:
    def __init__(
        self,
        command: Sequence[str],
        log_path: Path,
        *,
        cwd: Path | None = None,
        environment: Mapping[str, str] | None = None,
    ) -> None:
        self.command = [str(argument) for argument in command]
        self.log_path = log_path
        self.cwd = cwd
        self.environment = environment
        self._process: subprocess.Popen[str] | None = None
        self._log_file: IO[str] | None = None

    @property
    def pid(self) -> int:
        if self._process is None:
            raise ProcessError("进程尚未启动")
        return self._process.pid

    @property
    def returncode(self) -> int | None:
        return None if self._process is None else self._process.poll()

    def start(self) -> None:
        if self._process is not None:
            raise ProcessError("进程不能重复启动")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self.log_path.open("w", encoding="utf-8")
        self._log_file.write("command=" + " ".join(self.command) + "\n")
        self._log_file.flush()
        environment = os.environ.copy()
        if self.environment:
            environment.update(self.environment)
        self._process = subprocess.Popen(
            self.command,
            cwd=self.cwd,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=self._log_file,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )

    def ensure_running(self) -> None:
        if self._process is None:
            raise ProcessError("进程尚未启动")
        returncode = self._process.poll()
        if returncode is not None:
            raise ProcessError(
                f"进程提前退出，返回码 {returncode}: {self.log_path}"
            )

    def interrupt(self) -> None:
        if self._process is not None and self._process.poll() is None:
            os.killpg(self._process.pid, signal.SIGINT)

    def wait(self, timeout: float) -> int:
        if self._process is None:
            raise ProcessError("进程尚未启动")
        try:
            return self._process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            raise ProcessError(f"等待进程退出超时: {self.log_path}") from error
        finally:
            if self._process.poll() is not None:
                self._close_log()

    def stop(self, timeout: float = 5.0) -> None:
        if self._process is None:
            return
        if self._process.poll() is None:
            self.interrupt()
            try:
                self._process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(self._process.pid, signal.SIGKILL)
                self._process.wait(timeout=timeout)
        self._close_log()

    def _close_log(self) -> None:
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
