# API 参考

## McpClient

低层 MCP 客户端，封装全部 61 个 MCP 工具。

### 构造

```python
McpClient(
    url="http://127.0.0.1:10110/mcp",
    timeout=60.0,
    max_retries=3,
    retry_delay=0.5,
    auto_connect=False,
)
```

### 连接管理

| 方法 | 说明 |
|------|------|
| `connect()` | 初始化 MCP 连接（initialize → list tools） |
| `disconnect()` | 断开连接 |
| `connected` | 是否已连接（属性） |
| `ping()` | 发送 ping，返回 True/False |
| `wait_for_server(timeout, interval)` | 等待服务器可达 |

### 1. 设备管理（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_devices(include_simulation_devices)` | `get_devices` | 列出设备 |
| `get_channels()` | `get_channels` | 列出通道 |

### 2. 采集控制（4 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `start_capture(device_id, logic_device_configuration, capture_configuration)` | `start_capture` | 启动采集 |
| `stop_capture()` | `stop_capture` | 停止采集 |
| `wait_capture(timeout_seconds)` | `wait_capture` | 等待采集完成（SSE 流式） |
| `get_capture_status()` | `get_capture_status` | 获取采集状态 |

### 3. 文件操作（3 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `load_capture(filepath)` | `load_capture` | 加载 .pxc 文件 |
| `save_capture(filepath)` | `save_capture` | 保存为 .pxc 文件 |
| `close_capture()` | `close_capture` | 关闭采集 |

### 4. 协议解码（5 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `list_analyzers()` | `list_analyzers` | 列出可用解码器 |
| `get_analyzer_options(analyzer_name)` | `get_analyzer_options` | 获取解码器选项 |
| `add_analyzer(analyzer_name, settings, device_id, ...)` | `add_analyzer` | 添加解码器 |
| `remove_analyzer(analyzer_id)` | `remove_analyzer` | 移除解码器 |
| `get_analyzer_results(analyzer_id, start_sample, end_sample, max_count)` | `get_analyzer_results` | 获取解码结果 |

### 5. 数据导出（4 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `export_raw_data_csv(directory, ...)` | `export_raw_data_csv` | 导出 CSV |
| `export_raw_data_binary(directory, ...)` | `export_raw_data_binary` | 导出二进制 |
| `export_raw_data(format, directory, ...)` | `export_raw_data` | 多格式导出（csv/binary/vcd/hex/bits） |
| `export_data_table_csv(filepath, analyzers, ...)` | `export_data_table_csv` | 导出解码表 CSV |

### 6. 触发配置（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_trigger_config(mode)` | `get_trigger_config` | 获取触发配置 |
| `set_trigger_config(mode, **kwargs)` | `set_trigger_config` | 设置触发配置 |

### 7. 探头配置（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_probe_config(channel_index)` | `get_probe_config` | 获取探头配置 |
| `set_probe_config(channel_index, vdiv, coupling, ...)` | `set_probe_config` | 设置探头配置 |

### 8. 通道配置（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `set_channel_enabled(channel_index, enabled)` | `set_channel_enabled` | 启用/禁用通道 |
| `set_channel_name(channel_index, name)` | `set_channel_name` | 重命名通道 |

### 9. 采样配置（5 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_sample_config()` | `get_sample_config` | 获取采样配置 |
| `set_sample_rate(rate)` | `set_sample_rate` | 设置采样率（Hz） |
| `set_sample_limit(limit)` | `set_sample_limit` | 设置采样深度 |
| `set_time_base(time_base)` | `set_time_base` | 设置时基（ns） |
| `set_collect_mode(mode)` | `set_collect_mode` | 设置采集模式 |
| `set_repeat_interval(interval_ms)` | `set_repeat_interval` | 设置重复间隔 |

### 10. 样本读取（3 个工具）

| 方法 | MCP Tool | 返回类型 | 说明 |
|------|----------|----------|------|
| `get_logic_samples(channel_index, start_sample, end_sample)` | `get_logic_samples` | `bytes` | 读逻辑样本（每字节 0/1） |
| `get_analog_samples(channel_index, ...)` | `get_analog_samples` | `List[float]` | 读模拟样本 |
| `get_dso_samples(channel_index, ...)` | `get_dso_samples` | `List[float]` | 读 DSO 样本 |

### 11. 边沿/模式搜索（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `find_next_edge(channel_index, from_sample, rising_edge)` | `find_next_edge` | 查找下一个边沿 |
| `find_pattern(channel_index, pattern, from_sample)` | `find_pattern` | 搜索位模式 |

### 12. 解码器管理（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_active_decoders()` | `get_active_decoders` | 列出活跃解码器 |
| `clear_all_decoders()` | `clear_all_decoders` | 清除所有解码器 |

