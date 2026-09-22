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
  chatEmptyTitle: '你的 AI，运行在你自己的设备上',
  chatEmptyBody: '对话记录只保存在本机',
  drawerNewChat: '新对话',

  // Settings
  settingsTitle: '设置',
  settingsModels: '已配置的模型',
  settingsModelsEmpty: '还没有配置模型',
  settingsAdd: '添加远程模型',
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

  // System prompt
  settingsPrompt: '系统提示词',
  settingsPromptIntro:
    '每一轮对话都会带上这段话，用来交代助手的身份、语气和边界。全局生效，对本机模型和远程模型一样。留空则不发送。',
  settingsPromptHint: '例：你是一个简洁、直接的助手，回答用中文。',

  // Speech output
  settingsTts: '语音输出',
  settingsTtsIntro:
    'Qwen3-TTS 在本机朗读回答，不联网。要两个 GGUF：talker 写出音频码，12 Hz tokenizer 解成采样。两个都选好才能载入。',
  settingsTtsUnavailable: '这个版本没有编译语音输出。用 `build.cmd qtts` 重新构建。',
  settingsTtsTalker: 'Talker 模型',
  settingsTtsTokenizer: 'Tokenizer 模型',
  settingsTtsPick: '选择 GGUF 文件',
  settingsTtsNotPicked: '未选择',
  settingsTtsLoad: '载入',
  settingsTtsLoading: '正在载入…',
  settingsTtsUnload: '卸载',
  settingsTtsReady: '语音已就绪',
  settingsTtsVoice: '音色描述',
  settingsTtsVoiceHint: 'warm and clear, natural pace',
  settingsTtsVoiceNote:
    '用英文描述。VoiceDesign 必须有这段描述才能出声——同一段描述在不同文本上给出同一个音色，语速、气息、话筒远近都可以写。留空则恢复默认。',
  settingsTtsSpeaker: '发音人',
  settingsTtsSpeakerDefault: '模型默认',
  settingsTtsSpeakerNote:
    '只有 CustomVoice 检查点有发音人。逐句朗读时应当选一个：发音人是模型插入的一个 token，能在一句句独立合成之间锁住音色；而仅靠上面的文字描述只能大致拉住，前后句之间会听出差别。',
  settingsTtsSeed: '采样种子',
  settingsTtsSeedNote:
    '固定种子，同样的文字每次合成结果完全一致。填 -1 则每次重新抽，音色和语气会逐次变化——发音人和描述都拉不住这个，试听和朗读听起来像两个人就是这个原因。',
  settingsTtsAuto: '自动朗读回复',
  settingsTtsAutoNote:
    '开启后，模型写完回复就整段朗读一次。不按句切分——切开就是多次独立合成，前后接缝听得出来。文字回复照常显示，不受影响。',
  settingsTtsTest: '试听',
  settingsTtsTestText: '你好，我是 Body Clock。这是本机合成的语音。',
  ttsSpeak: '朗读',
  ttsStop: '停止',
  ttsNotReady: '语音还没就绪。先到设置里选好两个模型。',
  ttsBusy: '正在朗读上一句，先等它说完。',

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
