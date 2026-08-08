"""Command-line interface for PXView automation.

Usage::

    pxview-cli list-devices
    pxview-cli capture --device demo --channels 0,1 --rate 1M --time 1s
    pxview-cli decode --protocol i2c --scl 0 --sda 1
    pxview-cli samples --channel 0 --start 0 --count 100
    pxview-cli export --format csv --dir ./output
    pxview-cli run --device demo --channels 0,1 --rate 1M --time 1s \\
        --protocol i2c --scl 0 --sda 1 --export csv:./output

Global options::

    --host HOST       MCP server host (default: 127.0.0.1)
    --port PORT       MCP server port (default: 10110)
    --timeout SECS    HTTP timeout (default: 60)
    --json            Output raw JSON (for scripting)
    --auto-start      Auto-start PXView --headless if not reachable
    --exe PATH        Path to PXView executable (for --auto-start)
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any, List, Optional

from .client import McpClient
from .exceptions import McpConnectionError, McpError, PxvError
from .highlevel import PXView
from ._utils import parse_duration, parse_int_list


# ======================================================================
# Argument parser construction
# ======================================================================

def build_parser() -> argparse.ArgumentParser:
    """Build the top-level argument parser."""
    parser = argparse.ArgumentParser(
        prog="pxview-cli",
        description="PXView logic analyzer automation CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  pxview-cli list-devices
  pxview-cli capture --device demo --channels 0,1 --rate 1000000 --time 1s
  pxview-cli decode --protocol i2c --scl 0 --sda 1
  pxview-cli export --format csv --dir ./output
  pxview-cli samples --channel 0 --start 0 --count 1000
  pxview-cli run --device demo --channels 0,1 --rate 1M --time 1s \\
      --protocol i2c --scl 0 --sda 1 --export csv:./output
""",
    )

    parser.add_argument(
        "--host", default="127.0.0.1", help="MCP server host (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--port", type=int, default=10110, help="MCP server port (default: 10110)"
    )
    parser.add_argument(
        "--timeout", type=float, default=60.0, help="HTTP timeout in seconds"
    )
    parser.add_argument(
        "--json", action="store_true", help="Output raw JSON (for scripting)"
    )
    parser.add_argument(
        "--auto-start",
        action="store_true",
        help="Auto-start PXView --headless if server is not reachable",
    )
    parser.add_argument(
        "--exe", default=None, help="Path to PXView executable (for --auto-start)"
    )

    subparsers = parser.add_subparsers(dest="command", help="Sub-command")

    # ---- list-devices ----
    subparsers.add_parser("list-devices", help="List connected devices")

    # ---- scan ----
    subparsers.add_parser("scan", help="Hot-plug rescan for devices")

    # ---- channels ----
    subparsers.add_parser("channels", help="List channels of current device")

    # ---- capture ----
    p_cap = subparsers.add_parser("capture", help="Start a capture")
    p_cap.add_argument("--device", required=True, help="Device ID")
    p_cap.add_argument("--channels", default=None, help="Digital channels (e.g. 0,1,2-4)")
    p_cap.add_argument("--analog-channels", default=None, help="Analog channels")
    p_cap.add_argument("--rate", default=None, help="Sample rate in Hz (e.g. 1M, 100K)")
    p_cap.add_argument("--time", default=None, help="Capture duration (e.g. 1s, 500ms)")
    p_cap.add_argument("--samples", type=int, default=None, help="Sample count")
    p_cap.add_argument("--threshold", type=float, default=None, help="Threshold voltage")
    p_cap.add_argument("--trigger", default=None, help="Trigger spec: channel:type (e.g. 0:rising)")
    p_cap.add_argument("--no-wait", action="store_true", help="Don't wait for completion")

    # ---- decode ----
    p_dec = subparsers.add_parser("decode", help="Add a protocol decoder")
    p_dec.add_argument("--protocol", required=True, help="Decoder name (e.g. i2c, spi, uart)")
    p_dec.add_argument("--device", default=None, help="Device ID (for headless mode)")
    p_dec.add_argument(
        "--channel-map",
        action="append",
        default=[],
        help="Channel mapping: name=index (e.g. --channel-map scl=0 --channel-map sda=1)",
    )
    p_dec.add_argument(
        "--option",
        action="append",
        default=[],
        help="Decoder option: key=value (e.g. --option baudrate=115200)",
    )

    # ---- results ----
    p_res = subparsers.add_parser("results", help="Get decoder results")
    p_res.add_argument("--analyzer-id", required=True, help="Analyzer instance ID")
    p_res.add_argument("--max", type=int, default=1000, help="Max annotations")

    # ---- export ----
    p_exp = subparsers.add_parser("export", help="Export raw capture data")
    p_exp.add_argument("--format", default="csv", choices=["csv", "binary", "vcd", "hex", "bits"])
    p_exp.add_argument("--dir", required=True, help="Output directory")
    p_exp.add_argument("--digital-channels", default=None, help="Digital channels to export")
    p_exp.add_argument("--analog-channels", default=None, help="Analog channels to export")

    # ---- export-table ----
    p_et = subparsers.add_parser("export-table", help="Export decoder results as CSV table")
    p_et.add_argument("--analyzer-id", default=None, help="Specific analyzer (default: all)")
    p_et.add_argument("--out", required=True, help="Output CSV file path")

    # ---- samples ----
    p_smp = subparsers.add_parser("samples", help="Read raw samples")
    p_smp.add_argument("--channel", type=int, required=True, help="Channel index")
    p_smp.add_argument("--start", type=int, default=0, help="Start sample index")
    p_smp.add_argument("--count", type=int, default=None, help="Number of samples")
    p_smp.add_argument(
        "--type", default="logic", choices=["logic", "analog", "dso"], help="Sample type"
    )
    p_smp.add_argument(
        "--format", default="hex", choices=["hex", "bin", "dec"], help="Output format"
    )

    # ---- status ----
    subparsers.add_parser("status", help="Get capture status")

    # ---- save ----
    p_save = subparsers.add_parser("save", help="Save capture to .pxc file")
    p_save.add_argument("--out", required=True, help="Output .pxc file path")

    # ---- load ----
    p_load = subparsers.add_parser("load", help="Load capture from .pxc file")
    p_load.add_argument("--file", required=True, help="Input .pxc file path")

    # ---- run (all-in-one) ----
    p_run = subparsers.add_parser("run", help="Capture + decode + export in one command")
    p_run.add_argument("--device", required=True, help="Device ID")
    p_run.add_argument("--channels", default=None, help="Digital channels")
    p_run.add_argument("--rate", default=None, help="Sample rate")
    p_run.add_argument("--time", default=None, help="Capture duration")
    p_run.add_argument("--samples", type=int, default=None, help="Sample count")
    p_run.add_argument("--protocol", default=None, help="Decoder protocol")
    p_run.add_argument(
        "--channel-map",
        action="append",
        default=[],
        help="Channel mapping: name=index",
    )
    p_run.add_argument(
        "--option", action="append", default=[], help="Decoder option: key=value"
    )
    p_run.add_argument(
        "--export",
        action="append",
        default=[],
        help="Export spec: format:path (e.g. csv:./output)",
    )

    # ---- list-decoders ----
    subparsers.add_parser("list-decoders", help="List available protocol decoders")

    return parser