### 13. 会话管理（5 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `list_sessions()` | `list_sessions` | 列出会话 |
| `create_session(name, device_id, file_path)` | `create_session` | 创建会话 |
| `destroy_session(session_id)` | `destroy_session` | 销毁会话 |
| `set_active_session(session_id)` | `set_active_session` | 切换会话 |
| `get_session_count()` | `get_session_count` | 获取会话数 |

### 14. 设备连接（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `connect_device(device_id)` | `connect_device` | 连接设备 |
| `disconnect_device(device_id)` | `disconnect_device` | 断开设备 |

### 15. 通用配置（2 个工具）

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `get_config(key, value_type)` | `get_config` | 读取 SR_CONF_* 配置 |
| `set_config(key, value_type, value)` | `set_config` | 写入 SR_CONF_* 配置 |

### 16-20. 毛刺滤波/信号反转/重复状态/磁盘缓存/扩展工具

| 方法 | MCP Tool | 说明 |
|------|----------|------|
| `set_glitch_filter(channels, threshold, ...)` | `set_glitch_filter` | 设置毛刺滤波 |
| `clear_glitch_filter(channels)` | `clear_glitch_filter` | 清除毛刺滤波 |
| `get_glitch_filter_config()` | `get_glitch_filter_config` | 获取毛刺滤波配置 |
| `set_signal_invert(channels)` | `set_signal_invert` | 设置信号反转 |
| `clear_signal_invert(channels)` | `clear_signal_invert` | 清除信号反转 |
| `get_signal_invert_config()` | `get_signal_invert_config` | 获取信号反转配置 |
| `get_repeat_status()` | `get_repeat_status` | 获取重复状态 |
| `get_disk_cache_info()` | `get_disk_cache_info` | 获取磁盘缓存信息 |
| `refresh_device_list()` | `refresh_device_list` | 热插拔扫描 |
| `set_save_range(start_sample, end_sample)` | `set_save_range` | 设置保存范围 |
| `reconfigure_decoder(analyzer_id, options, channel_map)` | `reconfigure_decoder` | 重新配置解码器 |
| `get_decoder_class_names(analyzer_name)` | `get_decoder_class_names` | 获取解码器类名 |
| `get_decoder_binary_output(analyzer_id, output_id)` | `get_decoder_binary_output` | 读取二进制输出 |
| `get_math_results()` | `get_math_results` | 读取数学运算结果 |
| `get_spectrum_results()` | `get_spectrum_results` | 读取 FFT 频谱结果 |
| `get_lissajous_results()` | `get_lissajous_results` | 读取李萨如图形配置 |
| `get_error_state()` | `get_error_state` | 读取错误状态 |
| `clear_error_state()` | `clear_error_state` | 清除错误状态 |

---

## PXView（高层 API）

### 构造

```python
PXView(host="127.0.0.1", port=10110, timeout=60.0, auto_connect=False)
```

### 方法

| 方法 | 说明 |
|------|------|
| `connect()` / `disconnect()` | 连接/断开 MCP |
| `list_devices(include_sim)` | 列出设备 |
| `find_device(demo, hardware, driver)` | 查找设备 |
| `scan_devices()` | 热插拔扫描 |
| `capture(device_id, channels, sample_rate, duration_s, ...)` | 采集 |
| `stop_capture()` | 停止采集 |
| `get_status()` | 获取采集状态 |
| `list_decoders()` | 列出可用解码器 |
| `add_decoder(protocol, channel_map, options, ...)` | 添加解码器 |
| `get_decoder_results(analyzer_id, max_count)` | 获取解码结果 |
| `clear_decoders()` | 清除所有解码器 |
| `capture_and_decode(device_id, protocol, channel_map, ...)` | 采集+解码一条龙 |
| `export(format, directory, ...)` | 导出原始数据 |
| `export_decoder_table(filepath, analyzer_id)` | 导出解码表 |
| `get_logic_samples(channel, start, count)` | 读逻辑样本 |
| `get_analog_samples(channel, start, count)` | 读模拟样本 |
| `load(filepath)` / `save(filepath)` | 加载/保存 |
| `close()` | 关闭采集 |
| `get_sample_rate()` / `set_sample_rate(rate)` | 采样率 |
| `get_channels()` | 获取通道列表 |
| `enable_channel(index)` / `disable_channel(index)` | 启用/禁用通道 |
| `client` | 访问底层 McpClient（属性） |

---

## PXViewProcess

### 构造

```python
PXViewProcess(
    exe_path=None,          # PXView.exe 路径，None=自动搜索
    port=10110,
    ws_port=10430,
    log_level=-1,
    store_log=False,
    startup_timeout=30.0,
)
```

### 方法

| 方法 | 说明 |
|------|------|
| `start()` | 启动 PXView --headless |
| `stop()` | 停止进程 |
| `is_running` | 进程是否在运行（属性） |
| `port` | MCP 端口（属性） |
