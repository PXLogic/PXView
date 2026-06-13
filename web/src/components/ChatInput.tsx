import { useState, useRef, useEffect, type KeyboardEvent } from 'react';
import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

export default function ChatInput() {
  const [text, setText] = useState('');
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const sendMessage = useAppStore((s) => s.sendMessage);
  const stopGeneration = useAppStore((s) => s.stopGeneration);
  const isProcessing = useAppStore((s) => s.isProcessing);
  const { t } = useTranslation();

  useEffect(() => {
    const el = textareaRef.current;
    if (!el) return;
    el.style.height = 'auto';
    el.style.height = `${Math.min(el.scrollHeight, 4 * 24)}px`;
  }, [text]);

  const handleSend = () => {
    if (!textareaRef.current) return;
    const trimmed = textareaRef.current.value.trim();
    if (!trimmed || isProcessing) return;
    sendMessage(trimmed);
    textareaRef.current.value = '';
    setText('');
    textareaRef.current.style.height = 'auto';
  };

  const handleKeyDown = (e: KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  return (
    <div className="flex items-end gap-2 p-4 bg-bg-casing border-t-4 border-border shadow-[0_-4px_0_rgba(0,0,0,0.1)] relative z-10">
      <div className="flex-1 bg-white border-2 border-border shadow-[inset_0_2px_4px_rgba(0,0,0,0.2)] flex items-center p-2">
        <textarea
          ref={textareaRef}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={t('INPUT_PLACEHOLDER')}
          rows={1}
          className="flex-1 resize-none bg-transparent outline-none border-none px-2 py-1 text-lg text-text-casing placeholder:text-text-casing-muted placeholder:opacity-50 font-mono"
          spellCheck="false"
        />
      </div>
      {isProcessing ? (
        <button
          onClick={stopGeneration}
          className="px-6 py-3 border-2 border-border bg-error text-bg-casing font-bold uppercase tracking-widest shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all"
        >
          {t('HALT')}
        </button>
      ) : (
        <button
          onClick={handleSend}
          disabled={!text.trim() || isProcessing}
          className="px-6 py-3 border-2 border-border bg-accent text-bg-casing font-bold uppercase tracking-widest shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all disabled:opacity-50 disabled:active:translate-x-0 disabled:active:translate-y-0 disabled:active:shadow-[4px_4px_0_0_#000]"
        >
          {t('EXECUTE')}
        </button>
      )}
    </div>
  );
}
