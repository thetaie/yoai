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
  chatDisclaimer: 'YoAI 是 AI，可能会出错，请自行核实回答内容。',
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
  settingsTtsTestText: '你好，我是 YoAI。这是本机合成的语音。',
  ttsSpeak: '朗读',
  ttsStop: '停止',
  ttsNotReady: '语音还没就绪。先到设置里选好两个模型。',
  ttsBusy: '正在朗读上一句，先等它说完。',

  // Omni
  settingsOmni: '端到端语音（omni）',
  settingsOmniIntro:
    '一个模型听、想、说，中间没有文字。开启后发消息走 omni，不再走「模型 + 语音输出」那条链。代价很大：3.8 GB 权重、第二套推理框架，本机实测合成比实时慢 3.1 倍。',
  settingsOmniUnavailable: '这个版本没有编译 omni。用 `build.cmd clean mnn` 重新构建。',
  settingsOmniDir: '模型目录',
  settingsOmniPick: '选择目录',
  settingsOmniNotPicked: '未选择',
  settingsOmniDirNote: '要的是目录不是文件：MNN 模型是 config.json 加十几个 .mnn 图。',
  settingsOmniLoad: '载入',
  settingsOmniLoading: '正在载入…',
  settingsOmniUnload: '卸载',
  settingsOmniReady: 'omni 已就绪',
  settingsOmniDit: '扩散步数',
  settingsOmniDitNote:
    '0 表示用模型自带的配置（5）。MNN 每块跑 steps-1 次 DiT 前向，调小更快——但被牺牲的是自然度，而自然度正是带这个引擎的全部理由。',
  settingsOmniEnabled: '用 omni 回答',
  settingsOmniEnabledNote: '开启后，输入框里的消息交给 omni，文字和语音都由它产生。',

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
  settingsClearHistoryMsg: '所有本机对话记录都会被删除，无法恢复。',

  // Reading a reply while it is still being written
  settingsReader: '边写边读',
  settingsReaderIntro:
    '大模型每吐出一句完整的话就送去合成，不必等整段写完。用的是上面那对模型文件，但另开一份引擎——载入它会卸掉上面那条。',
  settingsReaderUnavailable: '这个构建没有语音引擎。',
  settingsReaderLoad: '载入',
  settingsReaderLoading: '载入中…',
  settingsReaderUnload: '卸载',
  settingsReaderLoaded: '已载入',
  settingsReaderRef: '参考录音',
  settingsReaderRefPick: '选择',
  settingsReaderRefNone: '未选择',
  settingsReaderRefNote:
    '单声道 16 位 24 kHz 的 .wav，只用 base 模型。每一句都锚到同一段录音上，否则句与句之间会换音区——实测十六句里有一句低了五个半音。',
  settingsReaderMin: '攒够多少字再开口',
  settingsReaderMinNote:
    '0 表示用默认的 16。这个数决定的不是引擎快慢（出声都在 0.2 秒左右），而是大模型要先写多少字——本机模型实测 3.7 到 9.1 tok/s，36 个字就要等 6.4 秒。调大更自然（实测 30 比 12 好听），调小出声更早。',
  settingsReaderEnabled: '用这条链读回复',
  settingsReaderEnabledNote: '开启后，回复不再等写完才朗读，而是边写边读。'
} as const

/** Picker labels for the three palette families, keyed the way prefs stores them. */
export const FAMILY_LABEL: Record<string, string> = {
  amber: S.settingsPaletteAmber,
  arctic: S.settingsPaletteArctic,
  quiet: S.settingsPaletteQuiet
}
