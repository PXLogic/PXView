use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::path::PathBuf;
use serde::Serialize;
use tauri::{Manager, AppHandle};

/// Global state holding the PXView child process handle.
/// When the Tauri app exits, we kill the child to clean up.
struct PxViewChild(Mutex<Option<Child>>);

/// Result struct returned to the frontend.
#[derive(Serialize)]
struct PxViewStatus {
    running: bool,
    pid: Option<u32>,
    path: String,
    message: String,
}

/// Try to locate PXView.exe in several candidate locations:
///
/// 1. Next to the current executable (production bundled mode)
/// 2. In a `pxview/` subfolder next to the executable (production)
/// 3. In `../../build.dir/` relative to the CWD (development)
/// 4. In `../../../build.dir/` relative to the executable (development from src-tauri)
/// 5. On the system PATH
fn find_pxview_exe(app: &AppHandle) -> Option<PathBuf> {
    // 1 & 2: Next to the current executable
    // The executable has a ".exe" suffix on Windows, but no suffix on
    // Linux/macOS. Use the platform-appropriate name so the Agent can find the
    // headless PXView sitting next to it in the packaged bundle.
    let exe_name = if cfg!(windows) { "PXView.exe" } else { "PXView" };

    if let Ok(exe_dir) = std::env::current_exe() {
        if let Some(exe_dir) = exe_dir.parent() {
            // Direct neighbour
            let direct = exe_dir.join(exe_name);
            if direct.exists() {
                return Some(direct);
            }
            // In a subfolder
            let subfolder = exe_dir.join("pxview").join(exe_name);
            if subfolder.exists() {
                return Some(subfolder);
            }
            // Dev mode: src-tauri/target/debug/ or release/ → build.dir/
            let dev_build = exe_dir
                .join("..")
                .join("..")
                .join("..")
                .join("..")
                .join("build.dir")
                .join(exe_name);
            if dev_build.exists() {
                return Some(dev_build.canonicalize().unwrap_or(dev_build));
            }
        }
    }

    // 3: Relative to CWD (development)
    let cwd_build = PathBuf::from("../../build.dir").join(exe_name);
    if cwd_build.exists() {
        return Some(cwd_build.canonicalize().unwrap_or(cwd_build));
    }

    // Also try from the tauri.conf.json directory
    if let Some(resource_path) = app.path().resource_dir().ok() {
        let res = resource_path.join(exe_name);
        if res.exists() {
            return Some(res);
        }
    }

    // 5: Try `where PXView` on Windows (PATH lookup)
    #[cfg(windows)]
    {
        if let Ok(output) = Command::new("where").arg("PXView.exe").output() {
            if output.status.success() {
                let text = String::from_utf8_lossy(&output.stdout);
                if let Some(first_line) = text.lines().next() {
                    let p = PathBuf::from(first_line.trim());
                    if p.exists() {
                        return Some(p);
                    }
                }
            }
        }
    }

    None
}

