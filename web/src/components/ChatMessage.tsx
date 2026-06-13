import { useState } from 'react';
import type { ConversationMessage } from '../hooks/useAppStore';
import ToolCallCard from './ToolCallCard';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

export default function ChatMessage({ message }: { message: ConversationMessage }) {
  const isUser = message.role === 'user';
  const isAssistant = message.role === 'assistant';
  const isStreaming = message.isStreaming;
  const isToolRunning = message.isToolRunning;
  const isStopped = message.isStopped;
  const hasToolCalls = message.toolCallStatuses && message.toolCallStatuses.length > 0;
  const showThinking = isStreaming && !message.content && (!hasToolCalls || message.toolCallStatuses!.every(tc => tc.status === 'pending'));

  const regenerateMessage = useAppStore(s => s.regenerateMessage);
  const isProcessing = useAppStore(s => s.isProcessing);
  const { t } = useTranslation();

  const [copied, setCopied] = useState(false);

  const handleCopy = () => {
    navigator.clipboard.writeText(message.content);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  const handleRegenerate = () => {
    if (!isProcessing) {
      regenerateMessage(message.id);
    }
  };

  const alignClass = isUser ? 'self-end' : 'self-start';
  const cardColor = isUser ? 'bg-white' : 'bg-bg-casing';
  const headerText = isUser ? 'USER INPUT' : message.role === 'tool' ? 'SYS OUTPUT' : 'RESPONSE DECK';

  return (
    <div className={`mb-6 flex flex-col ${alignClass} max-w-[85%] group`}>
      <div className={`relative border-2 border-border ${cardColor} text-text-casing shadow-[4px_4px_0_0_#000] p-4 flex flex-col gap-2`}>
        {/* Card Header */}
        <div className="flex justify-between items-center border-b-2 border-border pb-2 mb-2 font-bold text-xs uppercase tracking-widest opacity-60">
          <span>{headerText}</span>
          
          {/* Action buttons (Copy, Regenerate) shown on hover */}
          <div className="opacity-0 group-hover:opacity-100 transition-opacity flex gap-2">
            {message.content && (
              <button 
                onClick={handleCopy} 
                className="bg-bg-casing-dark border border-border px-2 py-0.5 text-text-casing hover:bg-border hover:text-bg-casing transition-colors"
              >
                {copied ? t('COPIED') : t('COPY')}
              </button>
            )}
            {isAssistant && !isProcessing && (
              <button 
                onClick={handleRegenerate}
                className="bg-bg-casing-dark border border-border px-2 py-0.5 text-warning hover:bg-warning hover:text-bg-casing transition-colors"
                title="Regenerate this response and discard everything after it"
              >
                {t('REGENERATE')}
              </button>
            )}
          </div>
        </div>

        {/* Thinking indicator */}
        {showThinking && (
          <div className="animate-pulse font-bold">{t('PROCESSING')}</div>
        )}

        {/* Text content with Markdown support */}
        {message.content && (
          <div className="markdown-body leading-relaxed max-w-full overflow-hidden">
            <ReactMarkdown remarkPlugins={[remarkGfm]}>
              {message.content}
            </ReactMarkdown>
            {isStreaming && (
              <span className="inline-block w-2 h-4 bg-text-casing animate-pulse ml-1 align-text-bottom" />
            )}
          </div>
        )}

        {/* Stopped indicator */}
        {isStopped && (
          <div className="text-error font-bold uppercase mt-2">{t('HALTED_USER')}</div>
        )}

        {/* Tool calls */}
        {hasToolCalls && (
          <div className="mt-4 space-y-2">
            {message.toolCallStatuses!.map((tc) => (
              <ToolCallCard key={tc.id} toolCall={tc} />
            ))}
          </div>
        )}

        {/* Tool running indicator */}
        {isToolRunning && hasToolCalls && message.toolCallStatuses!.some(tc => tc.status === 'running') && (
          <div className="mt-2 animate-pulse text-warning font-bold">
            {t('EXECUTING_SUBROUTINE')}
          </div>
        )}
      </div>
    </div>
  );
}
