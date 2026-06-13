import { useState } from 'react';
import { Copy } from 'lucide-react';

interface Row {
  [key: string]: string | number;
}

function parseData(data: string): { rows: Row[]; columns: string[] } | null {
  try {
    const parsed = JSON.parse(data);
    if (Array.isArray(parsed)) {
      if (parsed.length === 0) return { rows: [], columns: [] };
      const columns = Object.keys(parsed[0]);
      return { rows: parsed, columns };
    }
    if (parsed.rows && Array.isArray(parsed.rows)) {
      const columns = parsed.columns ?? (parsed.rows.length > 0 ? Object.keys(parsed.rows[0]) : []);
      return { rows: parsed.rows, columns };
    }
    return null;
  } catch {
    return null;
  }
}

function isPwmResult(columns: string[]): boolean {
  const lower = columns.map((c) => c.toLowerCase());
  return lower.some((c) => c.includes('duty')) && lower.some((c) => c.includes('period'));
}

function pwmColumns(columns: string[]): string[] {
  return columns.map((c) => {
    const l = c.toLowerCase();
    if (l.includes('duty')) return 'Duty Cycle';
    if (l.includes('period')) return 'Period';
    if (l.includes('time')) return 'Time';
    return c;
  });
}

function genericColumns(columns: string[]): string[] {
  return columns.map((c) => {
    const l = c.toLowerCase();
    if (l === 'start_sample' || l === 'startsample') return 'Start Sample';
    if (l === 'end_sample' || l === 'endsample') return 'End Sample';
    if (l === 'type') return 'Type';
    if (l === 'value') return 'Value';
    return c;
  });
}

export default function DecoderResultTable({ data }: { data: string }) {
  const [showAll, setShowAll] = useState(false);
  const result = parseData(data);

  if (!result) {
    return (
      <div className="text-xs text-text-secondary bg-bg-card rounded p-2 whitespace-pre-wrap font-mono">
        {data}
      </div>
    );
  }

  const { rows, columns } = result;
  const pwm = isPwmResult(columns);
  const headers = pwm ? pwmColumns(columns) : genericColumns(columns);
  const maxRows = 100;
  const display = showAll ? rows : rows.slice(0, maxRows);
  const truncated = rows.length > maxRows && !showAll;

  const copyCsv = () => {
    const header = headers.join(',');
    const body = display.map((r) => columns.map((c) => r[c] ?? '').join(',')).join('\n');
    navigator.clipboard.writeText(`${header}\n${body}`);
  };

  return (
    <div className="bg-bg-card rounded-lg border border-border overflow-hidden">
      <div className="flex items-center justify-between px-3 py-1.5 border-b border-border">
        <span className="text-xs text-text-secondary">
          {truncated ? `Showing ${maxRows} of ${rows.length}` : `${rows.length} rows`}
        </span>
        <button onClick={copyCsv} className="text-xs text-accent hover:underline flex items-center gap-1">
          <Copy className="w-3 h-3" /> Copy CSV
        </button>
      </div>
      <div className="overflow-x-auto max-h-64">
        <table className="w-full text-xs">
          <thead>
            <tr className="border-b border-border">
              <th className="px-2 py-1 text-left text-text-secondary font-medium">#</th>
              {headers.map((h) => (
                <th key={h} className="px-2 py-1 text-left text-text-secondary font-medium">{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {display.map((row, i) => (
              <tr key={i} className="border-b border-border/50 hover:bg-bg-primary/50">
                <td className="px-2 py-1 text-text-secondary">{i + 1}</td>
                {columns.map((c) => (
                  <td key={c} className="px-2 py-1 text-text-primary">{row[c] ?? ''}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {truncated && (
        <button onClick={() => setShowAll(true)} className="w-full text-xs text-accent py-1.5 hover:bg-bg-primary/50">
          Show all {rows.length} rows
        </button>
      )}
    </div>
  );
}
