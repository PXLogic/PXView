import { useState, useEffect } from 'react';
import type { ToolCallStatus } from '../hooks/useAppStore';
import DecoderResultTable from './DecoderResultTable';
import { useTranslation } from 'react-i18next';

function getFriendlyLabel(name: string, args: Record<string, unknown>): string | null {
  if (name === 'get_devices') return 'FETCHING_DEVICES';
  if (name === 'start_capture') return 'INITIATING_CAPTURE';
  if (name === 'add_analyzer') {
    const decoder = args.decoder ?? args.protocol ?? '';
    const ch = args.channel ?? args.ch ?? '';
    return `MOUNTING_${decoder}_CH${ch}`;
  }
  return null;
}

const StatusLabel = ({ status, t }: { status: ToolCallStatus['status'], t: any }) => {
  if (status === 'pending') return <span className="text-text-casing-muted animate-pulse">{t('TOOL_PENDING')}</span>;
  if (status === 'running') return <span className="text-warning animate-pulse">{t('TOOL_RUNNING')}</span>;
  if (status === 'success') return <span className="text-text-casing font-bold">{t('TOOL_SUCCESS')}</span>;
  if (status === 'cancelled') return <span className="text-text-casing-muted">{t('TOOL_CANCELLED')}</span>;
  return <span className="text-error">{t('TOOL_ERROR')}</span>;
};

function DeviceCards({ data, t }: { data: string, t: any }) {
  try {
    const devices = JSON.parse(data);
    if (!Array.isArray(devices)) return <pre className="text-sm p-2 overflow-x-auto">{data}</pre>;
    return (
      <div className="space-y-1">
        {devices.map((d: any, i: number) => (
          <div key={i} className="flex gap-4">
            <span className="w-4">{i}</span>
            <span className="font-bold">
              {d.display_name || d.name || d.modelName || t('UNKNOWN_DEVICE')}
              {d.is_active && <span className="ml-2 text-warning animate-pulse">[{t('ACTIVE')}]</span>}
            </span>
            <span>
              {d.usb_speed === 4 ? 'USB 3.0' : d.usb_speed === 3 ? 'USB 2.0' : d.usb_speed === 2 ? 'USB 1.1' : d.is_virtual ? 'Virtual' : '---'}
            </span>
            <span>
              {d.is_hardware_dso ? 'DSO' : d.is_hardware_logic ? 'Logic' : d.is_file ? 'File' : '---'}
            </span>
          </div>
        ))}
      </div>
    );
  } catch {
    return <pre className="text-sm p-2 overflow-x-auto">{data}</pre>;
  }
}

export default function ToolCallCard({ toolCall }: { toolCall: ToolCallStatus }) {
  const [expanded, setExpanded] = useState(false);
  const [showFull, setShowFull] = useState(false);
  const [liveElapsed, setLiveElapsed] = useState<number | null>(null);
  const { t } = useTranslation();

  useEffect(() => {
    if (toolCall.status !== 'running') return;

    const update = () => {
      setLiveElapsed(Math.round((Date.now() - toolCall.startTime) / 1000));
    };
    update(); // initial tick
    const timer = setInterval(update, 1000);

    return () => clearInterval(timer);
  }, [toolCall.status, toolCall.startTime]);

  const args = (() => {
    try {
      return typeof toolCall.args === 'string' ? JSON.parse(toolCall.args) : toolCall.args ?? {};
    } catch {
      return {};
    }
  })();

  const friendly = getFriendlyLabel(toolCall.name, args);
  const resultText = toolCall.result ?? '';
  const truncated = !showFull && resultText.length > 500;
  const displayResult = truncated ? resultText.slice(0, 500) : resultText;

  return (
    <div className="border border-text-casing-muted border-dashed p-2 my-2 bg-bg-casing-dark shadow-[inset_0_2px_4px_rgba(0,0,0,0.1)]">
      <button
        onClick={() => setExpanded(!expanded)}
        className="w-full flex items-center gap-2 text-left hover:bg-bg-casing cursor-pointer"
      >
        <span className="font-bold shrink-0">{expanded ? `[${t('COLLAPSE')}]` : `[${t('EXPAND')}]`}</span>
        <StatusLabel status={toolCall.status} t={t} />
        <span className="font-bold uppercase">{toolCall.name}</span>
        {friendly && <span className="opacity-70 text-sm hidden md:inline"> {friendly}</span>}
        
        {toolCall.status === 'running' && liveElapsed != null && (
          <span className="ml-auto text-sm text-warning animate-pulse">{liveElapsed}s</span>
        )}
        {toolCall.status !== 'running' && toolCall.status !== 'pending' && toolCall.elapsed != null && (
          <span className="ml-auto text-sm opacity-50">{toolCall.elapsed}{t('ELAPSED')}</span>
        )}
      </button>

      {expanded && (
        <div className="mt-2 border-t border-text-casing-muted border-dashed pt-2 space-y-2">
          {Object.keys(args).length > 0 && (
            <pre className="text-sm opacity-80 overflow-x-auto">
              {JSON.stringify(args, null, 2)}
            </pre>
          )}
          {resultText && (
            <div className="mt-2 border-t border-text-casing-muted border-dashed pt-2">
              {toolCall.name === 'get_analyzer_results' ? (
                <DecoderResultTable data={resultText} />
              ) : toolCall.name === 'get_devices' ? (
                <DeviceCards data={resultText} t={t} />
              ) : (
                <>
                  <pre className="text-sm overflow-x-auto whitespace-pre-wrap">
                    {displayResult}
                  </pre>
                  {truncated && (
                    <button onClick={() => setShowFull(true)} className="text-sm underline mt-1 opacity-70 hover:opacity-100">
                      [{t('SHOW_MORE')}]
                    </button>
                  )}
                </>
              )}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
