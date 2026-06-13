export interface McpTool {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
}

export interface McpToolResult {
  content: Array<{ type: string; text: string }>;
  isError?: boolean;
}

export interface ProgressEvent {
  progress?: number;
  message?: string;
  done?: boolean;
  result?: McpToolResult;
}

export type OnProgressCallback = (event: ProgressEvent) => void;

interface JsonRpcResponse {
  jsonrpc: '2.0';
  id: number | string;
  result?: unknown;
  error?: { code: number; message: string; data?: unknown };
}

export class McpClient {
  private url: string;
  private requestId = 0;
  private tools: McpTool[] = [];
  private _connected = false;

  constructor(url: string) {
    this.url = url;
  }

  get connected() { return this._connected; }
  get availableTools() { return this.tools; }

  async connect(): Promise<void> {
    // Send initialize request
    await this.sendRequest('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'PXView MCP Web', version: '1.0.0' },
    });

    // Send notifications/initialized (fire-and-forget, no response expected)
    await this.sendNotification('notifications/initialized');

    // Get tools list
    const toolsResult = await this.sendRequest('tools/list', {});
    if (toolsResult && typeof toolsResult === 'object' && 'tools' in (toolsResult as any)) {
      this.tools = (toolsResult as any).tools as McpTool[];
    }

    this._connected = true;
  }

  async listTools(): Promise<McpTool[]> {
    const result = await this.sendRequest('tools/list', {});
    if (result && typeof result === 'object' && 'tools' in (result as any)) {
      this.tools = (result as any).tools as McpTool[];
    }
    return this.tools;
  }

  async callTool(name: string, args: Record<string, unknown>, onProgress?: OnProgressCallback, signal?: AbortSignal): Promise<McpToolResult> {
    const id = ++this.requestId;
    const body = JSON.stringify({
      jsonrpc: '2.0',
      id,
      method: 'tools/call',
      params: { name, arguments: args },
    });

    const response = await fetch(this.url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body,
      signal,
    });

    if (!response.ok) {
      throw new Error(`MCP server returned HTTP ${response.status}`);
    }

    const contentType = response.headers.get('Content-Type') || '';

    // Handle SSE (text/event-stream) response
    if (contentType.includes('text/event-stream')) {
      return this.handleSSEResponse(response, onProgress, signal);
    }

    // Standard JSON-RPC response
    const json: JsonRpcResponse = await response.json();
    if (json.error) {
      throw new Error(`MCP error [${json.error.code}]: ${json.error.message}`);
    }
    return json.result as McpToolResult;
  }

  private async handleSSEResponse(response: Response, onProgress?: OnProgressCallback, signal?: AbortSignal): Promise<McpToolResult> {
    const reader = response.body?.getReader();
    if (!reader) {
      throw new Error('SSE response body is not readable');
    }

    const decoder = new TextDecoder();
    let buffer = '';
    let finalResult: McpToolResult | null = null;

    try {
      while (true) {
        if (signal?.aborted) {
          reader.cancel();
          break;
        }
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });

        // Process complete SSE events (separated by double newlines)
        const parts = buffer.split('\n\n');
        // Keep the last incomplete part in the buffer
        buffer = parts.pop() || '';

        for (const part of parts) {
          const dataLine = this.extractSSEData(part);
          if (!dataLine) continue;

          try {
            const event = JSON.parse(dataLine);

            // Handle standard JSON-RPC response format sent over SSE
            if (event.jsonrpc === '2.0') {
              if (event.error) {
                throw new Error(`MCP error: ${event.error.message}`);
              }
              finalResult = event.result;
            } 
            // Handle legacy ProgressEvent format
            else if (event.done && event.result) {
              finalResult = event.result;
            }

            if (onProgress && !event.jsonrpc) {
              onProgress(event as ProgressEvent);
            }
          } catch (e) {
            // Ignore malformed JSON in SSE events
          }
        }
      }

      // Process any remaining data in buffer
      if (buffer.trim()) {
        const dataLine = this.extractSSEData(buffer);
        if (dataLine) {
          try {
            const event = JSON.parse(dataLine);

            if (event.jsonrpc === '2.0') {
              if (event.error) {
                throw new Error(`MCP error: ${event.error.message}`);
              }
              finalResult = event.result;
            } else if (event.done && event.result) {
              finalResult = event.result;
            }

            if (onProgress && !event.jsonrpc) {
              onProgress(event as ProgressEvent);
            }
          } catch (e) {
            // Ignore malformed JSON
          }
        }
      }
    } finally {
      reader.releaseLock();
    }

    if (!finalResult) {
      // If the stream was aborted, throw AbortError instead of generic message
      if (signal?.aborted) {
        throw new DOMException('The operation was aborted.', 'AbortError');
      }
      throw new Error('SSE stream ended without a final result');
    }

    return finalResult;
  }

  private extractSSEData(eventBlock: string): string | null {
    // SSE format: lines starting with "data: " contain the payload
    for (const line of eventBlock.split('\n')) {
      const trimmed = line.trim();
      if (trimmed.startsWith('data: ')) {
        return trimmed.slice(6);
      }
      if (trimmed.startsWith('data:')) {
        return trimmed.slice(5).trimStart();
      }
    }
    return null;
  }

  disconnect(): void {
    this._connected = false;
    this.tools = [];
  }

  private async sendRequest(method: string, params: unknown): Promise<unknown> {
    const id = ++this.requestId;
    const body = JSON.stringify({
      jsonrpc: '2.0',
      id,
      method,
      params: params || {},
    });

    const response = await fetch(this.url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body,
    });

    if (!response.ok) {
      throw new Error(`MCP server returned HTTP ${response.status}`);
    }

    const json: JsonRpcResponse = await response.json();
    if (json.error) {
      throw new Error(`MCP error [${json.error.code}]: ${json.error.message}`);
    }
    return json.result;
  }

  private async sendNotification(method: string): Promise<void> {
    const body = JSON.stringify({
      jsonrpc: '2.0',
      method,
    });

    try {
      await fetch(this.url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body,
      });
    } catch {
      // Notifications are fire-and-forget
    }
  }
}
