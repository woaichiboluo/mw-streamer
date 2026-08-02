from __future__ import annotations

import json
import socket
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .models import E2EConfig
from .process import ManagedProcess, ProcessError


_allocated_ports: dict[socket.SocketKind, set[int]] = {
    socket.SOCK_STREAM: set(),
    socket.SOCK_DGRAM: set(),
}


def _free_port(socket_type: socket.SocketKind) -> int:
    while True:
        with socket.socket(socket.AF_INET, socket_type) as listener:
            listener.bind(("127.0.0.1", 0))
            port = int(listener.getsockname()[1])
        if port not in _allocated_ports[socket_type]:
            _allocated_ports[socket_type].add(port)
            return port


def allocate_tcp_port() -> int:
    return _free_port(socket.SOCK_STREAM)


def allocate_udp_port() -> int:
    return _free_port(socket.SOCK_DGRAM)


@dataclass(frozen=True)
class ServerPorts:
    api: int
    metrics: int
    rtsp: int
    rtmp: int
    srt: int

    @classmethod
    def allocate(cls) -> "ServerPorts":
        return cls(
            api=_free_port(socket.SOCK_STREAM),
            metrics=_free_port(socket.SOCK_STREAM),
            rtsp=_free_port(socket.SOCK_STREAM),
            rtmp=_free_port(socket.SOCK_STREAM),
            srt=_free_port(socket.SOCK_DGRAM),
        )