# ======================================================================
# Helpers
# ======================================================================

def _parse_rate(s: str) -> int:
    """Parse a sample rate string like '1M', '100K', '5000000'."""
    s = s.strip()
    if not s:
        raise ValueError("Empty rate")
    suffix = s[-1].upper()
    if suffix in ("K", "M", "G"):
        num = float(s[:-1])
        mult = {"K": 1e3, "M": 1e6, "G": 1e9}[suffix]
        return int(num * mult)
    return int(s)


def _parse_channel_map(items: List[str]) -> dict:
    """Parse a list of 'name=index' strings into a dict."""
    result = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid channel map entry: {item!r} (expected name=index)")
        name, idx = item.split("=", 1)
        result[name.strip()] = int(idx.strip())
    return result


def _parse_options(items: List[str]) -> dict:
    """Parse a list of 'key=value' strings into a dict."""
    result = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"Invalid option entry: {item!r} (expected key=value)")
        key, val = item.split("=", 1)
        result[key.strip()] = val.strip()
    return result


def _print(data: Any, as_json: bool = False) -> None:
    """Print data in human-readable or JSON format."""
    if as_json:
        print(json.dumps(data, indent=2, default=str))
    elif isinstance(data, (list, dict)):
        print(json.dumps(data, indent=2, default=str))
    else:
        print(data)


