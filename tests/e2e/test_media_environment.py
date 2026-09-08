from __future__ import annotations

import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest


@pytest.mark.smoke
def test_server_crash_is_reported_and_next_test_recovers(
    pytestconfig: pytest.Config, artifact_directory: Path, runner_path: Path
) -> None:
    # Run two actual pytest items through the production fixtures. The first
    # deliberately crashes servers; its teardown must fail, while the next
    # item must recover them without restarting the healthy servers.
    probe = artifact_directory / "test_server_isolation_probe.py"
    probe.write_text(
        '''import os
import signal
import pytest
from pathlib import Path
from mw_e2e.process import ProcessError

observed = {}

@pytest.fixture(scope="session")
def artifact_root():
    root = Path(__file__).parent / "nested-artifacts"
    root.mkdir()
    return root

def test_crash(media_environment):
    env = media_environment
    observed["healthy"] = env.sinks["rtmp"]._process.pid
    for server in [env.source, env.sinks["rtsp"]]:
        process = server._process
        observed[server.name] = (process.pid, server.ports, process.log_path)
        os.kill(process.pid, signal.SIGKILL)
        assert process.wait(5) == -signal.SIGKILL
        with pytest.raises(ProcessError, match="进程提前退出"):
            server.ensure_running()
        assert server._process.pid == process.pid
        observed[server.name + "_log"] = process.log_path.read_bytes()

def test_next_item(media_environment):
    env = media_environment
    assert env.sinks["rtmp"]._process.pid == observed["healthy"]
    for server in [env.source, env.sinks["rtsp"]]:
        pid, ports, log_path = observed[server.name]
        server.ensure_running()
        assert server._process.pid != pid
        assert server.ports == ports
        assert log_path.read_bytes() == observed[server.name + "_log"]
        assert server._process.log_path.name == "mediamtx-restart-1.log"
        assert "items" in server._request_json("/v3/paths/list")
''',
        encoding="utf-8",
    )
    report = artifact_directory / "isolation.xml"
    command = [
        sys.executable, "-m", "pytest", str(probe),
        "--e2e-config", str(Path(pytestconfig.getoption("--e2e-config")).resolve()),
        "--e2e-runner", str(runner_path),
        "--junitxml", str(report), "-vv", "--tb=short",
    ]
    with (artifact_directory / "isolation.log").open("w") as log:
        completed = subprocess.run(
            command, stdout=log, stderr=subprocess.STDOUT, timeout=90,
            check=False,
        )
    assert completed.returncode == 1
    cases = {case.attrib["name"]: case for case in ET.parse(report).iter("testcase")}
    assert set(cases) == {"test_crash", "test_next_item"}
    error = cases["test_crash"].find("error")
    assert error is not None
    assert "进程提前退出" in "".join(error.itertext())
    assert cases["test_next_item"].find("error") is None
    assert cases["test_next_item"].find("failure") is None
    assert cases["test_next_item"].find("skipped") is None