class MediaMtx:
    _connection_endpoints = {
        "rtsp": "rtspsessions",
        "rtmp": "rtmpconns",
        "srt": "srtconns",
    }

    def __init__(
        self,
        config: E2EConfig,
        name: str,
        root: Path,
        protocols: set[str],
        *,
        record: bool = False,
    ) -> None:
        self.config = config
        self.name = name
        self.root = root / name
        self.protocols = protocols
        self.record = record
        self.ports = ServerPorts.allocate()
        self._process: ManagedProcess | None = None

    @property
    def api_address(self) -> str:
        return f"http://127.0.0.1:{self.ports.api}"

    @property
    def recording_root(self) -> Path:
        return self.root / "recordings"

    def start(self) -> None:
        if self._process is not None:
            raise ProcessError(f"MediaMTX {self.name} 已经启动")
        self.root.mkdir(parents=True, exist_ok=True)
        config_path = self.root / "mediamtx.yml"
        config_path.write_text(self._render_config(), encoding="utf-8")
        self._process = ManagedProcess(
            [str(self.config.tools.mediamtx), str(config_path)],
            self.root / "mediamtx.log",
            cwd=self.root,
        )
        self._process.start()
        self._wait_for_api(self.config.tests.startup_timeout_seconds)

    def stop(self) -> None:
        if self._process is not None:
            self._process.stop()
            self._process = None

    def restart(self) -> None:
        self.stop()
        self.start()

    def read_url(self, protocol: str, path: str) -> str:
        if protocol == "rtsp":
            return f"rtsp://127.0.0.1:{self.ports.rtsp}/{path}"
        if protocol == "rtmp":
            return f"rtmp://127.0.0.1:{self.ports.rtmp}/{path}"
        if protocol == "srt":
            return (
                f"srt://127.0.0.1:{self.ports.srt}"
                f"?streamid=read:{path}"
            )
        raise ValueError(f"未知协议: {protocol}")

    def publish_url(self, protocol: str, path: str) -> str:
        if protocol == "rtsp":
            return f"rtsp://127.0.0.1:{self.ports.rtsp}/{path}"
        if protocol == "rtmp":
            return f"rtmp://127.0.0.1:{self.ports.rtmp}/{path}"
        if protocol == "srt":
            return (
                f"srt://127.0.0.1:{self.ports.srt}"
                f"?streamid=publish:{path}"
            )
        raise ValueError(f"未知协议: {protocol}")

    def wait_for_path(
        self,
        path: str,
        timeout: float,
        monitored_process: ManagedProcess | None = None,
    ) -> dict[str, Any]:
        def find() -> dict[str, Any] | None:
            if monitored_process is not None:
                monitored_process.ensure_running()
            return self._find_path(path)

        return self._wait_until(
            find,
            timeout,
            f"等待 MediaMTX 路径可用超时: {self.name}/{path}",
        )

    def list_connections(self, protocol: str) -> list[dict[str, Any]]:
        endpoint = self._connection_endpoints[protocol]
        response = self._request_json(f"/v3/{endpoint}/list")
        return list(response.get("items", []))

    def wait_for_connection(
        self,
        protocol: str,
        path: str,
        state: str,
        timeout: float,
        *,
        excluded_id: str | None = None,
    ) -> dict[str, Any]:
        def find() -> dict[str, Any] | None:
            for item in self.list_connections(protocol):
                if (
                    item.get("path") == path
                    and item.get("state") == state
                    and item.get("id") != excluded_id
                ):
                    return item
            return None

        return self._wait_until(
            find,
            timeout,
            f"等待 {protocol} {state} 连接超时: {self.name}/{path}",
        )

    def recordings(self, path: str) -> list[Path]:
        candidates = sorted((self.recording_root / path).glob("*.mp4"))
        if not candidates:
            raise ProcessError(
                f"MediaMTX没有生成录像: {self.recording_root / path}"
            )
        return candidates

    def kick_connection(self, protocol: str, connection_id: str) -> None:
        endpoint = self._connection_endpoints[protocol]
        request = urllib.request.Request(
            f"{self.api_address}/v3/{endpoint}/kick/{connection_id}",
            data=b"",
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=3):
                return
        except urllib.error.URLError as error:
            raise ProcessError(
                f"踢出 {protocol} 连接失败: {connection_id}: {error}"
            ) from error

    def _find_path(self, path: str) -> dict[str, Any] | None:
        response = self._request_json("/v3/paths/list")
        for item in response.get("items", []):
            if item.get("name") == path and item.get("available", item.get("ready")):
                return item
        return None

    def _wait_for_api(self, timeout: float) -> None:
        self._wait_until(
            lambda: self._request_json("/v3/paths/list"),
            timeout,
            f"MediaMTX API 启动超时: {self.name}",
        )

    def _wait_until(
        self,
        operation: Callable[[], Any],
        timeout: float,
        message: str,
    ) -> Any:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self._process is not None:
                self._process.ensure_running()
            try:
                result = operation()
                if result:
                    return result
            except (ProcessError, urllib.error.URLError, json.JSONDecodeError) as error:
                last_error = error
            time.sleep(0.1)
        if last_error:
            raise ProcessError(f"{message}: {last_error}") from last_error
        raise ProcessError(message)

    def _request_json(self, path: str) -> dict[str, Any]:
        with urllib.request.urlopen(self.api_address + path, timeout=2) as response:
            return json.load(response)

    def _render_config(self) -> str:
        enabled = lambda protocol: "true" if protocol in self.protocols else "false"
        record = "true" if self.record else "false"
        record_path = json.dumps(
            str(self.recording_root / "%path/%Y-%m-%d_%H-%M-%S-%f")
        )
        return f"""\
logLevel: debug
logDestinations: [stdout]
api: true
apiAddress: 127.0.0.1:{self.ports.api}
metrics: true
metricsAddress: 127.0.0.1:{self.ports.metrics}
playback: false
rtsp: {enabled("rtsp")}
rtspTransports: [tcp]
rtspAddress: 127.0.0.1:{self.ports.rtsp}
rtmp: {enabled("rtmp")}
rtmpAddress: 127.0.0.1:{self.ports.rtmp}
hls: false
webrtc: false
srt: {enabled("srt")}
srtAddress: 127.0.0.1:{self.ports.srt}
moq: false
paths:
  all_others:
    record: {record}
    recordPath: {record_path}
    recordFormat: fmp4
    recordPartDuration: 100ms
    recordSegmentDuration: 1h
    recordDeleteAfter: 0s
"""


class MediaEnvironment:
    def __init__(self, config: E2EConfig, root: Path) -> None:
        self.source = MediaMtx(
            config, "source", root, {"rtsp", "rtmp", "srt"}
        )
        self.sinks = {
            "rtsp": MediaMtx(config, "sink-rtsp", root, {"rtsp"}),
            "rtmp": MediaMtx(config, "sink-rtmp", root, {"rtmp", "rtsp"}),
            "srt": MediaMtx(config, "sink-srt", root, {"srt", "rtsp"}),
        }

    def start(self) -> None:
        started: list[MediaMtx] = []
        try:
            for server in [self.source, *self.sinks.values()]:
                server.start()
                started.append(server)
        except Exception:
            for server in reversed(started):
                server.stop()
            raise

    def stop(self) -> None:
        for server in reversed([self.source, *self.sinks.values()]):
            server.stop()
