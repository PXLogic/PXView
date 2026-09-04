# PXView 安装器磁盘契约（INSTALL_MANIFEST_SPEC）

跨平台安装/卸载/升级的**策略与格式**在此统一定义；各平台实现自己的引擎
（Windows: NSIS；Linux: 自解压 `.run`；macOS: DMG）。统一的是载荷与数据，
不是安装器。

## 1. 磁盘布局

| 路径 | 用途 |
|---|---|
| `<PREFIX>/` | 产品树，安装器全权拥有 |
| `<PREFIX>/.pxview/install.json` | 所有权标记 + 版本戳 |
| `<PREFIX>/.pxview/manifest.txt` | 安装文件清单 |
| `<PREFIX>.old.<ts>` | 升级时旧树暂存目录（`<ts>` = `YYYYMMDDHHMMSS`，同目录已存在则追加 `.$$`） |

## 2. install.json

```json
{
  "name": "PXView",
  "version": "1.6.0",
  "installed": "2026-09-04T20:00:00+0800",
  "installer": "self-extracting",
  "prefix": "/opt/PXView",
  "manifest": ".pxview/manifest.txt"
}
```

语义：

- **所有权标记**：目录存在但无此文件（且无 `bin/PXView`）→ 视为非 PXView
  管理目录，升级/卸载默认拒绝，需显式接管开关（Linux: `PXVIEW_FORCE=yes`）。
- **版本戳**：升级时读出旧版本号，打印 "从 X 升级到 Y"。

## 3. manifest.txt

三节，`#` 开头为注释：

```
# PXView install manifest -- consumed by uninstall.sh and the next upgrade
# version=1.6.0
[tree]
bin/PXView
lib/...
[external]
/usr/local/bin/pxview
/usr/lib/udev/rules.d/60-px.rules
/usr/local/share/applications/pxview.desktop
[shortcuts]
/home/alice/Desktop/pxview.desktop
```

- `[tree]`：产品树内文件，**相对路径**（整树随 `rm -rf <PREFIX>` 删除；
  覆盖模式下用于清理"本版本已移除组件"的孤儿文件——清单里有、新载荷里没有的才删）。
- `[external]` / `[shortcuts]`：安装到前缀之外的文件，**绝对路径**
  （udev 规则、CLI 封装、.desktop、图标、pixmaps、各用户桌面快捷方式）。
  卸载与升级时逐条精确删除——**不得再依赖写死文件名**。

## 4. 升级流程（Linux .run 实现）

```
1. 读旧 install.json → 版本号；检测运行中的实例（默认拒绝，见 §6）
2. mv <PREFIX> <PREFIX>.old.<ts>        ← 同一文件系统内原子改名
   （跨设备挂载点退回"先卸后装"，无回滚保护，需警告）
3. 按旧清单 [external]/[shortcuts] 精确清理旧系统集成文件
   （无清单的历史版本 → 通配兜底：pxview* / libsigrok* / 60-px）
4. 安装新树 + 重建集成文件
5. 自检：bin/PXView 可执行、ldd 无 "not found"
6. 提交：删除 .old.<ts>（PXVIEW_KEEP_OLD=yes 则保留）；清 ERR 陷阱
   任一步失败（步骤 2-5）→ rm 掉半成品新树，mv 回旧树，exit 非零
```

## 5. 卸载语义

- 读 manifest，删除全部 `[external]`/`[shortcuts]` 绝对路径 → `rm -rf <PREFIX>`
  → 清理 `.old.*` 残留 → 刷新 udev/desktop-db/icon-cache。
- manifest 缺失（历史版本）→ 纯通配兜底。
- **不删除用户配置**（Linux: `~/.config/PXlogicV20`；Windows: `%APPDATA%`）。

## 6. 运行实例

替换正在运行的程序文件会中断采集并丢失数据。Linux 默认**拒绝升级**，
`PXVIEW_KILL_RUNNING=yes` 才自动结束进程。Windows NSIS 沿用现状
（taskkill 后安装）。

## 7. 平台采用状态

| 平台 | 引擎 | install.json | manifest.txt | 升级保护 |
|---|---|---|---|---|
| Linux | 自解压 `.run`（packaging/install.sh） | 写 + 读 | 写 + 读 | 暂存切换 + ERR 回滚 |
| Windows | NSIS（window_nisi.nsi） | 暂不写（注册表 Uninstall 键即事实标记） | 不消费（NSIS 卸载按 RMDir /r 语义） | 注册表检测旧版 → 同步跑旧卸载器 |
| macOS | DMG 拖拽 | 不适用 | 不适用 | 无安装器 |

Windows 侧只保留目录选择保护（拒绝盘根/系统目录），不为本契约添加读取逻辑。

## 8. 后续规划

Linux 端的最终形态是 **deb/rpm（CPack 从现有 install() 规则生成）**：
dpkg/rpm 天然提供文件清单、版本、升级替换与事务回滚，udev/用户组/图标缓存
放 postinst/postrm，WebKit 依赖写 `Depends`，即可免费获得本文件的全部能力。
`.run` 自解压安装器保留为多发行版通用兜底（NI/NVIDIA 同构），暂存切换逻辑
在其被取代前维持有效。
