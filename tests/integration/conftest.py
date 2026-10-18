"""Shared fixtures: start logictd from the build tree with a temporary config."""

import json
import os
import shlex
import socket
import subprocess
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("LOGICT_BUILD_DIR", ROOT / "build" / "dev"))
LOGICTD = BUILD / "app" / "logictd" / "logictd"
# e.g. LOGICT_DAEMON_PREFIX="setarch -R" for sanitizer builds (see tests/CMakeLists.txt).
PREFIX = shlex.split(os.environ.get("LOGICT_DAEMON_PREFIX", ""))

# Password "logict".
ADMIN_HASH = "pbkdf2-sha256$200000$Jk2RpOYB+OuuW7OxmumE3Q==$z1VjELUQgptN5s5lCdbT4lLHsGH/MdrdLaPQvQ4YlTw="


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port: int, proc: subprocess.Popen, timeout: float = 15.0) -> None:
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            raise RuntimeError(f"process exited early with code {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"port {port} not open after {timeout}s")


class Daemon:
    def __init__(self, tmp: Path, project: Path, opcua: dict, trusted_certs=()):
        self.tmp = tmp
        self.port = free_port()
        self.pki = tmp / "pki"
        trusted = self.pki / "ApplCerts" / "trusted" / "certs"
        trusted.mkdir(parents=True, exist_ok=True)
        for cert in trusted_certs:
            (trusted / Path(cert).name).write_bytes(Path(cert).read_bytes())
        cfg = {
            "log_level": "info",
            "project_file": str(project),
            "opcua": {"port": self.port, "pki_dir": str(self.pki), "hostname": "localhost", **opcua},
            "users": [{"name": "admin", "password_hash": ADMIN_HASH}],
        }
        self.config = tmp / "logictd.json"
        self.config.write_text(json.dumps(cfg))
        self.log = open(tmp / "logictd.log", "w")
        self.proc = subprocess.Popen([*PREFIX, str(LOGICTD), "-c", str(self.config)], stdout=self.log, stderr=subprocess.STDOUT)
        wait_for_port(self.port, self.proc)

    @property
    def url(self) -> str:
        return f"opc.tcp://127.0.0.1:{self.port}"

    def kill(self) -> None:
        """Simulates a crash (no clean MQTT disconnect)."""
        self.proc.kill()
        self.proc.wait()
        self.killed = True

    def stop(self) -> int:
        if getattr(self, "killed", False):
            self.log.close()
            return 0
        self.proc.terminate()
        try:
            code = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = -9
        self.log.close()
        return code

    def output(self) -> str:
        return (self.tmp / "logictd.log").read_text()


@pytest.fixture
def daemon_factory(tmp_path):
    started = []

    def make(opcua=None, project=ROOT / "config" / "example-project.json", trusted_certs=(), iot_gateway=None):
        if iot_gateway is not None:
            p = json.loads(Path(project).read_text())
            p["project"]["_iot_gateway"] = iot_gateway
            project = tmp_path / "project-with-iot.json"
            project.write_text(json.dumps(p))
        d = Daemon(tmp_path, project, opcua or {}, trusted_certs)
        started.append(d)
        return d

    yield make
    for d in started:
        code = d.stop()
        assert code == 0, f"logictd did not shut down cleanly ({code}):\n{d.output()[-3000:]}"