def _connect_client(args: argparse.Namespace) -> McpClient:
    """Connect to the MCP server, optionally auto-starting PXView."""
    client = McpClient(
        url=f"http://{args.host}:{args.port}/mcp",
        timeout=args.timeout,
    )

    # Try to connect
    if not client.wait_for_server(timeout=3.0):
        if args.auto_start:
            from .process import PXViewProcess

            proc = PXViewProcess(exe_path=args.exe, port=args.port)
            proc.start()
            # Store proc on client so it stays alive and gets cleaned up
            client._pxv_process = proc  # type: ignore[attr-defined]
            if not client.wait_for_server(timeout=30.0):
                raise McpConnectionError(
                    f"MCP server at {args.host}:{args.port} not reachable "
                    "even after auto-start."
                )
        else:
            raise McpConnectionError(
                f"Cannot connect to MCP server at {args.host}:{args.port}. "
                "Is PXView running with --headless? "
                "Use --auto-start to launch it automatically."
            )

    client.connect()
    return client


# ======================================================================
# Command handlers
# ======================================================================

def cmd_list_devices(client: McpClient, args: argparse.Namespace) -> None:
    devices = client.get_devices()
    if args.json:
        print(json.dumps(devices, indent=2))
        return
    if not devices:
        print("No devices found.")
        return
    print(f"{'ID':<30} {'Driver':<15} {'Name':<25} {'Type'}")
    print("-" * 80)
    for d in devices:
        dev_type = "demo" if d.get("is_demo") else ("hardware" if d.get("is_hardware") else "file")
        print(f"{d['id']:<30} {d.get('driver_name',''):<15} {d.get('display_name',''):<25} {dev_type}")


def cmd_scan(client: McpClient, args: argparse.Namespace) -> None:
    devices = client.refresh_device_list()
    _print(devices, args.json)


def cmd_channels(client: McpClient, args: argparse.Namespace) -> None:
    channels = client.get_channels()
    if args.json:
        print(json.dumps(channels, indent=2))
        return
    if not channels:
        print("No channels (device not connected?).")
        return
    print(f"{'Index':<8} {'Name':<15} {'Type':<10} {'Enabled'}")
    print("-" * 45)
    type_names = {0: "Logic", 1: "Analog", 2: "DSO", 99: "Unknown"}
    for ch in channels:
        print(
            f"{ch['index']:<8} {ch.get('name',''):<15} "
            f"{type_names.get(ch.get('type',99),'?'):<10} "
            f"{'✓' if ch.get('enabled') else '✗'}"
        )


def cmd_capture(client: McpClient, args: argparse.Namespace) -> None:
    pxv = PXView.__new__(PXView)
    pxv._client = client

    channels = parse_int_list(args.channels) if args.channels else None
    analog_channels = parse_int_list(args.analog_channels) if args.analog_channels else None
    rate = _parse_rate(args.rate) if args.rate else None
    duration_s = parse_duration(args.time) if args.time else None

    trigger_channel = None
    trigger_type = None
    if args.trigger:
        parts = args.trigger.split(":")
        if len(parts) == 2:
            trigger_channel = int(parts[0])
            trigger_type = parts[1]

    status = pxv.capture(
        device_id=args.device,
        channels=channels,
        analog_channels=analog_channels,
        sample_rate=rate,
        duration_s=duration_s,
        sample_count=args.samples,
        threshold_v=args.threshold,
        instant=False,
        trigger_channel=trigger_channel,
        trigger_type=trigger_type,
        wait=not args.no_wait,
    )
    _print(status, args.json)


