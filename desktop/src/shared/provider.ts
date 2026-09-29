/**
 * Providers -- where a remote turn goes, and what the app knows about it.
 *
 * A port of harmony/entry/src/main/ets/model/Provider.ets. The vendor list is
 * the same list on purpose: it is a first-run shortcut, and two clients
 * offering different shortcuts for the same service would be a bug report
 * nobody could reproduce.
 *
 * Two layers:
 *
 *   VENDOR  a built-in record describing one OpenAI-compatible service: its base
 *           URL, where to get a key, a couple of model names to start from.
 *
 *   MODEL   what the user actually configured: a vendor plus a model name. This
 *           is what the picker lists and what gets registered with the core.
 *
 * A user is never limited to the built-in list: `custom` exists so any endpoint
 * that speaks /chat/completions can be added by typing a URL.
 */

export interface Vendor {
  /** Stable; also the key store's alias for its key. */
  id: string
  label: string
  /** OpenAI-compatible root; the core appends /chat/completions. */
  baseUrl: string
  /** Suggestions, not a whitelist -- free text is always allowed. */
  models: string[]
  registerUrl: string
}

/**
 * A 2026-09 snapshot. Model names and free tiers rotate fast: these are starting
 * points to be edited in the UI, not a list to trust indefinitely.
 */
export const VENDORS: Vendor[] = [
  {
    id: 'zhipu',
    label: 'GLM',
    baseUrl: 'https://open.bigmodel.cn/api/paas/v4',
    models: ['glm-4.7-flash', 'GLM-5.3-Flash'],
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
    label: '阿里云 (DashScope)',
    baseUrl: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    models: ['qwen3.8-flash', 'qwen3.8-omni-flash-realtime'],
    registerUrl: 'https://bailian.console.aliyun.com'
  },
  {
    id: 'moonshot',
    label: 'Kimi',
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
    label: 'Google',
    baseUrl: 'https://generativelanguage.googleapis.com/v1beta/openai',
    models: ['gemini-3.8-flash', 'gemini-3.8-live'],
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
]

export function findVendor(id: string): Vendor | undefined {
  return VENDORS.find((v) => v.id === id)
}

/**
 * Vendor id for a model that runs ON THIS MACHINE. A sentinel rather than a real
 * Vendor record, because there is no endpoint, no key and no catalog behind it --
 * but reusing the vendor/model shape lets the picker, the selection, the chat
 * path and the removal path all stay lane-agnostic. It is deliberately absent
 * from VENDORS: adding one is a file picker, not a URL and a pasted key.
 */
export const LOCAL_VENDOR_ID = 'local'

/** `kind` for an on-device entry. Anything else (including absent) is remote. */
export const KIND_LOCAL = 'local'

/**
 * One model the user configured. Persisted (minus the key, which lives in the
 * OS key store) and registered with the core at startup.
 */
export interface ModelConfig {
  /**
   * '<vendorId>/<model>'. Doubles as the core's provider id, so it must be
   * stable: it is written into saved messages as the label's source and is what
   * a turn names when it starts.
   */
  id: string
  vendorId: string
  /** Vendor's base URL, or the user's own for a custom endpoint. Empty when local. */
  baseUrl: string
  /** The model name sent in the request body; the file name when local. */
  model: string

  /** KIND_LOCAL for an on-device model, absent for a remote one. */
  kind?: string
  /** Absolute path to the .gguf. Only meaningful when kind is KIND_LOCAL. */
  modelPath?: string
}

export function isLocal(cfg: ModelConfig): boolean {
  return cfg.kind === KIND_LOCAL
}

export function modelId(vendorId: string, model: string): string {
  return vendorId + '/' + model
}

/** What the picker shows: "GLM · glm-4.7-flash", or the file name locally. */
export function modelLabel(cfg: ModelConfig): string {
  if (isLocal(cfg)) {
    // No vendor to name, and the file name is what the user recognizes. The
    // ".gguf" suffix is dropped: every one of them has it, so it distinguishes
    // nothing and only costs width in a menu row.
    const name = cfg.model
    return name.endsWith('.gguf') ? name.slice(0, -5) : name
  }
  const v = findVendor(cfg.vendorId)
  return (v === undefined ? cfg.vendorId : v.label) + ' · ' + cfg.model
}
