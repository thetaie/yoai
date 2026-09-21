/**
 * The interface's own words.
 *
 * Taken from harmony/entry/src/main/resources/base/element/string.json, key for
 * key, so the two clients say the same thing about the same feature. Where a
 * line reads oddly here it probably reads oddly there too, and should be fixed
 * in that file first -- it is the one under a translation workflow (see
 * design/i18n-status.md).
 *
 * Flat constants rather than a framework: this client is Chinese-only today,
 * and a lookup with one language behind it is a lookup that hides how little is
 * actually there. When a second language lands, this object is what it replaces.
 */
export const S = {
  // Chat
  chatNeedModel: '还没有可用的模型。先到设置里添加一个服务的 key。',
  chatEmptyReply: '模型没有返回内容，换个模型或稍后再试。',
  chatThinkingLabel: '思考过程',
  chatStopped: '已停止',
  chatDisclaimer: 'Body Clock 是 AI，可能会出错，请自行核实回答内容。',
  chatPickModel: '选择模型',
  chatEmptyTitle: '你的 AI，跑在你自己的设备上',
  chatEmptyBody: '对话记录只保存在本机。用你自己的 API key 调用模型。',
  drawerNewChat: '新对话',

  // Settings
  settingsTitle: '设置',
  settingsModels: '已配置的模型',
  settingsModelsEmpty: '还没有配置模型',
  settingsAdd: '添加模型',
  settingsIntro: '每个服务一个 key，只存在本机的加密存储里，绝不上传。',
  settingsVendor: '服务',
  settingsBaseUrl: '接口地址',
  settingsBaseUrlHint: 'https://…/v1（不要带 /chat/completions）',
  settingsModel: '模型名',
  settingsModelHint: '请求里发送的模型名',
  settingsKey: 'API Key',
  settingsKeyHint: '粘贴 key',
  settingsKeySaved: '已保存该服务的 key',
  settingsKeyKeep: '留空则沿用已保存的 key',
  settingsSave: '保存',
  settingsCancel: '取消',
  settingsDelete: '删除',
  settingsGetKey: '去获取 key ›',
  settingsNeedAll: '接口地址、模型名和 key 都要填。',
  settingsSaved: '已保存',

  // On-device models
  settingsLocal: '本机模型',
  settingsLocalIntro:
    '选一个 GGUF 文件，直接在这台设备上跑，不联网。文件留在原处，不会复制进应用，删除模型也不会删文件。',
  settingsLocalAdd: '选择 GGUF 文件',
  settingsLocalLoading: '正在加载模型…',
  settingsLocalReady: '模型已就绪',
  settingsLocalPreload: '预加载',

  // Appearance
  settingsInterface: '界面',
  settingsInterfaceIntro: '改动即时生效，无需重启。',
  settingsTheme: '主题',
  settingsPalette: '主题色',
  settingsPaletteAmber: 'Amber · 暖',
  settingsPaletteArctic: 'Arctic · 冷',
  settingsPaletteQuiet: 'Quiet · 低饱和',
  settingsPaletteNote:
    '色系和深浅是两件事：这里选色系，上面选深浅，色卡显示的是当前实际生效的那一套。',
  settingsTextSize: '文字大小',
  settingsTextSizeNote: '只作用于对话正文，界面本身的尺寸不变。',
  themeSystem: '跟随系统',
  themeLight: '浅色',
  themeDark: '深色',

  // Data and diagnostics
  settingsCore: '内核',
  settingsClearHistory: '清空所有对话记录',
  settingsClearHistoryMsg: '所有本机对话记录都会被删除，无法恢复。'
} as const

/** Picker labels for the three palette families, keyed the way prefs stores them. */
export const FAMILY_LABEL: Record<string, string> = {
  amber: S.settingsPaletteAmber,
  arctic: S.settingsPaletteArctic,
  quiet: S.settingsPaletteQuiet
}