def cmd_decode(client: McpClient, args: argparse.Namespace) -> None:
    channel_map = _parse_channel_map(args.channel_map)
    options = _parse_options(args.option)

    result = client.add_analyzer(
        analyzer_name=args.protocol,
        settings={
            "channelMap": channel_map,
            "options": options,
        } if channel_map or options else None,
        device_id=args.device,
    )
    _print(result, args.json)


def cmd_results(client: McpClient, args: argparse.Namespace) -> None:
    results = client.get_analyzer_results(
        analyzer_id=args.analyzer_id,
        max_count=args.max,
    )
    if args.json:
        print(json.dumps(results, indent=2, default=str))
        return
    if not results:
        print("No annotations.")
        return
    for ann in results:
        texts = ann.get("texts") or ann.get("data") or []
        if isinstance(texts, list):
            text = " | ".join(str(t) for t in texts)
        else:
            text = str(texts)
        start = ann.get("start_sample", ann.get("startSample", "?"))
        end = ann.get("end_sample", ann.get("endSample", "?"))
        print(f"[{start}-{end}] {text}")


def cmd_export(client: McpClient, args: argparse.Namespace) -> None:
    digital = parse_int_list(args.digital_channels) if args.digital_channels else None
    analog = parse_int_list(args.analog_channels) if args.analog_channels else None
    result = client.export_raw_data(
        format=args.format,
        directory=args.dir,
        digital_channels=digital,
        analog_channels=analog,
    )
    _print(result or "Export complete.", args.json)


def cmd_export_table(client: McpClient, args: argparse.Namespace) -> None:
    analyzers = [{"analyzerId": args.analyzer_id}] if args.analyzer_id else None
    result = client.export_data_table_csv(
        filepath=args.out,
        analyzers=analyzers,
    )
    _print(result or "Table export complete.", args.json)


def cmd_samples(client: McpClient, args: argparse.Namespace) -> None:
    count = args.count
    end = args.start + count if count else None

    if args.type == "logic":
        data = client.get_logic_samples(
            channel_index=args.channel,
            start_sample=args.start,
            end_sample=end,
        )
        if args.json:
            print(json.dumps({"data": data.hex(), "count": len(data)}))
        elif args.format == "hex":
            for i, b in enumerate(data):
                print(f"[{args.start + i}] 0x{b:02X}")
        elif args.format == "bin":
            for i, b in enumerate(data):
                print(f"[{args.start + i}] {b:08b}")
        else:
            for i, b in enumerate(data):
                print(f"[{args.start + i}] {b}")

    elif args.type == "analog":
        data = client.get_analog_samples(
            channel_index=args.channel,
            start_sample=args.start,
            end_sample=end,
        )
        if args.json:
            print(json.dumps(data))
        else:
            for i, v in enumerate(data):
                print(f"[{args.start + i}] {v}")

    elif args.type == "dso":
        data = client.get_dso_samples(
            channel_index=args.channel,
            start_sample=args.start,
            end_sample=end,
        )
        if args.json:
            print(json.dumps(data))
        else:
            for i, v in enumerate(data):
                print(f"[{args.start + i}] {v}")


def cmd_status(client: McpClient, args: argparse.Namespace) -> None:
    status = client.get_capture_status()
    _print(status, args.json)


def cmd_save(client: McpClient, args: argparse.Namespace) -> None:
    result = client.save_capture(filepath=args.out)
    _print(result or "Save complete.", args.json)


def cmd_load(client: McpClient, args: argparse.Namespace) -> None:
    result = client.load_capture(filepath=args.file)
    _print(result or "Load complete.", args.json)


