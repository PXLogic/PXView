"""
setup_demo_session.py - Create a demo .pxc session file with PATTERN_MIXED.

This script:
1. Connects to PXView (headless)
2. Adds 16 decoders (8 C + 8 Python) for all protocol buses
3. Captures 16 channels with PATTERN_MIXED pattern
4. Saves as demo.pxc

Channel layout (PATTERN_MIXED, 16 channels, 8 protocol types):
    ch0-1:   I2C      (SCL, SDA)
    ch2-5:   SPI      (CS, SCLK, MOSI, MISO)
    ch6:     UART     (RX)
    ch7:     CAN      (RX)
    ch8:     PWM      (DATA)
    ch9-11:  I2S      (SCK, WS, SD)
    ch12-13: MIPI DSI (D0N, D0P)
    ch14-15: SWD      (SWDIO, SWCLK)

Each bus gets one C decoder + one Python decoder = 16 decoders total.

Usage:
    python setup_demo_session.py [--output demo.pxc] [--samples 1000000]
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

# Ensure pxview-automation is importable
_pkg_src = Path(__file__).resolve().parent.parent / "pxview-automation" / "src"
if str(_pkg_src) not in sys.path:
    sys.path.insert(0, str(_pkg_src))

from pxview_automation import McpClient, PXViewProcess

# Only 16 channels are used by PATTERN_MIXED
ALL_CHANNELS = list(range(16))

# ---------------------------------------------------------------------------
# Some Python decoders use uppercase IDs that differ from the folder name
# (e.g. folder "mipi_dsi" but decoder id "MIPI_DSI"). srd_decoder_get_by_id()
# is case-sensitive, so we must use the exact id registered by the decoder.
PY_ID_OVERRIDES = {
    "mipi_dsi": "MIPI_DSI",
}

# ---------------------------------------------------------------------------
# Bus definitions: 8 protocol types, each with C + Python decoder.
# ---------------------------------------------------------------------------
BUSES = [
    # I2C: ch0=SCL, ch1=SDA
    {"proto": "i2c", "label": "I2C",
     "channels": {"scl": 0, "sda": 1}},

    # SPI: ch2=CS, ch3=CLK, ch4=MOSI, ch5=MISO
    {"proto": "spi", "label": "SPI",
     "channels": {"cs": 2, "clk": 3, "mosi": 4, "miso": 5}},

    # UART: ch6=RX
    {"proto": "uart", "label": "UART",
     "channels": {"rx": 6}},

    # CAN: ch7=RX (decoder channel id is "can_rx")
    {"proto": "can", "label": "CAN",
     "channels": {"can_rx": 7}},

    # PWM: ch8=DATA
    {"proto": "pwm", "label": "PWM",
     "channels": {"data": 8}},

    # I2S: ch9=SCK, ch10=WS, ch11=SD
    {"proto": "i2s", "label": "I2S",
     "channels": {"sck": 9, "ws": 10, "sd": 11}},

    # MIPI DSI: ch12=D0N, ch13=D0P
    {"proto": "mipi_dsi", "label": "MIPI DSI",
     "channels": {"D0N": 12, "D0P": 13}},

    # SWD: ch14=SWDIO, ch15=SWCLK
    # Note: SWD decoder channels are swclk(0), swdio(1)
    {"proto": "swd", "label": "SWD",
     "channels": {"swdio": 14, "swclk": 15}},
]


def setup_demo_session(output_path: str, sample_count: int = 1_000_000,
                        sample_rate: int = 1_000_000):
    """Create a demo session with PATTERN_MIXED and 16 decoders (C+Python)."""
    mcp_url = os.environ.get("PXVIEW_MCP_URL", "http://127.0.0.1:10110/mcp")
    mcp_port = int(os.environ.get("PXVIEW_MCP_PORT", "10110"))
    ws_port = int(os.environ.get("PXVIEW_WS_PORT", "10430"))
    startup_timeout = float(os.environ.get("PXVIEW_STARTUP_TIMEOUT", "120"))

    # Connect or start PXView
    proc = None
    probe = McpClient(url=mcp_url, timeout=5.0, max_retries=1)
    if not probe.wait_for_server(timeout=3.0, interval=0.5):
        print("Starting PXView headless...")
        proc = PXViewProcess(
            port=mcp_port, ws_port=ws_port,
            startup_timeout=startup_timeout,
        )
        proc.start()
    else:
        print("Connecting to existing PXView...")

    client = McpClient(url=mcp_url, timeout=120.0, max_retries=5, retry_delay=1.0)
    if not client.wait_for_server(timeout=startup_timeout, interval=1.0):
        print("ERROR: Cannot connect to PXView MCP server")
        if proc:
            proc.stop()
        sys.exit(1)
    client.connect()
    assert client.connected, "MCP connect failed"
    print(f"Connected to PXView (tools={len(client.tools)})")

    try:
        # Get demo device
        device = client.get_demo_device()
        device_id = device["id"]
        print(f"Demo device: {device_id}")

        # Connect device
        try:
            client.connect_device(device_id)
        except Exception:
            pass

        # Clear any existing state
        try:
            client.clear_all_decoders()
        except Exception:
            pass
        try:
            client.close_capture()
        except Exception:
            pass

        # Add decoders BEFORE capture so auto-decode triggers.
        # Each bus gets one C decoder + one Python decoder = 16 total.
        decoder_count = 0
        for bus in BUSES:
            proto = bus["proto"]
            label = bus["label"]
            channels = bus["channels"]

            # --- C decoder ---
            c_name = f"{proto}_c"
            try:
                client.add_analyzer(
                    analyzer_name=c_name,
                    settings={"channelMap": channels},
                )
                decoder_count += 1
                ch_str = ", ".join(f"{k}=ch{v}" for k, v in channels.items())
                print(f"  Added {label} [C] ({c_name}): {ch_str}")
            except Exception as e:
                print(f"  FAILED {label} [C] ({c_name}): {e}")

            # --- Python decoder ---
            py_name = PY_ID_OVERRIDES.get(proto, proto)
            try:
                client.add_analyzer(
                    analyzer_name=py_name,
                    settings={"channelMap": channels},
                )
                decoder_count += 1
                ch_str = ", ".join(f"{k}=ch{v}" for k, v in channels.items())
                print(f"  Added {label} [PY] ({py_name}): {ch_str}")
            except Exception as e:
                print(f"  FAILED {label} [PY] ({py_name}): {e}")

        print(f"\nTotal decoders added: {decoder_count}/{len(BUSES) * 2}")

        # Capture with PATTERN_MIXED
        print(f"\nStarting capture: {sample_count} samples "
              f"@ {sample_rate} Hz, pattern=mixed")
        logic_config = {
            "digitalChannels": ALL_CHANNELS,
            "digitalSampleRate": sample_rate,
            "pattern": "mixed",
        }
        capture_config = {
            "manualCaptureMode": {"sampleCount": sample_count},
        }
        client.start_capture(
            device_id=device_id,
            logic_device_configuration=logic_config,
            capture_configuration=capture_config,
        )

        wait_timeout = max(sample_count / sample_rate * 3, 30)
        client.wait_capture(timeout_seconds=wait_timeout, timeout=wait_timeout + 10)
        time.sleep(1)
        status = client.get_capture_status()
        print(f"Capture complete: {status.get('state', 'unknown')}")

        # Save as .pxc
        print(f"\nSaving session to: {output_path}")
        client.save_capture(output_path)
        if os.path.exists(output_path):
            size_mb = os.path.getsize(output_path) / (1024 * 1024)
            print(f"Saved: {output_path} ({size_mb:.1f} MB)")
        else:
            print(f"ERROR: File not created: {output_path}")

        # Verify decoders produced results
        print("\nDecoder results:")
        decoders = client.get_active_decoders()
        total_annotations = 0
        for d in decoders:
            aid = d.get("instance_id") or d.get("id")
            name = d.get("decoder_id") or d.get("name", "?")
            try:
                result = client.get_analyzer_results(analyzer_id=aid, max_count=10)
                if isinstance(result, dict):
                    count = len(result.get("annotations", []))
                else:
                    count = len(result) if result else 0
                total_annotations += count
                status_str = "OK" if count > 0 else "EMPTY"
                print(f"  [{status_str}] {name} (id={aid}): {count} annotations")
            except Exception as e:
                print(f"  [ERR]  {name} (id={aid}): {e}")

        print(f"\nTotal annotations (max 10/decoder): {total_annotations}")

    finally:
        try:
            client.clear_all_decoders()
        except Exception:
            pass
        try:
            client.close_capture()
        except Exception:
            pass
        client.disconnect()
        if proc:
            proc.stop()

    print("\nDone!")


def main():
    parser = argparse.ArgumentParser(
        description="Create a demo .pxc session with PATTERN_MIXED (16 decoders: 8 C + 8 Python)"
    )
    parser.add_argument(
        "--output", "-o",
        default="demo.pxc",
        help="Output .pxc file path (default: demo.pxc)",
    )
    parser.add_argument(
        "--samples", "-n",
        type=int, default=1_000_000,
        help="Number of samples to capture (default: 1000000)",
    )
    parser.add_argument(
        "--rate", "-r",
        type=int, default=1_000_000,
        help="Sample rate in Hz (default: 1000000)",
    )
    args = parser.parse_args()

    setup_demo_session(
        output_path=args.output,
        sample_count=args.samples,
        sample_rate=args.rate,
    )


if __name__ == "__main__":
    main()
