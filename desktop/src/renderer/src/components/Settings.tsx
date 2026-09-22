/**
 * Settings: the models the user has configured, and how the app looks.
 *
 * The same two jobs SettingsModels.ets and SettingsInterface.ets do on
 * HarmonyOS, on one scrolling page because a desktop window has the room.
 *
 * The key field is write-only by design. A saved key is never read back into
 * this form -- the main process will not hand one to a renderer at all -- so
 * the field says whether one exists and offers to replace it, which is every
 * operation anyone actually needs.
 */
import { useEffect, useState, type JSX } from 'react'
import {
  KIND_LOCAL,
  LOCAL_VENDOR_ID,
  ModelConfig,
  VENDORS,
  findVendor,
  isLocal,
  modelId,
  modelLabel
} from '@shared/provider'
import type { OmniStatus, TtsStatus } from '@shared/api'
import type { CoreStatus, InterfacePrefs } from '@shared/store'
import { FAMILY_LABEL, S } from '../strings'
import * as tts from '../tts'

interface Props {
  status: CoreStatus
  models: ModelConfig[]
  prefs: InterfacePrefs
  /** 'system' already answered, so the swatch shows what is actually in force. */
  resolvedMode: 'light' | 'dark'
  onModelsChanged: () => void | Promise<void>
  onPrefs: (patch: Partial<InterfacePrefs>) => void
  onClearSessions: () => void
  onClose: () => void
}

const SCALES: Array<{ value: number; label: string }> = [
  { value: 0.85, label: '更小' },
  { value: 0.925, label: '减小' },
  { value: 1, label: '正常' },
  { value: 1.15, label: '增大' },
  { value: 1.4, label: '更大' }
]

