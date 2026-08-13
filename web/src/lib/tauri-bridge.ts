/**
 * Tauri integration layer.
 *
 * When running inside Tauri (desktop mode), this module uses the Tauri `invoke`
 * API to communicate with the Rust backend — which manages the PXView headless
 * child process.
 *
 * When running in a plain browser (e.g. `npm run dev` without Tauri), all
 * functions gracefully degrade to no-ops or stubs, so the web app remains
 * fully functional as long as PXView is started manually.
 */

// ── Window controls (custom title bar) ─────────────────────────────────────

/** Minimize the application window. No-op outside Tauri. */
export async function windowMinimize(): Promise<void> {
  if (!isTauri()) return;
  try {
    const { getCurrentWindow } = await import('@tauri-apps/api/window');
    await getCurrentWindow().minimize();
  } catch { /* ignore */ }
}

/** Toggle maximize/restore the application window. No-op outside Tauri. */
export async function windowToggleMaximize(): Promise<void> {
  if (!isTauri()) return;
  try {
    const { getCurrentWindow } = await import('@tauri-apps/api/window');
    await getCurrentWindow().toggleMaximize();
  } catch { /* ignore */ }
}

/** Check if the window is currently maximized. Returns false outside Tauri. */
export async function windowIsMaximized(): Promise<boolean> {
  if (!isTauri()) return false;
  try {
    const { getCurrentWindow } = await import('@tauri-apps/api/window');
    return await getCurrentWindow().isMaximized();
  } catch { return false; }
}

/** Close the application window. No-op outside Tauri. */
export async function windowClose(): Promise<void> {
  if (!isTauri()) return;
  try {
    const { getCurrentWindow } = await import('@tauri-apps/api/window');
    await getCurrentWindow().close();
  } catch { /* ignore */ }
}

/**
 * Subscribe to window resize events (fires on maximize/restore/resize).
 * Returns an unlisten function, or null outside Tauri.
 * This replaces the previous 500ms polling approach.
 */
export async function onWindowResized(callback: () => void): Promise<(() => void) | null> {
  if (!isTauri()) return null;
  try {
    const { getCurrentWindow } = await import('@tauri-apps/api/window');
    return await getCurrentWindow().onResized(callback);
  } catch { return null; }
}

/** Status of the PXView headless backend process. */
export interface PxViewStatus {
  running: boolean;
  pid: number | null;
  path: string;
  message: string;
}

/** Detect whether we are running inside a Tauri webview. */
export function isTauri(): boolean {
  // In Tauri v2, the global `__TAURI_INTERNALS__` is injected.
  return typeof (window as any).__TAURI_INTERNALS__ !== 'undefined';
}

/**
 * Query the PXView headless process status.
 * Returns null when not running inside Tauri.
 */
export async function pxviewStatus(): Promise<PxViewStatus | null> {
  if (!isTauri()) return null;
  try {
    const { invoke } = await import('@tauri-apps/api/core');
    return await invoke<PxViewStatus>('pxview_status');
  } catch {
    return null;
  }
}

/**
 * Restart the PXView headless process.
 * Returns null when not running inside Tauri.
 */
export async function pxviewRestart(): Promise<PxViewStatus | null> {
  if (!isTauri()) return null;
  try {
    const { invoke } = await import('@tauri-apps/api/core');
    return await invoke<PxViewStatus>('pxview_restart');
  } catch {
    return null;
  }
}
