"""手动验证：统一 export_raw_data 工具支持 csv/binary/vcd/hex/bits 多格式导出。"""
import os, sys, time, json
sys.path.insert(0, os.path.dirname(__file__))
from mcp_client import McpClient

URL = "http://127.0.0.1:10110"
TMP = "c:/tmp/fmt_verify"
os.makedirs(TMP, exist_ok=True)

def list_files(d):
    return sorted(f for f in os.listdir(d) if not f.startswith("."))

def main():
    c = McpClient(URL)
    c.connect()
    try:
        c.stop_capture()
    except Exception:
        pass

    print("== start_capture (graycode) ==")
    print(c.start_capture(
        device_id="1",
        logic_device_configuration={
            "digitalChannels": [0, 1, 2, 3],
            "sampleRate": 1000000,
            "pattern": "graycode",
        },
        capture_configuration={"sampleCount": 50000},
    ))
    for _ in range(30):
        st = c.get_capture_status()
        if st.get("have_view_data"):
            break
        time.sleep(0.5)
    print("state:", st.get("state"), "view_data:", st.get("have_view_data"))

    for fmt in ["csv", "binary", "vcd", "hex", "bits"]:
        d = os.path.join(TMP, fmt)
        os.makedirs(d, exist_ok=True)
        for f in list_files(d):
            os.remove(os.path.join(d, f))
        print(f"\n== export_raw_data format={fmt} ==")
        try:
            r = c.export_raw_data(fmt, d, digital_channels=[0, 1, 2, 3])
            print("  result:", r)
        except Exception as e:
            print("  EXCEPTION:", e)
            continue
        files = list_files(d)
        print("  files:", files)
        for f in files:
            p = os.path.join(d, f)
            print(f"    {f}: {os.path.getsize(p)} bytes")

    c.disconnect()

if __name__ == "__main__":
    main()