/// Spawn PXView.exe in headless mode.
///
/// Arguments:
///   --headless        No GUI, only MCP + WS API
///   --port 10110      MCP server port
///   --ws-port 10430   WebSocket server port
fn spawn_pxview_headless(exe_path: &PathBuf) -> Result<Child, String> {
    Command::new(exe_path)
        .args(["--headless", "--port", "10110", "--ws-port", "10430"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("Failed to spawn PXView: {}", e))
}

/// Tauri command: check whether the PXView headless process is alive.
#[tauri::command]
fn pxview_status(state: tauri::State<'_, PxViewChild>) -> PxViewStatus {
    let mut guard = state.0.lock().unwrap();
    if let Some(ref mut child) = *guard {
        // Try to check if the child is still alive
        match child.try_wait() {
            Ok(None) => {
                // Still running
                PxViewStatus {
                    running: true,
                    pid: Some(child.id()),
                    path: String::new(),
                    message: "PXView headless is running".to_string(),
                }
            }
            Ok(Some(_)) => {
                // Exited
                PxViewStatus {
                    running: false,
                    pid: None,
                    path: String::new(),
                    message: "PXView headless has exited".to_string(),
                }
            }
            Err(_) => PxViewStatus {
                running: false,
                pid: None,
                path: String::new(),
                message: "Failed to query PXView status".to_string(),
            },
        }
    } else {
        PxViewStatus {
            running: false,
            pid: None,
            path: String::new(),
            message: "PXView headless was not started".to_string(),
        }
    }
}

/// Tauri command: manually restart PXView headless process.
#[tauri::command]
fn pxview_restart(app: AppHandle, state: tauri::State<'_, PxViewChild>) -> PxViewStatus {
    // Kill existing process
    {
        let mut guard = state.0.lock().unwrap();
        if let Some(ref mut child) = *guard {
            let _ = child.kill();
            let _ = child.wait();
        }
        *guard = None;
    }

    // Wait a brief moment for the port to be released
    std::thread::sleep(std::time::Duration::from_millis(500));

    // Restart
    match find_pxview_exe(&app) {
        Some(path) => {
            match spawn_pxview_headless(&path) {
                Ok(child) => {
                    let pid = child.id();
                    *state.0.lock().unwrap() = Some(child);
                    PxViewStatus {
                        running: true,
                        pid: Some(pid),
                        path: path.display().to_string(),
                        message: "PXView headless restarted".to_string(),
                    }
                }
                Err(e) => PxViewStatus {
                    running: false,
                    pid: None,
                    path: path.display().to_string(),
                    message: e,
                },
            }
        }
        None => PxViewStatus {
            running: false,
            pid: None,
            path: String::new(),
            message: "PXView.exe not found".to_string(),
        },
    }
}

/// WebKitGTK 2.4x+ composites through dmabuf + GPU. On stacks without a usable
/// dmabuf/GL path the WebProcess aborts during startup and the window stays
/// blank. Setting these forces the software path.
const SOFTWARE_COMPOSITING_VARS: [&str; 2] = [
    "WEBKIT_DISABLE_DMABUF_RENDERER",
    "WEBKIT_DISABLE_COMPOSITING_MODE",
];

/// Escape hatch: `PXVIEW_WEBKIT_SOFTWARE=0` keeps hardware compositing even on
/// a detected VM, `PXVIEW_WEBKIT_SOFTWARE=1` forces software everywhere.
const FORCE_SOFTWARE_VAR: &str = "PXVIEW_WEBKIT_SOFTWARE";

/// Substrings that identify a hypervisor in the DMI vendor/product strings.
const VM_VENDOR_HINTS: [&str; 10] = [
    "vmware",
    "virtualbox",
    "innotek", // VirtualBox's DMI vendor
    "qemu",
    "kvm",
    "xen",
    "bochs",
    "parallels",
    "microsoft corporation", // Hyper-V
    "amazon ec2",
];

fn read_lowercase(path: &str) -> Option<String> {
    std::fs::read_to_string(path)
        .ok()
        .map(|text| text.to_lowercase())
}

/// True when the DMI tables or the CPU flags say we run on a hypervisor.
fn is_virtual_machine() -> bool {
    const DMI_FILES: [&str; 3] = [
        "/sys/class/dmi/id/sys_vendor",
        "/sys/class/dmi/id/product_name",
        "/sys/class/dmi/id/board_vendor",
    ];

    for path in DMI_FILES {
        if let Some(text) = read_lowercase(path) {
            if VM_VENDOR_HINTS.iter().any(|hint| text.contains(hint)) {
                return true;
            }
        }
    }

    // Linux sets the `hypervisor` CPU flag under KVM / Xen / VMware / Hyper-V.
    if let Ok(cpuinfo) = std::fs::read_to_string("/proc/cpuinfo") {
        if cpuinfo
            .lines()
            .any(|line| line.split_whitespace().any(|token| token == "hypervisor"))
        {
            return true;
        }
    }

    false
}

/// True when a DRM render node or an NVIDIA device node is present.
fn has_gpu_device() -> bool {
    if let Ok(entries) = std::fs::read_dir("/dev/dri") {
        if entries
            .flatten()
            .any(|entry| entry.file_name().to_string_lossy().starts_with("card"))
        {
            return true;
        }
    }
    // Proprietary NVIDIA drivers expose /dev/nvidia* instead of /dev/dri when
    // modeset is disabled, so check those before assuming there is no GPU.
    std::path::Path::new("/dev/nvidiactl").exists()
        || std::path::Path::new("/dev/nvidia0").exists()
}

/// Hardware compositing is kept wherever it can plausibly work; only stacks
/// that are known to break get the software fallback.
fn needs_software_compositing() -> bool {
    if is_virtual_machine() {
        log::info!("virtual machine detected; WebKit GPU compositing disabled");
        return true;
    }
    if !has_gpu_device() {
        log::info!("no GPU device node found; WebKit GPU compositing disabled");
        return true;
    }
    false
}

fn apply_software_compositing() {
    for key in SOFTWARE_COMPOSITING_VARS {
        if std::env::var_os(key).is_none() {
            std::env::set_var(key, "1");
            log::info!("{key}=1 (WebKitGTK software-compositing fallback)");
        }
    }
}

/// Choose the WebKitGTK compositing mode before GTK/WebKit initialises.
///
/// Without the fallback the failure looks like this on affected machines:
///
///   (WebKitWebProcess:PID): ERROR **: WebProcess didn't exit as expected
///                                   after the UI process connection was closed
///   "Sorry, the program \"WebKitWebProcess\" closed unexpectedly"
///
/// What makes it nasty is that everything else stays healthy: the UI process
/// keeps running, the window has the right title and size, and the headless
/// PXView child keeps serving MCP port 10110. Only a pixel check sees it.
fn preset_webkit_env() {
    // An explicit choice always wins.
    match std::env::var(FORCE_SOFTWARE_VAR).ok().as_deref() {
        Some("0") => {
            log::info!("{FORCE_SOFTWARE_VAR}=0: keeping hardware compositing");
            return;
        }
        Some(value) => {
            log::info!("{FORCE_SOFTWARE_VAR}={value}: forcing software compositing");
            apply_software_compositing();
            return;
        }
        None => {}
    }

    // Respect values already set by the user or a launcher script.
    if SOFTWARE_COMPOSITING_VARS
        .iter()
        .any(|key| std::env::var_os(key).is_some())
    {
        return;
    }

    if needs_software_compositing() {
        apply_software_compositing();
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp(None)
        .init();

    preset_webkit_env();

    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            // Try to find and spawn PXView.exe in headless mode
            match find_pxview_exe(&app.handle()) {
                Some(path) => {
                    log::info!("Found PXView.exe at: {}", path.display());
                    match spawn_pxview_headless(&path) {
                        Ok(child) => {
                            let pid = child.id();
                            log::info!("PXView headless started, PID: {}", pid);
                            app.manage(PxViewChild(Mutex::new(Some(child))));
                        }
                        Err(e) => {
                            log::error!("{}", e);
                            app.manage(PxViewChild(Mutex::new(None)));
                        }
                    }
                }
                None => {
                    log::warn!("PXView.exe not found. The agent UI will start but MCP connection will fail until PXView is running.");
                    log::warn!("Expected locations:");
                    log::warn!("  1. Next to this executable");
                    log::warn!("  2. ../../build.dir/PXView.exe (development)");
                    log::warn!("  3. On system PATH");
                    app.manage(PxViewChild(Mutex::new(None)));
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![pxview_status, pxview_restart])
        .on_window_event(|window, event| {
            // When the main window is closed, kill the PXView child process
            if let tauri::WindowEvent::Destroyed = event {
                if let Some(state) = window.app_handle().try_state::<PxViewChild>() {
                    let mut guard = state.0.lock().unwrap();
                    if let Some(ref mut child) = *guard {
                        log::info!("Killing PXView headless process...");
                        let _ = child.kill();
                        let _ = child.wait();
                    }
                    *guard = None;
                }
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
