import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

export default function HistoryPanel() {
  const sessions = useAppStore((s) => s.sessions);
  const currentSessionId = useAppStore((s) => s.currentSessionId);
  const createNewSession = useAppStore((s) => s.createNewSession);
  const switchSession = useAppStore((s) => s.switchSession);
  const deleteSession = useAppStore((s) => s.deleteSession);
  const isProcessing = useAppStore((s) => s.isProcessing);
  const { t } = useTranslation();

  // Sort sessions by updatedAt descending
  const sortedSessions = Object.values(sessions).sort((a, b) => b.updatedAt - a.updatedAt);

  return (
    <div className="w-full h-full flex flex-col p-4 gap-4 overflow-y-auto">
      {/* Module Title */}
      <div className="flex items-center gap-2 border-b-4 border-border pb-2">
        <div className="w-4 h-4 bg-border"></div>
        <h2 className="font-bold uppercase tracking-widest text-text-casing text-xl">{t('TAPE_ARCHIVE')}</h2>
      </div>

      <button
        onClick={createNewSession}
        disabled={isProcessing}
        className="w-full py-3 border-2 border-border bg-text-screen text-bg-screen font-bold uppercase tracking-widest shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] disabled:opacity-50 transition-all"
      >
        {t('INSERT_TAPE')}
      </button>

      <div className="flex-1 flex flex-col gap-2 mt-2">
        {sortedSessions.map((session) => {
          const isActive = session.id === currentSessionId;
          const dateStr = new Date(session.updatedAt).toLocaleDateString(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
          
          return (
            <div
              key={session.id}
              className={`flex flex-col border-2 border-border p-2 cursor-pointer transition-colors ${
                isActive ? 'bg-bg-screen-light border-text-screen' : 'bg-bg-casing hover:bg-bg-casing-dark'
              }`}
              onClick={() => !isProcessing && switchSession(session.id)}
            >
              <div className="flex justify-between items-start gap-2">
                <div className={`font-bold truncate ${isActive ? 'text-text-screen' : 'text-text-casing'}`}>
                  {session.title || t('UNTITLED')}
                </div>
                <button
                  onClick={(e) => {
                    e.stopPropagation();
                    if (!isProcessing) deleteSession(session.id);
                  }}
                  disabled={isProcessing}
                  className="text-error font-bold opacity-60 hover:opacity-100 text-sm px-1"
                  title={t('ERASE_TAPE')}
                >
                  [X]
                </button>
              </div>
              <div className={`text-xs font-mono opacity-60 mt-1 ${isActive ? 'text-text-screen' : 'text-text-casing'}`}>
                {dateStr} | {t('MSGS')}: {session.messages.length}
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
