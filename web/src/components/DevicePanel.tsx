import { useAppStore } from '../hooks/useAppStore';
import { useTranslation } from 'react-i18next';

export default function DevicePanel() {
  const deviceInfo = useAppStore((s) => s.deviceInfo);
  const captureStatus = useAppStore((s) => s.captureStatus);
  const mcpConnected = useAppStore((s) => s.mcpConnected);
  const reconnectStatus = useAppStore((s) => s.reconnectStatus);
  const connectMcp = useAppStore((s) => s.connectMcp);
  const disconnectMcp = useAppStore((s) => s.disconnectMcp);
  const attemptReconnect = useAppStore((s) => s.attemptReconnect);
  const { t } = useTranslation();

  const isCapturing = captureStatus === 'capturing';

  return (
    <div className="w-full h-full flex flex-col p-4 gap-6 overflow-y-auto">
      {/* Module Title */}
      <div className="flex items-center gap-2 border-b-4 border-border pb-2">
        <div className="w-4 h-4 bg-border"></div>
        <h2 className="font-bold uppercase tracking-widest text-text-casing text-xl">{t('HW_DIAG_MOD')}</h2>
      </div>

      {/* Connection State LED */}
      <div className="bg-bg-casing p-4 border-4 border-border shadow-[4px_4px_0_0_rgba(0,0,0,0.2)]">
        <div className="text-xs font-bold text-text-casing-muted uppercase mb-3 tracking-widest">{t('MASTER_LINK')}</div>
        <div className="flex items-center gap-4">
          {/* Physical looking LED */}
          <div className="w-8 h-8 rounded-full border-4 border-border shadow-[inset_0_2px_4px_rgba(0,0,0,0.6)] flex items-center justify-center bg-bg-screen-light relative">
             <div className={`w-4 h-4 rounded-full ${mcpConnected ? 'bg-success shadow-[0_0_10px_#2ECC71]' : 'bg-[#111]'}`}></div>
          </div>
          <div className="flex-1">
            <div className={`font-bold text-lg uppercase tracking-widest ${mcpConnected ? 'text-success' : 'text-error'}`}>
              {reconnectStatus === 'reconnecting' ? t('RECONNECTING') : reconnectStatus === 'failed' ? t('LINK_FAILED') : mcpConnected ? t('ONLINE') : t('OFFLINE')}
            </div>
          </div>
        </div>
      </div>

      {/* Hardware Info Box */}
      <div className="bg-bg-casing p-4 border-4 border-border shadow-[4px_4px_0_0_rgba(0,0,0,0.2)] flex flex-col gap-3">
        <div className="text-xs font-bold text-text-casing-muted uppercase tracking-widest border-b-2 border-border pb-2">{t('HARDWARE_ID')}</div>
        {deviceInfo ? (
          <div className="flex flex-col gap-3">
            <div className="flex justify-between items-start gap-4">
              <span className="text-xs font-bold text-text-casing-muted uppercase tracking-widest mt-1 whitespace-nowrap">{t('UNIT')}</span>
              <span className="text-text-casing font-bold text-lg leading-tight text-right break-words">{deviceInfo.name}</span>
            </div>
            <div className="border-t-2 border-border opacity-30 border-dashed"></div>
            <div className="flex justify-between items-start gap-4">
              <span className="text-xs font-bold text-text-casing-muted uppercase tracking-widest mt-1 whitespace-nowrap">{t('BUS')}</span>
              <span className="text-text-casing font-bold text-lg leading-tight text-right">{deviceInfo.usbType}</span>
            </div>
            <div className="border-t-2 border-border opacity-30 border-dashed"></div>
            <div className="flex justify-between items-start gap-4">
              <span className="text-xs font-bold text-text-casing-muted uppercase tracking-widest mt-1 whitespace-nowrap">{t('MODE')}</span>
              <span className="text-text-casing font-bold text-lg leading-tight text-right uppercase">{deviceInfo.mode}</span>
            </div>
          </div>
        ) : (
          <div className="text-text-casing-muted text-right py-4 font-bold animate-pulse uppercase tracking-widest">
             {t('AWAITING_SIGNAL')}
          </div>
        )}
      </div>

      {/* Capture Status */}
      <div className="bg-bg-casing p-4 border-4 border-border shadow-[4px_4px_0_0_rgba(0,0,0,0.2)]">
        <div className="text-xs font-bold text-text-casing-muted uppercase mb-3 tracking-widest">{t('CAPTURE_STATE')}</div>
        <div className="flex items-center gap-4">
          <div className="w-8 h-8 rounded-full border-4 border-border shadow-[inset_0_2px_4px_rgba(0,0,0,0.6)] flex items-center justify-center bg-bg-screen-light relative">
             <div className={`w-4 h-4 rounded-full ${isCapturing ? 'bg-warning shadow-[0_0_10px_#F39C12]' : 'bg-[#111]'}`}></div>
          </div>
          <div className={`font-bold text-lg uppercase tracking-widest ${isCapturing ? 'text-warning animate-pulse' : 'text-text-casing-muted'}`}>
            {captureStatus.toUpperCase()}
          </div>
        </div>
      </div>

      {/* Main Action Buttons */}
      <div className="mt-auto pt-4 flex flex-col gap-3">
        {reconnectStatus === 'failed' ? (
           <button
             onClick={attemptReconnect}
             className="w-full py-3 bg-accent text-bg-casing font-bold uppercase tracking-widest border-4 border-border shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all"
           >
             {t('RESET_LINK')}
           </button>
        ) : mcpConnected ? (
           <button
             onClick={disconnectMcp}
             className="w-full py-3 bg-error text-bg-casing font-bold uppercase tracking-widest border-4 border-border shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all"
           >
             {t('CUT_POWER')}
           </button>
        ) : (
           <button
             onClick={connectMcp}
             className="w-full py-3 bg-success text-bg-casing font-bold uppercase tracking-widest border-4 border-border shadow-[4px_4px_0_0_#000] active:translate-x-[4px] active:translate-y-[4px] active:shadow-[0_0_0_0_#000] transition-all disabled:opacity-50"
             disabled={reconnectStatus === 'reconnecting'}
           >
             {t('ENGAGE')}
           </button>
        )}
      </div>
    </div>
  );
}
