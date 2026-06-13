import { useState, useEffect } from 'react';
import { useAppStore } from './hooks/useAppStore';
import TopBar from './components/TopBar';
import ChatPanel from './components/ChatPanel';
import DevicePanel from './components/DevicePanel';
import SettingsDrawer from './components/SettingsDrawer';
import ErrorBoundary from './components/ErrorBoundary';
import HistoryPanel from './components/HistoryPanel';
import { useTranslation } from 'react-i18next';

export default function App() {
  const settings = useAppStore((s) => s.settings);
  const connectMcp = useAppStore((s) => s.connectMcp);
  const clearChat = useAppStore((s) => s.clearChat);
  const mcpConnected = useAppStore((s) => s.mcpConnected);
  const { t } = useTranslation();

  const [settingsOpen, setSettingsOpen] = useState(false);
  const [mobileDeviceOpen, setMobileDeviceOpen] = useState(false);
  const [mobileHistoryOpen, setMobileHistoryOpen] = useState(false);

  // Auto-connect on first load if settings exist (silently catch errors)
  useEffect(() => {
    if (settings?.mcpServerUrl && !mcpConnected) {
      connectMcp().catch(() => {});
    }
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <ErrorBoundary fallback={
      <div className="h-screen flex items-center justify-center bg-bg-screen text-text-screen">
        <div className="text-center space-y-4 border-4 border-border p-8 bg-bg-screen-light">
          <p className="text-xl font-bold tracking-widest uppercase">{t('SYSTEM_ERROR')}</p>
          <button onClick={() => window.location.reload()} className="px-6 py-2 border-2 border-border bg-text-screen text-bg-screen font-bold hover:opacity-80 active:translate-y-1">
            {t('REBOOT')}
          </button>
        </div>
      </div>
    }>
    <div className="h-screen flex flex-col bg-bg-casing text-text-casing overflow-hidden selection:bg-accent selection:text-white">
      <TopBar
        onSettingsClick={() => setSettingsOpen(true)}
        onNewChat={clearChat}
      />

      {/* Main area - structured like a physical control desk */}
      <div className="flex-1 flex overflow-hidden p-4 md:p-6 gap-6 relative">
        {/* History panel — the tape archive */}
        <div className="hidden lg:flex lg:w-[250px] shrink-0 border-4 border-border bg-bg-casing-dark shadow-[4px_4px_0_0_rgba(0,0,0,0.2)] flex-col rounded-sm relative">
          <div className="absolute top-0 left-0 w-full h-2 bg-border opacity-10"></div>
          <HistoryPanel />
        </div>

        {/* Chat panel — Card interface */}
        <div className="flex-1 border-4 border-border bg-bg-casing-dark flex flex-col shadow-[inset_0_4px_10px_rgba(0,0,0,0.2)] relative rounded-sm overflow-hidden">
          <div className="bg-border text-bg-casing px-3 py-1 text-xs font-bold tracking-widest uppercase flex justify-between">
            <span>{t('TERMINAL_MODE')}</span>
            <span>{new Date().toLocaleTimeString()}</span>
          </div>
          <ChatPanel />
        </div>

        {/* Device panel — desktop hardware control deck */}
        <div className="hidden md:flex md:w-[350px] shrink-0 border-4 border-border bg-bg-casing-dark shadow-[4px_4px_0_0_rgba(0,0,0,0.2)] flex-col rounded-sm relative">
          <div className="absolute top-0 left-0 w-full h-2 bg-border opacity-10"></div>
          <DevicePanel />
        </div>
      </div>

      {/* Mobile device panel — bottom drawer */}
      {mobileDeviceOpen && (
        <>
          <div className="md:hidden fixed inset-0 bg-black/80 z-30" onClick={() => setMobileDeviceOpen(false)} />
          <div className="md:hidden fixed bottom-0 left-0 right-0 z-40 max-h-[70vh] overflow-y-auto bg-bg-casing-dark border-t-4 border-border shadow-[0_-10px_30px_rgba(0,0,0,0.5)]">
            <div className="flex justify-between items-center bg-border text-bg-casing px-4 py-2 font-bold uppercase tracking-widest">
              <span>{t('HW_CONTROL_DECK')}</span>
              <button onClick={() => setMobileDeviceOpen(false)} className="px-2 py-0.5 bg-bg-casing text-border hover:bg-bg-casing-dark">X</button>
            </div>
            <DevicePanel />
          </div>
        </>
      )}

      {/* Mobile device toggle */}
      {!mobileDeviceOpen && (
        <button
          onClick={() => setMobileDeviceOpen(true)}
          className="md:hidden fixed bottom-6 right-6 z-20 w-14 h-14 rounded-full border-4 border-border bg-accent text-bg-casing shadow-[4px_4px_0_0_#000] active:translate-x-1 active:translate-y-1 active:shadow-[0_0_0_0_#000] flex items-center justify-center font-bold text-xl transition-all"
        >
          HW
        </button>
      )}

      {/* Settings drawer */}
      <SettingsDrawer open={settingsOpen} onClose={() => setSettingsOpen(false)} />
    </div>
    </ErrorBoundary>
  );
}
