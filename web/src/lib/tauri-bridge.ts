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
