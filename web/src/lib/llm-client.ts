export interface ToolCall {
  id: string;
  type: 'function';
  function: { name: string; arguments: string };
}

export interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content?: string | null;
  tool_calls?: ToolCall[];
  tool_call_id?: string;
}

export interface OpenAITool {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: Record<string, unknown>;
  };
}

export interface ChatResponse {
  message: { content?: string | null; tool_calls?: ToolCall[] };
  done: boolean;
}

/** Callback types for streaming */
export interface StreamCallbacks {
  onText: (text: string) => void;           // incremental text delta
  onToolCallStart: (tc: ToolCall) => void;  // tool call begins (name known, args streaming)
  onToolCallArgs: (id: string, delta: string) => void; // incremental args delta
  onDone: (message: { content: string | null; tool_calls?: ToolCall[] }) => void;
  onError: (err: Error) => void;
}

export class LlmClient {
  private baseUrl: string;
  private apiKey: string;
  private model: string;

  constructor(baseUrl: string, apiKey: string, model: string) {
    this.baseUrl = baseUrl;
    this.apiKey = apiKey;
    this.model = model;
  }

  updateConfig(baseUrl: string, apiKey: string, model: string): void {
    this.baseUrl = baseUrl;
    this.apiKey = apiKey;
    this.model = model;
  }

  /** Non-streaming chat (fallback) */
  async chat(messages: ChatMessage[], tools: OpenAITool[], signal?: AbortSignal): Promise<ChatResponse> {
    const body: Record<string, unknown> = {
      model: this.model,
      messages,
      max_tokens: 4096,
    };

    if (tools.length > 0) {
      body.tools = tools;
      body.tool_choice = 'auto';
    }

    const url = `${this.baseUrl.replace(/\/+$/, '')}/chat/completions`;
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
    };
    if (this.apiKey) {
      headers['Authorization'] = `Bearer ${this.apiKey}`;
    }

    const response = await fetch(url, {
      method: 'POST',
      headers,
      body: JSON.stringify(body),
      signal,
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`LLM API error (${response.status}): ${errorText}`);
    }

    const json = await response.json();
    const choice = json.choices?.[0];
    if (!choice) throw new Error('No choices in LLM response');

    const msg = choice.message;
    const hasToolCalls = msg.tool_calls && msg.tool_calls.length > 0;

    return {
      message: {
        content: msg.content || null,
        tool_calls: hasToolCalls ? msg.tool_calls : undefined,
      },
      done: !hasToolCalls,
    };
  }

  /** Streaming chat — parses SSE and calls callbacks in real-time */
  async chatStream(
    messages: ChatMessage[],
    tools: OpenAITool[],
    callbacks: StreamCallbacks,
    signal?: AbortSignal,
  ): Promise<{ content: string | null; tool_calls?: ToolCall[] }> {
    const body: Record<string, unknown> = {
      model: this.model,
      messages,
      max_tokens: 4096,
      stream: true,
    };

    if (tools.length > 0) {
      body.tools = tools;
      body.tool_choice = 'auto';
    }

    const url = `${this.baseUrl.replace(/\/+$/, '')}/chat/completions`;
    const headers: Record<string, string> = {
      'Content-Type': 'application/json',
    };
    if (this.apiKey) {
      headers['Authorization'] = `Bearer ${this.apiKey}`;
    }

    const response = await fetch(url, {
      method: 'POST',
      headers,
      body: JSON.stringify(body),
      signal,
    });

    if (!response.ok) {
      const errorText = await response.text();
      const err = new Error(`LLM API error (${response.status}): ${errorText}`);
      callbacks.onError(err);
      return { content: null };
    }

    const reader = response.body?.getReader();
    if (!reader) {
      const err = new Error('No response body');
      callbacks.onError(err);
      return { content: null };
    }

    const decoder = new TextDecoder();
    let buffer = '';

    // Accumulated state
    let fullContent = '';
    const toolCallsMap = new Map<number, ToolCall>();

    try {
      while (true) {
        if (signal?.aborted) break;
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        // Keep the last potentially incomplete line
        buffer = lines.pop() || '';

        for (const line of lines) {
          const trimmed = line.trim();
          if (!trimmed || trimmed === 'data: [DONE]') continue;
          if (!trimmed.startsWith('data: ')) continue;

          try {
            const json = JSON.parse(trimmed.slice(6));
            const delta = json.choices?.[0]?.delta;
            if (!delta) continue;

            // Text content
            if (delta.content) {
              fullContent += delta.content;
              callbacks.onText(delta.content);
            }

            // Tool calls
            if (delta.tool_calls) {
              for (const tc of delta.tool_calls) {
                const idx = tc.index ?? 0;
                if (!toolCallsMap.has(idx)) {
                  // New tool call
                  const newTc: ToolCall = {
                    id: tc.id || '',
                    type: 'function',
                    function: {
                      name: tc.function?.name || '',
                      arguments: tc.function?.arguments || '',
                    },
                  };
                  toolCallsMap.set(idx, newTc);
                  callbacks.onToolCallStart(newTc);
                } else {
                  // Append arguments
                  const existing = toolCallsMap.get(idx)!;
                  if (tc.function?.arguments) {
                    existing.function.arguments += tc.function.arguments;
                    callbacks.onToolCallArgs(existing.id, tc.function.arguments);
                  }
                  if (tc.id) existing.id = tc.id;
                  if (tc.function?.name) existing.function.name = tc.function.name;
                }
              }
            }
          } catch {
            // Skip malformed JSON
          }
        }
      }
    } catch (err: any) {
      // If aborted by user, call onDone with partial message before returning
      if (signal?.aborted || err.name === 'AbortError') {
        const partialMessage = {
          content: fullContent || null,
          tool_calls: toolCallsMap.size > 0 ? Array.from(toolCallsMap.values()) : undefined,
        };
        callbacks.onDone(partialMessage);
        return partialMessage;
      }
      callbacks.onError(err);
      return { content: null };
    }

    // If we broke out of the while loop due to signal.aborted, call onDone
    if (signal?.aborted) {
      const partialMessage = {
        content: fullContent || null,
        tool_calls: toolCallsMap.size > 0 ? Array.from(toolCallsMap.values()) : undefined,
      };
      callbacks.onDone(partialMessage);
      return partialMessage;
    }

    // Final assembled message
    const toolCalls = toolCallsMap.size > 0
      ? Array.from(toolCallsMap.values())
      : undefined;

    const finalMessage = {
      content: fullContent || null,
      tool_calls: toolCalls,
    };

    callbacks.onDone(finalMessage);
    return finalMessage;
  }
}

export function mcpToOpenAITools(mcpTools: Array<{ name: string; description: string; inputSchema: Record<string, unknown> }>): OpenAITool[] {
  return mcpTools.map(tool => ({
    type: 'function' as const,
    function: {
      name: tool.name,
      description: tool.description,
      parameters: tool.inputSchema,
    },
  }));
}
