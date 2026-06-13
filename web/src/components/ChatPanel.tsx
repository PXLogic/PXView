import { useEffect, useRef } from 'react';
import { useAppStore } from '../hooks/useAppStore';
import ChatMessage from './ChatMessage';
import ChatInput from './ChatInput';
import ErrorBoundary from './ErrorBoundary';
import { useTranslation } from 'react-i18next';

export default function ChatPanel() {
  const messages = useAppStore((s) => s.messages);
  const clearChat = useAppStore((s) => s.clearChat);
  const isProcessing = useAppStore((s) => s.isProcessing);
  const bottomRef = useRef<HTMLDivElement>(null);
  const { t } = useTranslation();

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages]);

  const handleExport = () => {
    if (messages.length === 0) return;
    const content = messages.map(m => {
      const prefix = m.role === 'user' ? 'USER>' : m.role === 'tool' ? 'OUT >' : 'SYS >';
      return `${prefix}\n${m.content}\n`;
    }).join('\n');
    
    const blob = new Blob([content], { type: 'text/markdown' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `pxview_diag_${new Date().getTime()}.md`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  };

  return (
    <div className="flex flex-col h-full w-full relative">
      {/* Messages area */}
      <div className="flex-1 overflow-y-auto px-4 py-4 font-mono text-lg flex flex-col gap-2 relative">
        <div className="text-text-casing font-bold mb-4 tracking-widest border-b-2 border-border pb-2 flex justify-between items-end">
          <div>
            {t('APP_TITLE')} v1.0 [{t('TERMINAL_MODE')}]<br />
            {t('READY_MSG')}
          </div>
          <div className="flex gap-2 mb-1 opacity-0 hover:opacity-100 transition-opacity">
            <button 
              onClick={handleExport}
              disabled={messages.length === 0}
              className="text-xs bg-bg-casing border-2 border-border px-2 py-1 text-text-casing hover:bg-border hover:text-bg-casing transition-colors disabled:opacity-50"
            >
              {t('EXPORT_LOG')}
            </button>
            <button 
              onClick={clearChat}
              disabled={isProcessing}
              className="text-xs bg-bg-casing border-2 border-border px-2 py-1 text-error hover:bg-error hover:text-bg-casing transition-colors disabled:opacity-50"
            >
              {t('CLEAR_LOG')}
            </button>
          </div>
        </div>
        
        {/* Helper overlay for actions */}
        <div className="absolute top-4 right-4 text-xs opacity-30 pointer-events-none">
          {t('HOVER_ACTIONS')}
        </div>
        
        {messages.length === 0 ? (
          <div className="text-text-casing opacity-50 font-bold animate-pulse text-center mt-10">
            {t('AWAITING_INPUT')}
          </div>
        ) : (
          messages.map((msg) => (
            <ErrorBoundary key={msg.id}>
              <ChatMessage message={msg} />
            </ErrorBoundary>
          ))
        )}
        <div ref={bottomRef} />
      </div>

      {/* Input area */}
      <ChatInput />
    </div>
  );
}
