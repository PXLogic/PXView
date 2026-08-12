"""Typed dataclasses and enums for pxview-automation.

This module provides strongly-typed configuration objects that mirror
the structure of PXView's MCP tool arguments.  Using these classes
gives you IDE autocompletion, type checking, and validation instead
of passing raw dicts.

Typical usage::

    from pxview_automation import PXView, LogicDeviceConfiguration, TimedCaptureMode

    with PXView() as pxv:
        pxv.connect()
        pxv.capture_typed(
            device_id="demo",
            device_config=LogicDeviceConfiguration(
                digital_channels=[0, 1],
                digital_sample_rate=1_000_000,
            ),
            capture_config=CaptureConfiguration(
                capture_mode=TimedCaptureMode(duration_seconds=1.0),
            ),
        )

All dataclasses can be converted to the dict format expected by the
low-level :class:`~pxview_automation.client.McpClient` via
:meth:`to_dict`.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Union


# ======================================================================
# Enums
# ======================================================================


class DeviceType(Enum):
    """Device type classification."""

    LOGIC = "logic"
    ANALOG = "analog"
    DSO = "dso"
    MIXED = "mixed"
    FILE = "file"
    DEMO = "demo"
    UNKNOWN = "unknown"


class ChannelType(Enum):
    """Channel signal type."""

    LOGIC = 0
    ANALOG = 1
    DSO = 2
    UNKNOWN = 99


class CollectMode(Enum):
    """Capture collect mode."""

    SINGLE = "single"
    REPEAT = "repeat"
    LOOP = "loop"


class StreamMode(Enum):
    """Data stream mode."""

    STOP = "stop"
    STREAM = "stream"


class DigitalTriggerType(Enum):
    """Digital trigger edge/pulse type."""

    RISING = "rising"
    FALLING = "falling"
    PULSE_HIGH = "pulse_high"
    PULSE_LOW = "pulse_low"


class DigitalTriggerLinkedChannelState(Enum):
    """Required state of a linked channel at trigger time."""

    LOW = "low"
    HIGH = "high"


class RadixType(Enum):
    """Display radix for data table export."""

    BINARY = "binary"
    DECIMAL = "decimal"
    HEXADECIMAL = "hex"
    ASCII = "ascii"


class ExportFormat(Enum):
    """Raw data export format."""

    CSV = "csv"
    BINARY = "binary"
    VCD = "vcd"
    HEX = "hex"
    BITS = "bits"


class CouplingType(Enum):
    """Probe coupling type."""

    DC = 0
    AC = 1
    GND = 2


# ======================================================================
# Glitch filter
# ======================================================================


@dataclass
class GlitchFilterEntry:
    """Glitch filter specification for a single digital channel.

    Pulses shorter than ``pulse_width_samples`` (or
    ``pulse_width_seconds``) are removed before decoding.

    Specify either ``pulse_width_samples`` or ``pulse_width_seconds``,
    not both.
    """

    channel_index: int
    pulse_width_samples: Optional[int] = None
    pulse_width_seconds: Optional[float] = None

    def to_dict(self) -> dict:
        d: dict = {"channelIndex": self.channel_index}
        if self.pulse_width_samples is not None:
            d["pulseWidthSamples"] = self.pulse_width_samples
        if self.pulse_width_seconds is not None:
            d["pulseWidthSeconds"] = self.pulse_width_seconds
        return d


# ======================================================================
# Device configuration
# ======================================================================


@dataclass
class LogicDeviceConfiguration:
    """Device/channel configuration for a capture.

    This is the typed equivalent of the ``logicDeviceConfiguration``
    dict argument in :meth:`McpClient.start_capture`.

    Example::

        LogicDeviceConfiguration(
            digital_channels=[0, 1],
            analog_channels=[2, 3],
            digital_sample_rate=1_000_000,
            analog_sample_rate=500_000,
            digital_threshold_volts=1.8,
        )
    """

    digital_channels: List[int] = field(default_factory=list)
    analog_channels: List[int] = field(default_factory=list)
    digital_sample_rate: Optional[int] = None
    analog_sample_rate: Optional[int] = None
    digital_threshold_volts: Optional[float] = None
    glitch_filters: List[GlitchFilterEntry] = field(default_factory=list)

    def to_dict(self) -> dict:
        d: dict = {}
        if self.digital_channels:
            d["digitalChannels"] = self.digital_channels
        if self.analog_channels:
            d["analogChannels"] = self.analog_channels
        if self.digital_sample_rate is not None:
            d["digitalSampleRate"] = self.digital_sample_rate
        if self.analog_sample_rate is not None:
            d["analogSampleRate"] = self.analog_sample_rate
        if self.digital_threshold_volts is not None:
            d["digitalThresholdVolts"] = self.digital_threshold_volts
        if self.glitch_filters:
            d["glitchFilters"] = [g.to_dict() for g in self.glitch_filters]
        return d


# ======================================================================
# Capture modes
# ======================================================================


@dataclass
class ManualCaptureMode:
    """Manual capture mode — stop with :meth:`McpClient.stop_capture`.

    Args:
        trim_data_seconds: If > 0, keep only the latest N seconds.
        sample_count: Number of samples to capture (optional).
    """

    trim_data_seconds: Optional[float] = None
    sample_count: Optional[int] = None

    def to_dict(self) -> dict:
        d: dict = {}
        if self.trim_data_seconds is not None:
            d["trimDataSeconds"] = self.trim_data_seconds
        if self.sample_count is not None:
            d["sampleCount"] = self.sample_count
        return d


@dataclass
class TimedCaptureMode:
    """Timed capture mode — auto-stop after ``duration_seconds``.

    Args:
        duration_seconds: Capture duration in seconds.
        trim_data_seconds: If > 0, keep only the latest N seconds.
    """

    duration_seconds: float
    trim_data_seconds: Optional[float] = None

    def to_dict(self) -> dict:
        d: dict = {"durationSeconds": self.duration_seconds}
        if self.trim_data_seconds is not None:
            d["trimDataSeconds"] = self.trim_data_seconds
        return d


@dataclass
class DigitalTriggerLinkedChannel:
    """A digital channel that must be HIGH or LOW at trigger time."""

    channel_index: int
    state: DigitalTriggerLinkedChannelState

    def to_dict(self) -> dict:
        return {
            "channelIndex": self.channel_index,
            "state": self.state.value,
        }


@dataclass
class DigitalTriggerCaptureMode:
    """Digital trigger capture mode.

    The capture auto-stops when the trigger condition is met and the
    post-trigger recording length is complete.

    Args:
        trigger_type: Type of trigger (rising/falling/pulse).
        trigger_channel_index: Channel to watch for the trigger.
        min_pulse_width_seconds: Min pulse width (pulse triggers only).
        max_pulse_width_seconds: Max pulse width (pulse triggers only).
        linked_channels: Other channels that must be in a specific state.
        trim_data_seconds: If > 0, keep only the latest N seconds.
        after_trigger_seconds: Seconds to record after triggering.
    """

    trigger_type: DigitalTriggerType
    trigger_channel_index: int
    min_pulse_width_seconds: Optional[float] = None
    max_pulse_width_seconds: Optional[float] = None
    linked_channels: List[DigitalTriggerLinkedChannel] = field(
        default_factory=list
    )
    trim_data_seconds: Optional[float] = None
    after_trigger_seconds: Optional[float] = None

    def to_dict(self) -> dict:
        d: dict = {
            "triggerType": self.trigger_type.value,
            "triggerChannelIndex": self.trigger_channel_index,
        }
        if self.min_pulse_width_seconds is not None:
            d["minPulseWidthSeconds"] = self.min_pulse_width_seconds
        if self.max_pulse_width_seconds is not None:
            d["maxPulseWidthSeconds"] = self.max_pulse_width_seconds
        if self.linked_channels:
            d["linkedChannels"] = [lc.to_dict() for lc in self.linked_channels]
        if self.trim_data_seconds is not None:
            d["trimDataSeconds"] = self.trim_data_seconds
        if self.after_trigger_seconds is not None:
            d["afterTriggerSeconds"] = self.after_trigger_seconds
        return d


CaptureMode = Union[ManualCaptureMode, TimedCaptureMode, DigitalTriggerCaptureMode]


@dataclass
class CaptureConfiguration:
    """Top-level capture configuration.

    Args:
        capture_mode: One of ManualCaptureMode, TimedCaptureMode,
                      or DigitalTriggerCaptureMode.
        buffer_size_megabytes: Max capture buffer size in MB.
    """

    capture_mode: CaptureMode = field(default_factory=ManualCaptureMode)
    buffer_size_megabytes: Optional[int] = None

    def to_dict(self) -> dict:
        d: dict = {}
        if isinstance(self.capture_mode, ManualCaptureMode):
            d["manualCaptureMode"] = self.capture_mode.to_dict()
        elif isinstance(self.capture_mode, TimedCaptureMode):
            d["timedCaptureMode"] = self.capture_mode.to_dict()
        elif isinstance(self.capture_mode, DigitalTriggerCaptureMode):
            d["digitalCaptureMode"] = self.capture_mode.to_dict()
        if self.buffer_size_megabytes is not None:
            d["bufferSizeMegabytes"] = self.buffer_size_megabytes
        return d


# ======================================================================
# Analyzer / decoder configuration
# ======================================================================


@dataclass
class AnalyzerSettingValue:
    """A single analyzer setting value (typed union)."""

    value: Union[str, int, float, bool]

    def to_dict(self) -> dict:
        v = self.value
        if isinstance(v, bool):
            return {"boolValue": v}
        if isinstance(v, str):
            return {"stringValue": v}
        if isinstance(v, int):
            return {"int64Value": v}
        if isinstance(v, float):
            return {"doubleValue": v}
        raise TypeError(f"Unsupported setting value type: {type(v)}")


@dataclass
class AnalyzerHandle:
    """Opaque handle to a created analyzer instance."""

    analyzer_id: str

    def __str__(self) -> str:
        return self.analyzer_id


@dataclass
class DataTableExportConfiguration:
    """Per-analyzer export config for data table CSV.

    Args:
        analyzer: Analyzer handle or ID string.
        radix: Display radix for the export.
    """

    analyzer: Union[AnalyzerHandle, str]
    radix: RadixType = RadixType.HEXADECIMAL

    def to_dict(self) -> dict:
        aid = (
            self.analyzer.analyzer_id
            if isinstance(self.analyzer, AnalyzerHandle)
            else self.analyzer
        )
        return {"analyzerId": aid, "radixType": self.radix.value}


@dataclass
class DataTableFilter:
    """Filter specification for data table export.

    Args:
        query: Query string to filter rows.
        columns: Column names to apply the query to.
    """

    query: str
    columns: List[str] = field(default_factory=list)

    def to_dict(self) -> dict:
        d: dict = {"query": self.query}
        if self.columns:
            d["columns"] = self.columns
        return d


# ======================================================================
# Channel / probe configuration
# ======================================================================


@dataclass
class ChannelInfo:
    """Channel descriptor returned by :meth:`McpClient.get_channels`."""

    index: int
    name: str
    type: ChannelType
    enabled: bool
    enabled_default: Optional[bool] = None

    @classmethod
    def from_dict(cls, d: dict) -> "ChannelInfo":
        return cls(
            index=d.get("index", -1),
            name=d.get("name", ""),
            type=ChannelType(d.get("type", 99)),
            enabled=d.get("enabled", False),
            enabled_default=d.get("enabled_default"),
        )


@dataclass
class ProbeConfig:
    """Probe configuration for an analog/DSO channel.

    Args:
        vdiv: Volts per division.
        coupling: Coupling type.
        vfactor: Voltage probe factor (e.g. 10x = 10).
        map_default: If True, use default mapping.
    """

    vdiv: Optional[float] = None
    coupling: Optional[CouplingType] = None
    vfactor: Optional[float] = None
    map_default: Optional[bool] = None

    def to_dict(self) -> dict:
        d: dict = {}
        if self.vdiv is not None:
            d["vdiv"] = self.vdiv
        if self.coupling is not None:
            d["coupling"] = self.coupling.value
        if self.vfactor is not None:
            d["vfactor"] = self.vfactor
        if self.map_default is not None:
            d["mapDefault"] = self.map_default
        return d

    @classmethod
    def from_dict(cls, d: dict) -> "ProbeConfig":
        coupling = None
        if "coupling" in d and d["coupling"] is not None:
            try:
                coupling = CouplingType(d["coupling"])
            except ValueError:
                coupling = None
        return cls(
            vdiv=d.get("vdiv"),
            coupling=coupling,
            vfactor=d.get("vfactor"),
            map_default=d.get("map_default"),
        )


# ======================================================================
# Sample configuration
# ======================================================================


@dataclass
class SampleConfig:
    """Full sample configuration.

    Returned by :meth:`McpClient.get_sample_config`.
    """

    sample_rate: int = 0
    sample_limit: int = 0
    time_base: int = 0
    collect_mode: CollectMode = CollectMode.SINGLE
    stream_mode: StreamMode = StreamMode.STOP
    rle_enabled: bool = False
    repeat_interval: int = 0
    repeat_hold_percent: float = 0.0

    @classmethod
    def from_dict(cls, d: dict) -> "SampleConfig":
        cm = CollectMode.SINGLE
        if "collect_mode" in d:
            try:
                cm = CollectMode(d["collect_mode"])
            except ValueError:
                pass
        sm = StreamMode.STOP
        if "stream_mode" in d:
            try:
                sm = StreamMode(d["stream_mode"])
            except ValueError:
                pass
        return cls(
            sample_rate=d.get("sample_rate", 0),
            sample_limit=d.get("sample_limit", 0),
            time_base=d.get("time_base", 0),
            collect_mode=cm,
            stream_mode=sm,
            rle_enabled=d.get("rle_enabled", False),
            repeat_interval=d.get("repeat_interval", 0),
            repeat_hold_percent=d.get("repeat_hold_percent", 0.0),
        )


# ======================================================================
# Capture status
# ======================================================================


class CaptureState(Enum):
    """Capture state values."""

    IDLE = "idle"
    CAPTURING = "capturing"
    COMPLETED = "completed"
    PAUSED = "paused"
    ERROR = "error"
    UNKNOWN = "unknown"


@dataclass
class CaptureStatus:
    """Capture status and progress.

    Returned by :meth:`McpClient.get_capture_status`.
    """

    state: CaptureState = CaptureState.UNKNOWN
    progress: float = 0.0
    triggered: bool = False
    elapsed_seconds: float = 0.0
    sample_count: int = 0

    @classmethod
    def from_dict(cls, d: dict) -> "CaptureStatus":
        state = CaptureState.UNKNOWN
        raw = d.get("state", "unknown")
        try:
            state = CaptureState(raw)
        except ValueError:
            pass
        return cls(
            state=state,
            progress=d.get("progress", 0.0),
            triggered=d.get("triggered", False),
            elapsed_seconds=d.get("elapsed_seconds", 0.0),
            sample_count=d.get("sample_count", 0),
        )


# ======================================================================
# Device descriptor
# ======================================================================


@dataclass
class DeviceDesc:
    """Connected device descriptor.

    Returned by :meth:`McpClient.get_devices`.
    """

    id: str
    driver_name: str = ""
    display_name: str = ""
    path: str = ""
    is_hardware: bool = False
    is_demo: bool = False
    is_file: bool = False

    @property
    def device_type(self) -> DeviceType:
        if self.is_demo:
            return DeviceType.DEMO
        if self.is_file:
            return DeviceType.FILE
        if self.is_hardware:
            return DeviceType.UNKNOWN  # refined by driver_name
        return DeviceType.UNKNOWN

    @classmethod
    def from_dict(cls, d: dict) -> "DeviceDesc":
        return cls(
            id=d.get("id", ""),
            driver_name=d.get("driver_name", ""),
            display_name=d.get("display_name", ""),
            path=d.get("path", ""),
            is_hardware=d.get("is_hardware", False),
            is_demo=d.get("is_demo", False),
            is_file=d.get("is_file", False),
        )


# ======================================================================
# App info
# ======================================================================


@dataclass
class Version:
    """Semantic version."""

    major: int = 0
    minor: int = 0
    patch: int = 0

    def __str__(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @classmethod
    def from_dict(cls, d: dict) -> "Version":
        return cls(
            major=d.get("major", 0),
            minor=d.get("minor", 0),
            patch=d.get("patch", 0),
        )


@dataclass
class AppInfo:
    """PXView application information."""

    app_version: str = ""
    app_pid: int = 0
    api_version: Version = field(default_factory=Version)

    @classmethod
    def from_dict(cls, d: dict) -> "AppInfo":
        return cls(
            app_version=d.get("application_version", d.get("app_version", "")),
            app_pid=d.get("launch_pid", d.get("app_pid", 0)),
            api_version=Version.from_dict(
                d.get("api_version", d.get("apiVersion", {}))
            ),
        )
