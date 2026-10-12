/**
 * The interface's own words.
 *
 * Taken from examples/harmony/entry/src/main/resources/base/element/string.json, key for
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
  settingsAddOpen: '添加',
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
  settingsLocal: '添加本机模型',
  settingsLocalIntro:
    '选一个 GGUF 文件，直接在这台设备上跑，不联网。文件留在原处，不会复制进应用，删除模型也不会删文件。',
  settingsLocalAdd: '选择 GGUF 文件',
  settingsLocalOffer: '还没有本机模型，可以下载这一个：',
  settingsLocalOfferNote:
    'Qwen3.5 4B，4 位量化。选一个文件夹下载，文件夹里已有就不重复下载，下完自动加入模型列表；还没有选用的模型时直接选用并载入。已有其他 GGUF：点下面的「选择 GGUF 文件」。',
  settingsLocalLoading: '载入中…',
  settingsLocalReady: '已载入',

  // System prompt
  settingsPrompt: '系统提示词',
  settingsPromptIntro:
    '每一轮对话都会带上这段话，用来交代助手的身份、语气和边界。全局生效，对本机模型和远程模型一样。留空则不发送。',
  settingsPromptHint: '例：你是一个简洁、直接的助手，回答用中文。',
  settingsPromptDefault: '当前为内置默认提示词，随版本更新。',
  settingsPromptCustom: '当前为自定义提示词。',
  settingsPromptReset: '恢复默认',

  // Speech output
  settingsTts: '语音输出',
  settingsTtsIntro:
    'Qwen3-TTS 在本机朗读回答，合成时不联网。要两个 GGUF：talker 写出音频码，12 Hz tokenizer 解成采样。两个都就位后自动载入。',
  settingsTtsSetup: '还缺以下模型文件：',
  settingsTtsPickExisting: '选择已有文件',
  settingsTtsDownload: '下载到文件夹',
  settingsTtsSetupNote:
    '已有这些文件：选其中任意一个，同一文件夹里的其他文件按文件名自动填上。没有：选一个文件夹下载，文件夹里已有的不重复下载，下完自动载入。文件留在所选位置，应用不会移动或删除。',
  settingsTtsConnecting: '正在连接下载服务器…',
  settingsTtsDownloading: '正在下载',
  settingsTtsVerifying: '正在校验',
  settingsTtsRetrying: '连接中断，{s}s 后自动重试（第 {n} 次，共 {max} 次）',
  settingsTtsErrSpace: '所选文件夹所在的磁盘空间不足。',
  settingsTtsErrNetwork: '下载失败：多次重试后仍连不上下载服务器。再点一次下载会从中断处继续。',
  settingsTtsErrChecksum: '下载的文件校验不符，已删除。请重新下载。',
  settingsTtsErrDisk: '无法写入所选文件夹。',
  settingsTtsErrCanceled: '已取消下载。',
  settingsTtsUnavailable: '这个版本没有编译语音输出。用 `python examples/desktop/build.py qtts` 重新构建。',
  settingsTtsTalker: 'Talker 模型（Base）',
  settingsTtsTalkerCustom: 'Talker 模型（CustomVoice）',
  settingsTtsMode: '音色来源',
  settingsTtsModeBase: '克隆音色（Base）',
  settingsTtsModeCustom: '内置说话人（CustomVoice）',
  settingsTtsModeNote:
    '克隆音色用一段参考录音，可以换成自己的声音；内置说话人用模型自带的音色，目前听感更好，还能选朗读风格。两种模式各用各的 talker 模型，切换后自动重新载入。',
  settingsTtsSpeaker: '说话人',
  settingsTtsSpeakerNote: '模型自带的说话人，载入后可选。',
  settingsTtsStyle: '朗读风格',
  settingsTtsStyleNone: '不指定',
  settingsTtsStyleCalm: '平静',
  settingsTtsStyleCaring: '关切',
  settingsTtsStyleCheerful: '轻快开心',
  settingsTtsStyleSorry: '歉意',
  settingsTtsStyleNote: '随每次回复一起交给模型的一句简短描述，从下一次回复开始生效。',
  settingsTtsTokenizer: 'Tokenizer 模型',
  settingsTtsPick: '选择 GGUF 文件',
  settingsTtsNotPicked: '未选择',
  settingsTtsPickBoth: '两个模型文件就位后自动载入。',
  settingsTtsLoading: '正在载入…',
  settingsTtsLoaded: '已载入',
  settingsTtsSeed: '采样种子',
  settingsTtsSeedNote:
    '默认 101，节奏感较强。固定种子，同样的文字每次合成结果完全一致；换一个种子，整体读法会随之改变。填 -1 则每段重新抽，前后段的语气会不一样。',
  settingsTtsTemperature: '语调变化（采样温度）',
  settingsTtsTemperatureNote:
    '默认 1.3（模型自身默认 0.9）。调高后语速和语调更多变，读起来不那么像念稿；调得太高会漏字、念错或跑调。改动从下一段开始生效。',
  settingsTtsTemperatureCustomNote: '内置说话人固定用模型自身的 0.9，调高后听感不如克隆音色自然。这里的设置只对克隆音色生效。',
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
  settingsOmniUnavailable: '这个版本没有编译 omni。用 `python examples/desktop/build.py clean mnn` 重新构建。',
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

  // Speech output, reading a reply while it is still being written
  settingsReaderRef: '参考音色',
  settingsReaderRefOwn: '自选文件',
  settingsReaderRefPick: '选择文件',
  settingsReaderRefBusy: '处理中…',
  settingsReaderRefRecord: '录制',
  recordTitle: '录一段自己的声音',
  recordScript:
    '今天天气不错，我打算下午去湖边走一走。那里的风景很好，春天有花，秋天有鸟。如果你也有空，我们可以一起去，顺便找家小店喝杯茶，聊聊最近的事情。回来的时候，记得提醒我买点水果。',
  recordNote:
    '用平常说话的语气，把上面这段读完，约 20 秒。找个安静的地方，离麦克风一拳左右。录好后先回放听一下，再点“用这段录音”。',
  recordNoMic: '打不开麦克风',
  recordStart: '开始录音',
  recordStop: '停止',
  recordAgain: '重录',
  recordPlay: '回放',
  recordUse: '用这段录音',
  recordSaving: '正在提取…',
  recordTooShort: '太短了，至少录 10 秒。',
  recordClipped: '声音过载（削波），离麦克风远一点再录一遍。',
  settingsReaderRefNote:
    '下拉框列出应用自带的音色（speaker 目录），默认 01_female，载入时按模型尺寸自动选用对应的 .spk。也可以录制自己的声音，或选择 .wav 录音（单声道、16 位、24 kHz；实测用过 17 秒和 22 秒的录音）或 .spk 向量。录音首次使用时提取一次，约需数秒，之后直接读缓存；.spk 须与模型尺寸一致（1.7B 为 2048 维，0.6B 为 1024 维）。',
  settingsReaderMin: '攒够多少字再开口',
  settingsReaderMinNote:
    '0 表示用默认的 12。这个数决定的不是引擎快慢（出声都在 0.2 秒左右），而是大模型要先写多少字——本机模型实测 3.7 到 9.1 tok/s，36 个字就要等 6.4 秒。调大更自然（实测 30 比 12 好听），调小出声更早。',
  settingsReaderEnabled: '自动朗读回复',
  settingsReaderEnabledNote:
    '默认开启。回复边写边读，每写出一段就开始朗读。关闭后仍可点消息旁的朗读按钮。'
} as const

/** Picker labels for the three palette families, keyed the way prefs stores them. */
export const FAMILY_LABEL: Record<string, string> = {
  amber: S.settingsPaletteAmber,
  arctic: S.settingsPaletteArctic,
  quiet: S.settingsPaletteQuiet
}
