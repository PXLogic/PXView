# Changelog

All notable changes to this project will be documented in this file.

## [1.5.5] - 2026-08-08

### Added
- Initial release as a standalone pip package (formerly `pxview-mcp`, renamed to `pxview-automation`).
- `McpClient` — low-level client wrapping all 61 MCP tools (ported from `tests/mcp_client.py`).
- `PXView` — high-level API combining multiple tools into domain operations (`capture`, `decode`, `capture_and_decode`, `export`, etc.).
- `PXViewProcess` — context manager to auto-start/stop PXView `--headless`.
- `pxview-cli` — command-line tool with subcommands: `list-devices`, `scan`, `channels`, `capture`, `decode`, `results`, `export`, `export-table`, `samples`, `status`, `save`, `load`, `run`, `list-decoders`.
- Zero runtime dependencies (Python standard library only).
- Comprehensive documentation: API reference, CLI reference, examples.
- Unit tests with mocked server (no PXView required).
- Context manager support (`with` statement) for `McpClient` and `PXView`.
- SSE (Server-Sent Events) response parsing for `wait_capture`.
- Automatic MSYS2/Cygwin path conversion for Windows.
- Duration parsing (`1s`, `500ms`, `2m`) and sample rate parsing (`1M`, `100K`).
