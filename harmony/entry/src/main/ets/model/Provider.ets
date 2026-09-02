/**
 * Providers -- where a remote turn goes, and what the app knows about it.
 *
 * Two layers, on purpose:
 *
 *   VENDOR  a built-in record describing one OpenAI-compatible service: its base
 *           URL, where to get a key, a couple of model names to start from. Ships
 *           with the app so the first-run flow is "paste a key", not "find out
 *           what this vendor's base URL is".
 *
 *   MODEL   what the user actually configured: a vendor plus a model name. This
 *           is what the picker lists and what gets registered with the core.
 *
 * A user is never limited to the built-in list: `custom` exists so any endpoint
 * that speaks /chat/completions can be added by typing a URL. That is the whole
 * point of a bring-your-own-key client, and treating the built-ins as anything
 * more than a shortcut would undercut it.
 */

export interface Vendor {
  id: string;         // stable; also the secure-store alias for its key
  label: string;      // shown in the UI
  baseUrl: string;    // OpenAI-compatible root; the core appends /chat/completions
  models: string[];   // suggestions, not a whitelist -- free text is always allowed
  registerUrl: string;// where to get a key
}

/**
 * A 2026-09 snapshot. Model names and free tiers rotate fast: these are starting
 * points to be edited in the UI, not a list to trust indefinitely.
 */
export const VENDORS: Vendor[] = [
  {
    id: 'zhipu',
    label: '智谱 GLM',
    baseUrl: 'https://open.bigmodel.cn/api/paas/v4',
    models: ['glm-4.7-flash', 'glm-4.7'],
    registerUrl: 'https://open.bigmodel.cn'
  },
  {
    id: 'deepseek',
    label: 'DeepSeek',
    baseUrl: 'https://api.deepseek.com/v1',
    models: ['deepseek-chat', 'deepseek-reasoner'],
    registerUrl: 'https://platform.deepseek.com'
  },
  {
    id: 'dashscope',
    label: '阿里云百炼 (DashScope)',
    baseUrl: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    models: ['qwen-plus', 'qwen-turbo'],
    registerUrl: 'https://bailian.console.aliyun.com'
  },
  {
    id: 'moonshot',
    label: '月之暗面 Kimi',
    baseUrl: 'https://api.moonshot.cn/v1',
    models: ['kimi-k2-turbo-preview'],
    registerUrl: 'https://platform.moonshot.cn'
  },
  {
    id: 'openai',
    label: 'OpenAI',
    baseUrl: 'https://api.openai.com/v1',
    models: ['gpt-5-nano', 'gpt-5-mini'],
    registerUrl: 'https://platform.openai.com/api-keys'
  },
  {
    id: 'gemini',
    label: 'Google Gemini',
    baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    models: ['gemini-3.6-flash'],
    registerUrl: 'https://aistudio.google.com/apikey'
  },
  {
    id: 'openrouter',
    label: 'OpenRouter',
    baseUrl: 'https://openrouter.ai/api/v1',
    models: ['moonshotai/kimi-k2.6', 'deepseek/deepseek-chat'],
    registerUrl: 'https://openrouter.ai/keys'
  },
  {
    id: 'custom',
    label: '自定义 (OpenAI 兼容)',
    baseUrl: '',
    models: [],
    registerUrl: ''
  }
];

export function findVendor(id: string): Vendor | undefined {
  for (const v of VENDORS) {
    if (v.id === id) {
      return v;
    }
  }
  return undefined;
}

/**
 * One model the user configured. Persisted (minus the key, which lives in the
 * secure store) and registered with the core at startup.
 */
export interface ModelConfig {
  /**
   * '<vendorId>/<model>'. Doubles as the core's provider id, so it must be
   * stable: it is written into saved messages as the label's source and is what
   * a turn names when it starts.
   */
  id: string;
  vendorId: string;
  /** Vendor's base URL, or the user's own for a custom endpoint. */
  baseUrl: string;
  /** The model name sent in the request body. */
  model: string;
}

export function modelId(vendorId: string, model: string): string {
  return vendorId + '/' + model;
}

/** What the picker shows: "智谱 GLM · glm-4.7-flash". */
export function modelLabel(cfg: ModelConfig): string {
  const v: Vendor | undefined = findVendor(cfg.vendorId);
  const vendorLabel: string = v === undefined ? cfg.vendorId : v.label;
  return vendorLabel + ' · ' + cfg.model;
}