export function Settings({
  status,
  models,
  prefs,
  resolvedMode,
  onModelsChanged,
  onPrefs,
  onClearSessions,
  onClose
}: Props): JSX.Element {
  const [vendorId, setVendorId] = useState('zhipu')
  const [baseUrl, setBaseUrl] = useState('')
  const [model, setModel] = useState('')
  const [apiKey, setApiKey] = useState('')
  const [keyed, setKeyed] = useState<string[]>([])
  const [note, setNote] = useState<{ text: string; tone: 'danger' | 'success' } | null>(null)
  const [loadingLocal, setLoadingLocal] = useState('')
  /**
   * The preload result, tagged with the model it belongs to.
   *
   * Per model rather than in the page-wide `note`, which renders down in the
   * "add a model" section: clicking 预加载 in the list put its answer three
   * sections away, where it read as a comment on whatever was near it.
   */
  const [localNote, setLocalNote] = useState<{
    id: string
    text: string
    tone: 'danger' | 'success'
  } | null>(null)
  const [prompt, setPrompt] = useState<string | null>(null)
  const [ttsStatus, setTtsStatus] = useState<TtsStatus | null>(null)
  const [omniStatus, setOmniStatus] = useState<OmniStatus | null>(null)
  const [omniBusy, setOmniBusy] = useState(false)
  /**
   * The omni load result, shown in the omni section.
   *
   * Not the page-wide `note`, which renders down in "add a remote model":
   * loading takes ten seconds and its answer landed several screens away, so
   * the button looked like it did nothing at all.
   */
  const [omniNote, setOmniNote] = useState<{ text: string; tone: 'danger' | 'success' } | null>(
    null
  )
  const [ttsBusy, setTtsBusy] = useState(false)

  const vendor = findVendor(vendorId)
  const hasKey = keyed.includes(vendorId)

  const refreshKeys = async (): Promise<void> => {
    setKeyed(await window.bodyclock.models.vendorsWithKeys(VENDORS.map((v) => v.id)))
  }

  useEffect(() => {
    void refreshKeys()
  }, [])

  const refreshTts = async (): Promise<void> => {
    setTtsStatus(await window.bodyclock.tts.status())
  }

  useEffect(() => {
    void refreshTts()
  }, [])

  useEffect(() => {
    void (async () => {
      setPrompt((await window.bodyclock.chat.settings()).systemPrompt)
    })()
  }, [])

  const pickTts = async (which: 'talker' | 'tokenizer'): Promise<void> => {
    const picked = await window.bodyclock.tts.pickModel(which)
    if (picked !== null) {
      await refreshTts()
    }
  }

  const loadTts = async (): Promise<void> => {
    setTtsBusy(true)
    setNote(null)
    const reason = await window.bodyclock.tts.load()
    setTtsBusy(false)
    await refreshTts()
    setNote(
      reason.length === 0
        ? { text: S.settingsTtsReady, tone: 'success' }
        : { text: reason, tone: 'danger' }
    )
  }

  const unloadTts = async (): Promise<void> => {
    await tts.stop()
    await window.bodyclock.tts.unload()
    await refreshTts()
  }

  const testTts = async (): Promise<void> => {
    const reason = await tts.speak('settings-test', S.settingsTtsTestText)
    if (reason.length > 0) {
      setNote({ text: reason, tone: 'danger' })
      return
    }
    // A say that started can still fail -- the engine reports it on the event
    // stream, not on the call. Without this the only sign would be silence.
    const fail = tts.watchOnce()
    void fail.then((err) => {
      if (err.length > 0) {
        setNote({ text: err, tone: 'danger' })
      }
    })
  }

  const saveVoice = async (text: string): Promise<void> => {
    await window.bodyclock.tts.setVoice(text)
    await refreshTts()
  }

  const savePrompt = async (text: string): Promise<void> => {
    const next = await window.bodyclock.chat.setSettings({ systemPrompt: text })
    setPrompt(next.systemPrompt)
  }

  const setSpeaker = async (name: string): Promise<void> => {
    await window.bodyclock.tts.setSpeaker(name)
    await refreshTts()
  }

  const setSeed = async (raw: string): Promise<void> => {
    const n = Number.parseInt(raw, 10)
    await window.bodyclock.tts.setSeed(Number.isFinite(n) ? n : 7)
    await refreshTts()
  }

  const refreshOmni = async (): Promise<void> => {
    setOmniStatus(await window.bodyclock.omni.status())
  }

  useEffect(() => {
    void refreshOmni()
  }, [])

  const pickOmniDir = async (): Promise<void> => {
    if ((await window.bodyclock.omni.pickDir()) !== null) {
      await refreshOmni()
    }
  }

  const loadOmni = async (): Promise<void> => {
    setOmniBusy(true)
    setOmniNote(null)
    const reason = await window.bodyclock.omni.load()
    setOmniBusy(false)
    await refreshOmni()
    setOmniNote(
      reason.length === 0
        ? { text: S.settingsOmniReady, tone: 'success' }
        : { text: reason, tone: 'danger' }
    )
  }

  const unloadOmni = async (): Promise<void> => {
    await window.bodyclock.omni.unload()
    await refreshOmni()
  }

  const setOmni = async (patch: { ditSteps?: number; enabled?: boolean }): Promise<void> => {
    setOmniStatus(await window.bodyclock.omni.set(patch))
  }

  const setAuto = async (on: boolean): Promise<void> => {
    await window.bodyclock.tts.setAutoSpeak(on)
    await refreshTts()
  }

  // Prefill from the vendor record, which is the whole reason the built-in list
  // exists: the first run should be "paste a key", not "find out what this
  // vendor's base URL is".
  useEffect(() => {
    const v = findVendor(vendorId)
    setBaseUrl(v?.baseUrl ?? '')
    setModel(v?.models[0] ?? '')
    setApiKey('')
    setNote(null)
  }, [vendorId])

  const addRemote = async (): Promise<void> => {
    const url = baseUrl.trim()
    const name = model.trim()
    const key = apiKey.trim()

    // A key is required only when the vendor does not already have one: several
    // models can share it, and re-typing it to add a second model would be
    // asking for a mistake in a field nobody can read back.
    if (url.length === 0 || name.length === 0 || (key.length === 0 && !hasKey)) {
      setNote({ text: S.settingsNeedAll, tone: 'danger' })
      return
    }

    if (key.length > 0) {
      const result = await window.bodyclock.keys.save(vendorId, key)
      if (!result.ok) {
        setNote({ text: result.reason ?? '这个 key 没能保存', tone: 'danger' })
        return
      }
    }

    await window.bodyclock.models.add({
      id: modelId(vendorId, name),
      vendorId,
      baseUrl: url,
      model: name
    })
    setApiKey('')
    setNote({ text: S.settingsSaved, tone: 'success' })
    await refreshKeys()
    await onModelsChanged()
  }

  const addLocal = async (): Promise<void> => {
    const picked = await window.bodyclock.models.pickLocal()
    if (picked === null) {
      return
    }
    await window.bodyclock.models.add({
      id: modelId(LOCAL_VENDOR_ID, picked.name),
      vendorId: LOCAL_VENDOR_ID,
      baseUrl: '',
      model: picked.name,
      kind: KIND_LOCAL,
      modelPath: picked.path
    })
    await onModelsChanged()
  }

  const preload = async (id: string): Promise<void> => {
    setLoadingLocal(id)
    setLocalNote(null)
    const reason = await window.bodyclock.models.preload(id)
    setLoadingLocal('')
    setLocalNote(
      reason.length === 0
        ? { id, text: S.settingsLocalReady, tone: 'success' }
        : { id, text: reason, tone: 'danger' }
    )
  }

  const drop = async (cfg: ModelConfig): Promise<void> => {
    await window.bodyclock.models.remove(cfg.id)
    await onModelsChanged()
  }

  const locals = models.filter(isLocal)
  const remotes = models.filter((m) => !isLocal(m))

  return (
    <div className="settings">
      <div className="settings-inner">
        <div className="topbar" style={{ padding: 0, border: 0, background: 'none' }}>
          <h2 style={{ margin: 0, fontSize: '1.0625rem' }}>{S.settingsTitle}</h2>
          <div className="topbar-spacer" />
          <button className="btn" onClick={onClose}>
            返回对话
          </button>
        </div>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsModels}</h3>
          <p>{S.settingsIntro}</p>
          <div className="card">
            {models.length === 0 ? (
              <div className="row">
                <span className="row-label row-sub">{S.settingsModelsEmpty}</span>
              </div>
            ) : (
              models.map((m) => (
                <div className="row" key={m.id}>
                  <span className="row-label">
                    {modelLabel(m)}
                    {isLocal(m) && <span className="row-sub"> · 本机</span>}
                  </span>
                  {/* The answer lands where the button was. A model that is
                      resident has nothing left to click, so the success text
                      REPLACES the button; a failure keeps it, because the only
                      useful thing to do with a failure is try again. */}
                  {isLocal(m) &&
                    (localNote !== null && localNote.id === m.id && localNote.tone === 'success' ? (
                      <span className="field-note" data-tone="success">
                        {localNote.text}
                      </span>
                    ) : (
                      <>
                        {localNote !== null && localNote.id === m.id && (
                          <span className="field-note" data-tone="danger">
                            {localNote.text}
                          </span>
                        )}
                        <button
                          className="btn"
                          disabled={loadingLocal.length > 0}
                          onClick={() => void preload(m.id)}
                        >
                          {loadingLocal === m.id ? S.settingsLocalLoading : S.settingsLocalPreload}
                        </button>
                      </>
                    ))}
                  <button className="btn btn-danger" onClick={() => void drop(m)}>
                    {S.settingsDelete}
                  </button>
                </div>
              ))
            )}
          </div>
          {remotes.length > 0 && locals.length > 0 && (
            <p className="field-note" style={{ marginTop: 8 }}>
              远程 {remotes.length} 个，本机 {locals.length} 个。
            </p>
          )}
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsAdd}</h3>
          <div className="card">
            <div className="field">
              <label htmlFor="vendor">{S.settingsVendor}</label>
              <select
                id="vendor"
                className="control"
                value={vendorId}
                onChange={(e) => setVendorId(e.target.value)}
              >
                {VENDORS.map((v) => (
                  <option key={v.id} value={v.id}>
                    {v.label}
                  </option>
                ))}
              </select>
              {vendor !== undefined && vendor.registerUrl.length > 0 && (
                <button
                  className="link"
                  style={{ alignSelf: 'flex-start' }}
                  onClick={() => void window.bodyclock.openExternal(vendor.registerUrl)}
                >
                  {S.settingsGetKey}
                </button>
              )}
            </div>

            <div className="field">
              <label htmlFor="baseUrl">{S.settingsBaseUrl}</label>
              <input
                id="baseUrl"
                className="control"
                value={baseUrl}
                placeholder={S.settingsBaseUrlHint}
                onChange={(e) => setBaseUrl(e.target.value)}
              />
            </div>

            <div className="field">
              <label htmlFor="model">{S.settingsModel}</label>
              <input
                id="model"
                className="control"
                value={model}
                placeholder={S.settingsModelHint}
                list="model-suggestions"
                onChange={(e) => setModel(e.target.value)}
              />
              <datalist id="model-suggestions">
                {(vendor?.models ?? []).map((m) => (
                  <option key={m} value={m} />
                ))}
              </datalist>
            </div>

            <div className="field">
              <label htmlFor="apiKey">{S.settingsKey}</label>
              <input
                id="apiKey"
                className="control"
                type="password"
                value={apiKey}
                placeholder={hasKey ? S.settingsKeyKeep : S.settingsKeyHint}
                autoComplete="off"
                onChange={(e) => setApiKey(e.target.value)}
              />
              {hasKey && <span className="field-note">{S.settingsKeySaved}</span>}
              {!status.keysAvailable && (
                <span className="field-note" data-tone="danger">
                  这台机器没有可用的系统加密存储（{status.keyBackend || '无'}），key 不会被保存。
                </span>
              )}
            </div>

            <div className="row">
              <span className="row-label">
                {note !== null && (
                  <span className="field-note" data-tone={note.tone}>
                    {note.text}
                  </span>
                )}
              </span>
              <button className="btn btn-primary" onClick={() => void addRemote()}>
                {S.settingsSave}
              </button>
            </div>
          </div>
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsLocal}</h3>
          <p>{S.settingsLocalIntro}</p>
          {status.localAvailable ? (
            <>
              <div className="card">
                {locals.length === 0 ? (
                  <div className="row">
                    <span className="row-label row-sub">{S.settingsModelsEmpty}</span>
                  </div>
                ) : (
                  locals.map((m) => (
                    <div className="row" key={m.id}>
                      <span className="row-label">
                        {modelLabel(m)}
                        <div className="path">{m.modelPath}</div>
                      </span>
                    </div>
                  ))
                )}
              </div>
              <button className="btn" style={{ marginTop: 10 }} onClick={() => void addLocal()}>
                {S.settingsLocalAdd}
              </button>
            </>
          ) : (
            <p className="field-note" data-tone="danger">
              这个版本没有编译本机推理引擎。用 <code>desktop/build.cmd llama</code>
              （或 <code>./build.sh llama</code>）重新构建 addon 即可打开这条车道。
            </p>
          )}
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsPrompt}</h3>
          <p>{S.settingsPromptIntro}</p>
          <div className="field">
            {/* Uncontrolled with onBlur, like the voice description: a textarea
                that round-trips to disk on every keystroke fights the cursor. */}
            {prompt !== null && (
              <textarea
                className="control"
                rows={4}
                defaultValue={prompt}
                placeholder={S.settingsPromptHint}
                onBlur={(e) => void savePrompt(e.target.value)}
              />
            )}
          </div>
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsTts}</h3>
          <p>{S.settingsTtsIntro}</p>
          {ttsStatus !== null && !ttsStatus.available ? (
            <p className="field-note" data-tone="danger">
              {S.settingsTtsUnavailable}
            </p>
          ) : (
            <>
              <div className="card">
                {[
                  {
                    which: 'talker' as const,
                    label: S.settingsTtsTalker,
                    path: ttsStatus?.talkerPath ?? ''
                  },
                  {
                    which: 'tokenizer' as const,
                    label: S.settingsTtsTokenizer,
                    path: ttsStatus?.tokenizerPath ?? ''
                  }
                ].map((slot) => (
                  <div className="row" key={slot.which}>
                    <span className="row-label">
                      {slot.label}
                      <div className="path">
                        {slot.path.length > 0 ? slot.path : S.settingsTtsNotPicked}
                      </div>
                    </span>
                    <button className="btn" onClick={() => void pickTts(slot.which)}>
                      {S.settingsTtsPick}
                    </button>
                  </div>
                ))}
              </div>

              <div className="row" style={{ marginTop: 10, gap: 8 }}>
                {ttsStatus?.loaded === true ? (
                  <>
                    <button className="btn" onClick={() => void testTts()}>
                      {S.settingsTtsTest}
                    </button>
                    <button className="btn" onClick={() => void unloadTts()}>
                      {S.settingsTtsUnload}
                    </button>
                  </>
                ) : (
                  <button className="btn" disabled={ttsBusy} onClick={() => void loadTts()}>
                    {ttsBusy ? S.settingsTtsLoading : S.settingsTtsLoad}
                  </button>
                )}
              </div>

              {/* The voice, and on voice_design it is not a preference: the C
                  ABI REQUIRES an instruct there and say() fails without one, so
                  this field is what makes the checkpoint able to speak at all.
                  Optional on custom_voice, refused by base -- which is why the
                  field is shown only for the two that accept it. */}
              {ttsStatus !== null &&
                (ttsStatus.modelType === 'voice_design' ||
                  ttsStatus.modelType === 'custom_voice') && (
                  <div className="field" style={{ marginTop: 12 }}>
                    <label htmlFor="ttsVoice">{S.settingsTtsVoice}</label>
                    <input
                      id="ttsVoice"
                      className="control"
                      defaultValue={ttsStatus.voice}
                      placeholder={S.settingsTtsVoiceHint}
                      onBlur={(e) => void saveVoice(e.target.value)}
                    />
                    <span className="field-note">{S.settingsTtsVoiceNote}</span>
                  </div>
                )}

              {/* Only custom_voice carries names, and it is the checkpoint to
                  use when replies are read one sentence at a time: the speaker
                  is a token, so identity holds across independent utterances.
                  The description above still applies here -- it varies delivery
                  while this fixes who is speaking. */}
              {ttsStatus !== null && ttsStatus.speakers.length > 0 && (
                <div className="field" style={{ marginTop: 12 }}>
                  <label htmlFor="ttsSpeaker">{S.settingsTtsSpeaker}</label>
                  <select
                    id="ttsSpeaker"
                    className="control"
                    value={ttsStatus.speaker}
                    onChange={(e) => void setSpeaker(e.target.value)}
                  >
                    <option value="">{S.settingsTtsSpeakerDefault}</option>
                    {ttsStatus.speakers.map((name) => (
                      <option key={name} value={name}>
                        {name}
                      </option>
                    ))}
                  </select>
                  <span className="field-note">{S.settingsTtsSpeakerNote}</span>
                </div>
              )}

              {ttsStatus !== null && (
                <div className="field" style={{ marginTop: 12 }}>
                  <label htmlFor="ttsSeed">{S.settingsTtsSeed}</label>
                  <input
                    id="ttsSeed"
                    className="control"
                    type="number"
                    defaultValue={ttsStatus.seed}
                    onBlur={(e) => void setSeed(e.target.value)}
                  />
                  <span className="field-note">{S.settingsTtsSeedNote}</span>
                </div>
              )}

              <div className="field" style={{ marginTop: 12 }}>
                <label htmlFor="ttsAuto" style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                  <input
                    id="ttsAuto"
                    type="checkbox"
                    checked={ttsStatus?.autoSpeak === true}
                    onChange={(e) => void setAuto(e.target.checked)}
                  />
                  {S.settingsTtsAuto}
                </label>
                <span className="field-note">{S.settingsTtsAutoNote}</span>
              </div>

              {ttsStatus?.loaded === true && ttsStatus.modelType.length > 0 && (
                <p className="field-note" style={{ marginTop: 8 }}>
                  已载入 · {ttsStatus.modelType}
                </p>
              )}
            </>
          )}
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsOmni}</h3>
          <p>{S.settingsOmniIntro}</p>
          {omniStatus !== null && !omniStatus.available ? (
            <p className="field-note" data-tone="danger">
              {S.settingsOmniUnavailable}
            </p>
          ) : (
            <>
              <div className="card">
                <div className="row">
                  <span className="row-label">
                    {S.settingsOmniDir}
                    <div className="path">
                      {omniStatus !== null && omniStatus.modelDir.length > 0
                        ? omniStatus.modelDir
                        : S.settingsOmniNotPicked}
                    </div>
                  </span>
                  <button className="btn" onClick={() => void pickOmniDir()}>
                    {S.settingsOmniPick}
                  </button>
                </div>
              </div>
              <span className="field-note">{S.settingsOmniDirNote}</span>

              <div className="row" style={{ marginTop: 10, gap: 8 }}>
                {omniStatus?.loaded === true ? (
                  <button className="btn" onClick={() => void unloadOmni()}>
                    {S.settingsOmniUnload}
                  </button>
                ) : (
                  <button className="btn" disabled={omniBusy} onClick={() => void loadOmni()}>
                    {omniBusy ? S.settingsOmniLoading : S.settingsOmniLoad}
                  </button>
                )}
                {omniNote !== null && (
                  <span className="field-note" data-tone={omniNote.tone}>
                    {omniNote.text}
                  </span>
                )}
              </div>

              {omniStatus !== null && (
                <div className="field" style={{ marginTop: 12 }}>
                  <label htmlFor="omniDit">{S.settingsOmniDit}</label>
                  <input
                    id="omniDit"
                    className="control"
                    type="number"
                    defaultValue={omniStatus.ditSteps}
                    onBlur={(e) => void setOmni({ ditSteps: Number.parseInt(e.target.value, 10) })}
                  />
                  <span className="field-note">{S.settingsOmniDitNote}</span>
                </div>
              )}

              <div className="field" style={{ marginTop: 12 }}>
                <label htmlFor="omniOn" style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                  <input
                    id="omniOn"
                    type="checkbox"
                    checked={omniStatus?.enabled === true}
                    onChange={(e) => void setOmni({ enabled: e.target.checked })}
                  />
                  {S.settingsOmniEnabled}
                </label>
                <span className="field-note">{S.settingsOmniEnabledNote}</span>
              </div>
            </>
          )}
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsInterface}</h3>
          <p>{S.settingsInterfaceIntro}</p>
          <div className="card">
            <div className="field">
              <label>{S.settingsTheme}</label>
              <div style={{ display: 'flex', gap: 8 }}>
                {(
                  [
                    ['system', S.themeSystem],
                    ['light', S.themeLight],
                    ['dark', S.themeDark]
                  ] as const
                ).map(([value, label]) => (
                  <button
                    key={value}
                    className={prefs.colorMode === value ? 'btn btn-primary' : 'btn'}
                    onClick={() => onPrefs({ colorMode: value })}
                  >
                    {label}
                  </button>
                ))}
              </div>
            </div>

            <div className="field">
              <label>{S.settingsPalette}</label>
              <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                {Object.keys(FAMILY_LABEL).map((family) => (
                  <button
                    key={family}
                    className={prefs.paletteFamily === family ? 'btn btn-primary' : 'btn'}
                    onClick={() => onPrefs({ paletteFamily: family })}
                  >
                    <span className="swatches" data-swatch={`${family}-${resolvedMode}`}>
                      <i className="swatch" />
                      <i className="swatch" />
                      <i className="swatch" />
                    </span>{' '}
                    {FAMILY_LABEL[family]}
                  </button>
                ))}
              </div>
              <span className="field-note">{S.settingsPaletteNote}</span>
            </div>

            <div className="field">
              <label>{S.settingsTextSize}</label>
              <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
                {SCALES.map((s) => (
                  <button
                    key={s.value}
                    className={prefs.fontScale === s.value ? 'btn btn-primary' : 'btn'}
                    onClick={() => onPrefs({ fontScale: s.value })}
                  >
                    {s.label}
                  </button>
                ))}
              </div>
              <span className="field-note">{S.settingsTextSizeNote}</span>
            </div>
          </div>
        </section>

        {/*------------------------------------------------------------------*/}
        <section className="section">
          <h3>{S.settingsCore}</h3>
          <div className="card">
            <div className="row">
              <span className="row-label">
                <span className="path">{status.detail}</span>
              </span>
            </div>
            <div className="row">
              <span className="row-label">
                本机推理引擎
                <div className="row-sub">{status.localAvailable ? '已编译' : '未编译'}</div>
              </span>
            </div>
            <div className="row">
              <span className="row-label">
                密钥存储
                <div className="row-sub">{status.keyBackend || '不可用'}</div>
              </span>
            </div>
            <div className="row">
              <span className="row-label">
                {S.settingsClearHistory}
                <div className="row-sub">{S.settingsClearHistoryMsg}</div>
              </span>
              <button className="btn btn-danger" onClick={onClearSessions}>
                {S.settingsDelete}
              </button>
            </div>
          </div>
        </section>
      </div>
    </div>
  )
}
