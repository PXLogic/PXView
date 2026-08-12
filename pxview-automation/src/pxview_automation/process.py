"""PXView process management.

This module provides a context manager that automatically starts
and stops a PXView ``--headless`` process, so you don't need to
manually launch ``PXView.exe`` before running automation scripts.

Typical usage::

    from pxview_automation import PXViewProcess, PXView

    with PXViewProcess(exe_path="C:/PXView/PXView.exe") as proc:
        with PXView(port=proc.port) as pxv:
            pxv.connect()
            devices = pxv.list_devices()
            print(devices)

On Windows, the process is launched with ``--headless`` which starts
the MCP (:10110) and WS (:10430) transports without any GUI.
Custom ports can be set via the *port* and *ws_port* parameters, which
are passed to PXView via ``--port`` and ``--ws-port`` command-line
options.
"""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import time
from typing import Optional

from .exceptions import ProcessError


class PXViewProcess:
    """Manage a PXView ``--headless`` subprocess.

    Args:
        exe_path:   Path to ``PXView.exe`` (or ``PXView`` on Linux/macOS).
                     If None, searches ``PATH`` and common install dirs.
        port:       MCP port (default: 10110).
        ws_port:    WebSocket port (default: 10430, informational only).
        log_level:  Log level 0-5 (passed via ``--loglevel``).
        store_log:  If True, pass ``--storelog`` to save logs to file.
        startup_timeout: Seconds to wait for the MCP port to become
                         reachable (default: 30).

    Attributes:
        port:          MCP port.
        process:       The ``subprocess.Popen`` object, or None if
                       not started.
    """

    # Common Windows install locations
    _WIN_SEARCH_PATHS = [
        r"C:\Program Files\PXView\PXView.exe",
        r"C:\Program Files (x86)\PXView\PXView.exe",
        r"C:\PXView\PXView.exe",
        r"D:\PXView\PXView.exe",
    ]

    # Common Linux install locations
    _LINUX_SEARCH_PATHS = [
        "/usr/local/bin/PXView",
        "/usr/bin/PXView",
        "/opt/PXView/PXView",
    ]

    def __init__(
        self,
        exe_path: Optional[str] = None,
        port: int = 10110,
        ws_port: int = 10430,
        log_level: int = -1,
        store_log: bool = False,
        startup_timeout: float = 30.0,
    ):
        self.port = port
        self.ws_port = ws_port
        self.log_level = log_level
        self.store_log = store_log
        self.startup_timeout = startup_timeout
        self._exe_path = exe_path or self._find_exe()
        self.process: Optional[subprocess.Popen] = None

    # ---- Context manager ----

    def __enter__(self) -> "PXViewProcess":
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.stop()

    # ---- Lifecycle ----

    def start(self) -> None:
        """Launch the PXView headless process.

        Raises:
            ProcessError: if the executable is not found, the process
                          fails to start, or the MCP port never becomes
                          reachable.
        """
        if self.process is not None:
            return  # Already started

        exe = self._exe_path
        if not exe or not os.path.isfile(exe):
            raise ProcessError(
                f"PXView executable not found: {exe!r}. "
                "Set exe_path explicitly or add PXView to PATH."
            )

        # Build command line
        cmd = [exe, "--headless"]
        if self.port != 10110:
            cmd.extend(["--port", str(self.port)])
        if self.ws_port != 10430:
            cmd.extend(["--ws-port", str(self.ws_port)])
        if self.log_level >= 0:
            cmd.extend(["--loglevel", str(self.log_level)])
        if self.store_log:
            cmd.append("--storelog")

        try:
            # On Windows, use CREATE_NO_WINDOW to avoid a console popup.
            kwargs: dict = {}
            if sys.platform == "win32":
                kwargs["creationflags"] = (
                    subprocess.CREATE_NO_WINDOW
                    if hasattr(subprocess, "CREATE_NO_WINDOW")
                    else 0x08000000
                )

            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                **kwargs,
            )
        except OSError as exc:
            raise ProcessError(
                f"Failed to start PXView: {exc}"
            ) from exc

        # On Windows, assign the child process to a Job Object so that
        # it is automatically killed when this (parent) process exits,
        # even on abnormal termination (crash, SIGKILL, etc.).
        # This mirrors the approach used by Saleae's logic2-automation.
        if sys.platform == "win32":
            self._assign_to_job_object()

        # Wait for MCP port to become reachable
        if not self._wait_for_port(self.startup_timeout):
            # Kill the process if it started but port never came up
            self.stop()
            raise ProcessError(
                f"PXView started but MCP port {self.port} did not become "
                f"reachable within {self.startup_timeout}s. "
                "Check PXView logs for errors."
            )

    def stop(self) -> None:
        """Stop the PXView process."""
        if self.process is None:
            return

        try:
            if sys.platform == "win32":
                # On Windows, terminate the process tree
                self.process.terminate()
            else:
                self.process.send_signal(signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass

        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            # Force kill if it doesn't exit gracefully
            try:
                self.process.kill()
                self.process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass

        self.process = None

    @property
    def is_running(self) -> bool:
        """True if the process is still running."""
        return self.process is not None and self.process.poll() is None

    # ---- Internal helpers ----

    def _assign_to_job_object(self) -> None:
        """Assign the child process to a Windows Job Object.

        The Job Object is configured with
        ``JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`` so that the child
        process is automatically terminated when the parent process
        exits — including on crashes, SIGKILL, or any abnormal
        termination.

        This prevents orphaned PXView --headless processes from
        lingering after the Python automation script exits.

        Requires ``pywin32`` (``win32job``, ``win32api``, ``win32con``).
        If pywin32 is not installed, this method silently does nothing
        (falls back to manual terminate() in stop()).
        """
        if self.process is None:
            return
        try:
            import win32job
            import win32api
            import win32con
        except ImportError:
            # pywin32 not installed — fall back to manual cleanup.
            return

        try:
            job = win32job.CreateJobObject(None, "PXViewAutomationJob")
            limits = win32job.QueryInformationJobObject(
                job, win32job.JobObjectExtendedLimitInformation
            )
            limits["BasicLimitInformation"]["LimitFlags"] = (
                win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            )
            win32job.SetInformationJobObject(
                job,
                win32job.JobObjectExtendedLimitInformation,
                limits,
            )
            child_handle = win32api.OpenProcess(
                win32con.PROCESS_ALL_ACCESS, False, self.process.pid
            )
            win32job.AssignProcessToJobObject(job, child_handle)
            # Store the job on the process object so it is not garbage
            # collected while the Popen object is alive.
            self._win32_job = job  # type: ignore[attr-defined]
        except Exception:
            # If Job Object creation fails for any reason, fall back to
            # manual cleanup. Don't crash the application.
            pass

    @classmethod
    def _find_exe(cls) -> Optional[str]:
        """Search for PXView executable in PATH and common locations."""
        exe_name = "PXView.exe" if sys.platform == "win32" else "PXView"

        # Search PATH
        found = shutil.which(exe_name)
        if found:
            return found

        # Search common Windows install locations
        if sys.platform == "win32":
            for path in cls._WIN_SEARCH_PATHS:
                if os.path.isfile(path):
                    return path

        # Search common Linux install locations
        if sys.platform != "win32":
            for path in cls._LINUX_SEARCH_PATHS:
                if os.path.isfile(path):
                    return path

        return None

    def _wait_for_port(self, timeout: float) -> bool:
        """Wait until the MCP port is reachable."""
        import json
        import urllib.request

        url = f"http://127.0.0.1:{self.port}/mcp"
        deadline = time.time() + timeout

        while time.time() < deadline:
            # Check if process died
            if self.process and self.process.poll() is not None:
                return False

            try:
                raw = json.dumps(
                    {"jsonrpc": "2.0", "id": 0, "method": "ping"}
                ).encode("utf-8")
                req = urllib.request.Request(
                    url,
                    data=raw,
                    headers={
                        "Content-Type": "application/json",
                        "Connection": "close",
                    },
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=3.0) as resp:
                    if resp.read().strip():
                        return True
            except Exception:
                pass

            time.sleep(0.5)

        return False
