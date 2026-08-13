import { useState, useEffect } from 'react';
import { Settings, MessageSquarePlus, Monitor, Minus, Square, X, Copy as RestoreIcon } from 'lucide-react';
import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';
import { isTauri, windowMinimize, windowToggleMaximize, windowClose, windowIsMaximized, onWindowResized } from '../lib/tauri-bridge';

export default function TopBar({
  onSettingsClick,
  onNewChat,
}: {
  onSettingsClick: () => void;
  onNewChat: () => void;
}) {
  const updateSettings = useAppStore((s) => s.updateSettings);
  const { t, i18n } = useTranslation();

  const [isMaximized, setIsMaximized] = useState(false);

  // Track maximized state via Tauri's onResized event (event-driven, no polling).
  // The event fires on maximize, restore, and manual resize.
  useEffect(() => {
    if (!isTauri()) return;
    let unlisten: (() => void) | null = null;

    // Get initial state immediately
    windowIsMaximized().then(setIsMaximized);

    // Subscribe to resize events — covers maximize/restore without polling
    onWindowResized(() => {
      windowIsMaximized().then(setIsMaximized);
    }).then(fn => { unlisten = fn; });

    return () => { unlisten?.(); };
  }, []);

  const toggleLanguage = () => {
    const nextLang = i18n.language === 'en' ? 'zh' : 'en';
    updateSettings({ language: nextLang });
  };

  return (
    <div
      data-tauri-drag-region
      className="h-16 bg-bg-casing border-b-4 border-border flex items-center justify-between px-6 shrink-0 shadow-[0_4px_0_rgba(0,0,0,0.1)] z-10 relative"
    >
      {/* Left: Logo/Label */}
      <div className="flex items-center gap-4" data-tauri-drag-region>
        <div
          className="bg-border text-bg-casing px-3 py-1 font-bold tracking-widest text-xl uppercase select-none"
          data-tauri-drag-region
        >
          {t('APP_TITLE')}
        </div>
        <div
          className="font-bold truncate text-text-casing hidden sm:block border-l-2 border-border pl-4 select-none"
          data-tauri-drag-region
        >
          {t('APP_SUBTITLE')}
        </div>
        {isTauri() && (
          <div className="hidden md:flex items-center gap-1 px-2 py-0.5 border-2 border-border bg-accent text-bg-casing font-bold text-xs uppercase tracking-widest">
            <Monitor className="w-3.5 h-3.5" />
            Desktop
          </div>
        )}
      </div>

      {/* Right: Actions + Window Controls */}
      <div className="flex items-center gap-3">
        <button
          onClick={toggleLanguage}
          className="px-3 py-1.5 border-2 border-border bg-bg-casing shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all text-text-casing hover:bg-bg-casing-dark font-bold font-mono text-xs"
          title={t('LANG_TOGGLE')}
        >
          {t('LANG_TOGGLE')} ({i18n.language.toUpperCase()})
        </button>
        <button
          onClick={onSettingsClick}
          className="p-2 border-2 border-border bg-bg-casing shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all text-text-casing hover:bg-bg-casing-dark"
          title={t('MAINTENANCE_PNL')}
        >
          <Settings className="w-5 h-5" />
        </button>
        <button
          onClick={onNewChat}
          className="p-2 border-2 border-border bg-bg-casing shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all text-text-casing hover:bg-bg-casing-dark"
          title="Reset Terminal"
        >
          <MessageSquarePlus className="w-5 h-5" />
        </button>

        {/* Window control buttons — cassette futurism style */}
        {isTauri() && (
          <div className="flex items-center gap-1.5 ml-2 pl-3 border-l-2 border-border">
            {/* Minimize */}
            <button
              onClick={windowMinimize}
              className="w-9 h-9 flex items-center justify-center border-2 border-border bg-bg-casing shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all text-text-casing hover:bg-bg-casing-dark"
              title="Minimize"
            >
              <Minus className="w-4 h-4" strokeWidth={3} />
            </button>

            {/* Maximize / Restore */}
            <button
              onClick={windowToggleMaximize}
              className="w-9 h-9 flex items-center justify-center border-2 border-border bg-bg-casing shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all text-text-casing hover:bg-bg-casing-dark"
              title={isMaximized ? "Restore" : "Maximize"}
            >
              {isMaximized ? (
                <RestoreIcon className="w-3.5 h-3.5" strokeWidth={2.5} />
              ) : (
                <Square className="w-3.5 h-3.5" strokeWidth={2.5} />
              )}
            </button>

            {/* Close — uses accent red to signal danger */}
            <button
              onClick={windowClose}
              className="w-9 h-9 flex items-center justify-center border-2 border-border bg-error text-white shadow-[2px_2px_0_0_#000] active:shadow-[0_0_0_0_#000] active:translate-x-[2px] active:translate-y-[2px] transition-all hover:brightness-110"
              title="Close"
            >
              <X className="w-4 h-4" strokeWidth={3} />
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