def cmd_list_decoders(client: McpClient, args: argparse.Namespace) -> None:
    decoders = client.list_analyzers()
    if args.json:
        print(json.dumps(decoders, indent=2))
        return
    if not decoders:
        print("No decoders available.")
        return
    print(f"{'Name':<20} {'Long Name':<35} {'Channels'}")
    print("-" * 70)
    for d in decoders:
        print(
            f"{d.get('id',''):<20} {d.get('long_name',''):<35} "
            f"{d.get('channels',0)}"
        )


def cmd_run(client: McpClient, args: argparse.Namespace) -> None:
    """All-in-one: capture + decode + export."""
    pxv = PXView.__new__(PXView)
    pxv._client = client

    channels = parse_int_list(args.channels) if args.channels else None
    rate = _parse_rate(args.rate) if args.rate else None
    duration_s = parse_duration(args.time) if args.time else None

    # Add decoder if specified (before capture for auto-decode)
    analyzer_id = None
    if args.protocol:
        channel_map = _parse_channel_map(args.channel_map)
        options = _parse_options(args.option)
        all_channels = set(channels or [])
        all_channels.update(channel_map.values())
        channels = sorted(all_channels)
        analyzer_id = pxv.add_decoder(
            protocol=args.protocol,
            channel_map=channel_map,
            options=options,
            device_id=args.device,
        )
        print(f"Added decoder: {args.protocol} → {analyzer_id}")

    # Capture
    print(f"Starting capture on {args.device}...")
    status = pxv.capture(
        device_id=args.device,
        channels=channels,
        sample_rate=rate,
        duration_s=duration_s,
        sample_count=args.samples,
        wait=True,
    )
    print(f"Capture complete: {status.get('state', 'unknown')}")

    # Export
    for spec in args.export:
        parts = spec.split(":", 1)
        fmt = parts[0]
        path = parts[1] if len(parts) > 1 else "."
        print(f"Exporting {fmt} → {path}")
        pxv.export(format=fmt, directory=path)

    # Print decoder results
    if analyzer_id:
        import time
        time.sleep(0.5)
        results = pxv.get_decoder_results(analyzer_id)
        print(f"\nDecoded {len(results)} annotations:")
        for ann in results[:20]:
            texts = ann.get("texts") or ann.get("data") or []
            if isinstance(texts, list):
                text = " | ".join(str(t) for t in texts)
            else:
                text = str(texts)
            start = ann.get("start_sample", ann.get("startSample", "?"))
            end = ann.get("end_sample", ann.get("endSample", "?"))
            print(f"  [{start}-{end}] {text}")
        if len(results) > 20:
            print(f"  ... and {len(results) - 20} more")


# ======================================================================
# Command dispatch
# ======================================================================

_COMMAND_MAP = {
    "list-devices": cmd_list_devices,
    "scan": cmd_scan,
    "channels": cmd_channels,
    "capture": cmd_capture,
    "decode": cmd_decode,
    "results": cmd_results,
    "export": cmd_export,
    "export-table": cmd_export_table,
    "samples": cmd_samples,
    "status": cmd_status,
    "save": cmd_save,
    "load": cmd_load,
    "run": cmd_run,
    "list-decoders": cmd_list_decoders,
}


def main(argv: Optional[List[str]] = None) -> int:
    """CLI entry point.

    Args:
        argv: Command-line arguments (default: ``sys.argv[1:]``).

    Returns:
        Exit code: 0 = success, 1 = error.
    """
    parser = build_parser()
    args = parser.parse_args(argv)

    if not args.command:
        parser.print_help()
        return 0

    handler = _COMMAND_MAP.get(args.command)
    if handler is None:
        print(f"Unknown command: {args.command}", file=sys.stderr)
        return 1

    try:
        client = _connect_client(args)
        handler(client, args)
        client.disconnect()
        return 0
    except McpConnectionError as exc:
        print(f"Connection error: {exc}", file=sys.stderr)
        return 1
    except McpError as exc:
        print(f"MCP error: {exc}", file=sys.stderr)
        return 1
    except PxvError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"Unexpected error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
