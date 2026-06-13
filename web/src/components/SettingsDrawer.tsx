import { useState, useEffect } from 'react';
import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

interface Settings {
  mcpServerUrl: string;
  llmBaseUrl: string;
  llmApiKey: string;
  llmModel: string;
  systemPrompt: string;
}

export default function SettingsDrawer({ open, onClose }: { open: boolean; onClose: () => void }) {
  const settings = useAppStore((s) => s.settings);
  const updateSettings = useAppStore((s) => s.updateSettings);
  const { t } = useTranslation();

  const [form, setForm] = useState<Settings>({
    mcpServerUrl: '',
    llmBaseUrl: '',
    llmApiKey: '',
    llmModel: '',
    systemPrompt: '',
  });

  useEffect(() => {
    if (open) {
      setForm({
        mcpServerUrl: settings?.mcpServerUrl ?? '',
        llmBaseUrl: settings?.llmBaseUrl ?? '',
        llmApiKey: settings?.llmApiKey ?? '',
        llmModel: settings?.llmModel ?? '',
        systemPrompt: settings?.systemPrompt ?? '',
      });
    }
  }, [open, settings]);

  const handleSave = () => {
    updateSettings(form);
    onClose();
  };

  if (!open) return null;

  const field = (label: string, key: keyof Settings, type: string = 'text') => (
    <div className="flex flex-col gap-1">
      <label className="text-xs font-bold text-text-casing-muted uppercase tracking-widest">{label}</label>
      {type === 'textarea' ? (
        <textarea
          ref={(el) => {
            if (el) {
              el.style.height = 'auto';
              el.style.height = el.scrollHeight + 'px';
            }
          }}
          value={form[key] as string}
          onChange={(e) => {
            setForm({ ...form, [key]: e.target.value });
            e.target.style.height = 'auto';
            e.target.style.height = e.target.scrollHeight + 'px';
          }}
          rows={4}
          className="w-full bg-bg-casing-dark border-2 border-border px-3 py-2 text-sm text-text-casing font-mono placeholder:text-text-casing-muted placeholder:opacity-50 focus:outline-none focus:bg-bg-casing shadow-[inset_2px_2px_4px_rgba(0,0,0,0.1)] transition-colors overflow-hidden resize-none"
        />
      ) : (
        <input
          type={type}
          value={form[key] as string}
          onChange={(e) => setForm({ ...form, [key]: e.target.value })}
          className="w-full bg-bg-casing-dark border-2 border-border px-3 py-2 text-sm text-text-casing font-mono placeholder:text-text-casing-muted placeholder:opacity-50 focus:outline-none focus:bg-bg-casing shadow-[inset_2px_2px_4px_rgba(0,0,0,0.1)] transition-colors"
        />
      )}
    </div>
  );

  return (
    <>
      {/* Backdrop */}
      <div className="fixed inset-0 bg-black/80 z-40" onClick={onClose} />

      {/* Drawer */}
      <div className="fixed top-0 right-0 h-full w-[450px] max-w-full bg-bg-casing border-l-4 border-border z-50 flex flex-col shadow-[-10px_0_30px_rgba(0,0,0,0.5)]">
        {/* Header */}
        <div className="flex items-center justify-between px-6 py-4 border-b-4 border-border bg-bg-casing-dark">
          <div className="flex items-center gap-3">
            <div className="w-3 h-3 bg-warning shadow-[0_0_5px_#F39C12] rounded-full border border-border"></div>
            <h2 className="text-lg font-bold text-text-casing uppercase tracking-widest">{t('MAINTENANCE_PNL')}</h2>
          </div>
          <button onClick={onClose} className="px-3 py-1 font-bold text-bg-casing bg-border hover:bg-opacity-80">
            [X]
          </button>
        </div>

        {/* Fields */}
        <div className="flex-1 overflow-y-auto p-6 space-y-6 bg-bg-casing">
          {field(t('SYS_CONN'), 'mcpServerUrl')}
          {field(t('AI_CORE'), 'llmBaseUrl')}
          {field(t('AI_AUTH'), 'llmApiKey', 'password')}
          {field(t('AI_MDL'), 'llmModel')}
          {field(t('SYS_PROMPT'), 'systemPrompt', 'textarea')}
        </div>

        {/* Save */}
        <div className="p-6 border-t-4 border-border bg-bg-casing-dark flex justify-end gap-4">
          <button
            onClick={onClose}
            className="px-6 py-3 border-2 border-border bg-bg-casing font-bold uppercase tracking-widest shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all"
          >
            {t('CANCEL')}
          </button>
          <button
            onClick={handleSave}
            className="px-6 py-3 border-2 border-border bg-success text-bg-casing font-bold uppercase tracking-widest shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all"
          >
            {t('WRITE_ROM')}
          </button>
        </div>
      </div>
    </>
  );
}
