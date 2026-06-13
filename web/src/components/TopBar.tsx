import { Settings, MessageSquarePlus } from 'lucide-react';
import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

export default function TopBar({
  onSettingsClick,
  onNewChat,
}: {
  onSettingsClick: () => void;
  onNewChat: () => void;
}) {
  const mcpConnected = useAppStore((s) => s.mcpConnected);
  const settings = useAppStore((s) => s.settings);
  const updateSettings = useAppStore((s) => s.updateSettings);
  const { t, i18n } = useTranslation();

  const toggleLanguage = () => {
    const nextLang = i18n.language === 'en' ? 'zh' : 'en';
    updateSettings({ language: nextLang });
  };

  return (
    <div className="h-16 bg-bg-casing border-b-4 border-border flex items-center justify-between px-6 shrink-0 shadow-[0_4px_0_rgba(0,0,0,0.1)] z-10 relative">
      {/* Left: Logo/Label */}
      <div className="flex items-center gap-4">
        <div className="bg-border text-bg-casing px-3 py-1 font-bold tracking-widest text-xl uppercase">
          {t('APP_TITLE')}
        </div>
        <div className="font-bold truncate text-text-casing hidden sm:block border-l-2 border-border pl-4">
          {t('APP_SUBTITLE')}
        </div>
      </div>



      {/* Right: Actions */}
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
      </div>
    </div>
  );
}
