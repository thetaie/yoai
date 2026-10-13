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
import type {
  ChatSettings,
  LocalOffer,
  ModelDownload,
  ModelLoadState,
  ReaderStatus,
  OmniStatus,
  TtsStatus
} from '@shared/api'
import type { CoreStatus, InterfacePrefs } from '@shared/store'
import { FAMILY_LABEL, S } from '../strings'
import * as tts from '../tts'
import { VoiceRecorder } from './VoiceRecorder'

interface Props {
  status: CoreStatus
  models: ModelConfig[]
  prefs: InterfacePrefs
  /** 'system' already answered, so the swatch shows what is actually in force. */
  resolvedMode: 'light' | 'dark'
  onModelsChanged: () => void | Promise<void>
  onPrefs: (patch: Partial<InterfacePrefs>) => void
  onClearSessions: () => void
  /**
   * Whether there is speech to stop: this page's test or a recorded take, or a
   * reply from the chat still being read. The composer, which offers 停止
   * there, is not on this page.
   */
  speaking: boolean
  onStopSpeech: () => void
  onClose: () => void
}

const SCALES: Array<{ value: number; label: string }> = [
  { value: 0.85, label: '更小' },
  { value: 0.925, label: '减小' },
  { value: 1, label: '正常' },
  { value: 1.15, label: '增大' },
  { value: 1.4, label: '更大' }
]

const DOWNLOAD_ERROR: Record<Exclude<ModelDownload['error'], ''>, string> = {
  space: S.settingsTtsErrSpace,
  network: S.settingsTtsErrNetwork,
  checksum: S.settingsTtsErrChecksum,
  disk: S.settingsTtsErrDisk,
  canceled: S.settingsTtsErrCanceled
}

/** Decimal units, the way model sizes are quoted: 1.5 GB, 291 MB. */
function formatBytes(n: number): string {
  return n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : `${Math.round(n / 1e6)} MB`
}

/**
 * Follow a download while it runs. It runs in the main process, so it may
 * already be under way when this page opens. `run` is bumped when one is
 * started here, which starts following it; `onOver` gets how it ended.
 */
function useDownload(
  status: () => Promise<ModelDownload>,
  run: number,
  onOver: (d: ModelDownload) => void | Promise<void>
): ModelDownload | null {
  const [download, setDownload] = useState<ModelDownload | null>(null)
  useEffect(() => {
    let stopped = false
    let timer: ReturnType<typeof setTimeout> | undefined
    // Started from here: its end matters even if it was never seen running,
    // which is what a folder already holding every file looks like.
    let started = run > 0
    const poll = async (): Promise<void> => {
      const d = await status()
      if (stopped) {
        return
      }
      setDownload(d)
      if (d.running) {
        started = true
        timer = setTimeout(() => void poll(), 500)
      } else if (started) {
        await onOver(d)
      }
    }
    void poll()
    return () => {
      stopped = true
      if (timer !== undefined) {
        clearTimeout(timer)
      }
    }
  }, [run])
  return download
}

/** A running download: what it is doing, how far it has got, and a way to stop it. */
function DownloadProgress({ d, onCancel }: { d: ModelDownload; onCancel: () => void }): JSX.Element {
  return (
    <div className="field">
      <label>
        {d.retry > 0
          ? S.settingsTtsRetrying
              .replace('{s}', String(d.retryIn))
              .replace('{n}', String(d.retry))
              .replace('{max}', String(d.retries))
          : d.file.length === 0
            ? S.settingsTtsConnecting
            : `${d.verifying ? S.settingsTtsVerifying : S.settingsTtsDownloading} ${d.file}`}
      </label>
      <progress className="meter" max={Math.max(d.total, 1)} value={d.received} />
      <div className="row" style={{ padding: 0 }}>
        <span className="row-label row-sub">
          {formatBytes(d.received)} / {formatBytes(d.total)}
          {d.host.length > 0 && ` · ${d.host}`}
        </span>
        <button className="btn" onClick={onCancel}>
          {S.settingsCancel}
        </button>
      </div>
      <div className="path">{d.dir}</div>
    </div>
  )
}

/** Why the last download stopped; nothing when it did not. */
function DownloadFailure({ d }: { d: ModelDownload | null }): JSX.Element | null {
  if (d === null || d.error === '') {
    return null
  }
  return (
    <>
      <span className="field-note" data-tone={d.error === 'canceled' ? undefined : 'danger'}>
        {DOWNLOAD_ERROR[d.error]}
      </span>
      {d.detail.length > 0 && <div className="path">{d.detail}</div>}
    </>
  )
}

export function Settings({
  status,
  models,
  prefs,
  resolvedMode,
  onModelsChanged,
  onPrefs,
  onClearSessions,
  speaking,
  onStopSpeech,
  onClose
}: Props): JSX.Element {
  const [vendorId, setVendorId] = useState('zhipu')
  const [baseUrl, setBaseUrl] = useState('')
  const [model, setModel] = useState('')
  const [apiKey, setApiKey] = useState('')
  const [keyed, setKeyed] = useState<string[]>([])
  const [note, setNote] = useState<{ text: string; tone: 'danger' | 'success' } | null>(null)
  /** The add-a-remote-model form, folded away until someone asks for it. */
  const [addOpen, setAddOpen] = useState(false)
  /** How each on-device model's automatic preload went, by model id. */
  const [loadStates, setLoadStates] = useState<Record<string, ModelLoadState>>({})
  const [prompt, setPrompt] = useState<ChatSettings | null>(null)
  /** Bumped by a reset, which remounts the prompt box with the default in it. */
  const [promptRev, setPromptRev] = useState(0)
  const [ttsStatus, setTtsStatus] = useState<TtsStatus | null>(null)
  /** Bumped when a download is started here, which starts following it. */
  const [downloadRun, setDownloadRun] = useState(0)
  /** The on-device model a download would fetch, and its download. */
  const [offer, setOffer] = useState<LocalOffer | null>(null)
  const [llmDownloadRun, setLlmDownloadRun] = useState(0)
  const [readerStatus, setReaderStatus] = useState<ReaderStatus | null>(null)
  /** A reference voice being picked, or extracted from a recording. */
  const [refBusy, setRefBusy] = useState(false)
  /** The recorder open under the reference voice. */
  const [recording, setRecording] = useState(false)
  const [readerNote, setReaderNote] = useState<{ text: string; tone: 'danger' | 'success' } | null>(
    null
  )
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

  const vendor = findVendor(vendorId)
  const hasKey = keyed.includes(vendorId)

  const refreshKeys = async (): Promise<void> => {
    setKeyed(await window.yoai.models.vendorsWithKeys(VENDORS.map((v) => v.id)))
  }

  useEffect(() => {
    void refreshKeys()
  }, [])

  const refreshTts = async (): Promise<void> => {
    setTtsStatus(await window.yoai.tts.status())
  }

  useEffect(() => {
    void refreshTts()
  }, [])

  // Polled while any preload is still reading, which is seconds: the page is
  // often opened while the weights are still coming in at startup.
  useEffect(() => {
    let stopped = false
    let timer: ReturnType<typeof setTimeout> | undefined
    const poll = async (): Promise<void> => {
      const states = await window.yoai.models.loadStates()
      if (stopped) {
        return
      }
      setLoadStates(states)
      if (Object.values(states).some((st) => st.state === 'loading')) {
        timer = setTimeout(() => void poll(), 1000)
      }
    }
    void poll()
    return () => {
      stopped = true
      if (timer !== undefined) {
        clearTimeout(timer)
      }
    }
  }, [models])

  useEffect(() => {
    void (async () => {
      setPrompt(await window.yoai.chat.settings())
    })()
  }, [])

  /**
   * The voice loads in the background -- at startup, and after a model file is
   * picked -- so the page follows it until it is up or has said why not.
   */
  const followVoice = async (): Promise<void> => {
    for (let i = 0; i < 30; i++) {
      const r = await window.yoai.reader.status()
      setReaderStatus(r)
      if (r.loaded || r.note.length > 0) {
        return
      }
      await new Promise((resolve) => setTimeout(resolve, 1000))
    }
  }

  useEffect(() => {
    void followVoice()
  }, [])

  const setVoiceMode = async (mode: 'base' | 'custom'): Promise<void> => {
    await window.yoai.tts.setVoiceMode(mode)
    await refreshTts()
    void followVoice()
  }

  const pickTts = async (which: 'talker' | 'customTalker' | 'tokenizer'): Promise<void> => {
    const picked = await window.yoai.tts.pickModel(which)
    if (picked !== null) {
      await refreshTts()
      void followVoice()
    }
  }

  const pickAnyTts = async (): Promise<void> => {
    if ((await window.yoai.tts.pickAny()) !== null) {
      await refreshTts()
      void followVoice()
    }
  }

  const startDownload = async (): Promise<void> => {
    if (await window.yoai.tts.download()) {
      setDownloadRun((n) => n + 1)
    }
  }

  const download = useDownload(window.yoai.tts.downloadStatus, downloadRun, async (d) => {
    // Over: show the paths it stored, and follow the load it started.
    await refreshTts()
    if (d.error === '') {
      void followVoice()
    }
  })

  useEffect(() => {
    void (async () => {
      setOffer(await window.yoai.models.offer())
    })()
  }, [])

  const startLlmDownload = async (): Promise<void> => {
    if (await window.yoai.models.download()) {
      setLlmDownloadRun((n) => n + 1)
    }
  }

  const llmDownload = useDownload(window.yoai.models.downloadStatus, llmDownloadRun, async (d) => {
    // Over: list the model it added, whose preload the list then follows.
    if (d.error === '') {
      await onModelsChanged()
    }
  })

  const testTts = async (): Promise<void> => {
    setReaderNote(null)
    const reason = await tts.speak('settings-test', S.settingsTtsTestText)
    if (reason.length > 0) {
      setReaderNote({ text: reason, tone: 'danger' })
      return
    }
    // A stream that started can still fail, and says so when it ends. Without
    // this the only sign would be silence.
    void tts.watchOnce().then((err) => {
      if (err.length > 0) {
        setReaderNote({ text: err, tone: 'danger' })
      }
    })
  }

  const savePrompt = async (text: string): Promise<void> => {
    // Left as it was: nothing to store, and nothing for the model to warm again.
    if (prompt !== null && text === prompt.systemPrompt) {
      return
    }
    setPrompt(await window.yoai.chat.setSettings({ systemPrompt: text }))
  }

  const resetPrompt = async (): Promise<void> => {
    setPrompt(await window.yoai.chat.setSettings({ systemPrompt: null }))
    setPromptRev((n) => n + 1)
  }

  const setSeed = async (raw: string): Promise<void> => {
    // Not a number goes over as NaN, and the main process puts its default in.
    await window.yoai.tts.setSeed(Number.parseInt(raw, 10))
    await refreshTts()
  }

  const refreshOmni = async (): Promise<void> => {
    setOmniStatus(await window.yoai.omni.status())
    setReaderStatus(await window.yoai.reader.status())
  }

  useEffect(() => {
    void refreshOmni()
  }, [])

  const pickOmniDir = async (): Promise<void> => {
    if ((await window.yoai.omni.pickDir()) !== null) {
      await refreshOmni()
    }
  }

  const loadOmni = async (): Promise<void> => {
    setOmniBusy(true)
    setOmniNote(null)
    const reason = await window.yoai.omni.load()
    setOmniBusy(false)
    await refreshOmni()
    setOmniNote(
      reason.length === 0
        ? { text: S.settingsOmniReady, tone: 'success' }
        : { text: reason, tone: 'danger' }
    )
  }

  const unloadOmni = async (): Promise<void> => {
    await window.yoai.omni.unload()
    await refreshOmni()
  }

  const setReader = async (patch: {
    enabled?: boolean
    minChars?: number
    temperature?: number
    style?: '' | 'calm' | 'caring' | 'cheerful' | 'sorry'
  }): Promise<void> => {
    setReaderStatus(await window.yoai.reader.set(patch))
  }

  /** Use a bundled voice by name, or pick a .wav or .spk as the reference voice (null). */
  const chooseReference = async (bundled: string | null): Promise<void> => {
    setReaderNote(null)
    setRefBusy(true)
    const why =
      bundled === null
        ? await window.yoai.reader.pickReference()
        : await window.yoai.reader.useBundled(bundled)
    setRefBusy(false)
    setReaderStatus(await window.yoai.reader.status())
    if (why !== null && why.length > 0) {
      setReaderNote({ text: why, tone: 'danger' })
    }
  }

  const setOmni = async (patch: { ditSteps?: number; enabled?: boolean }): Promise<void> => {
    setOmniStatus(await window.yoai.omni.set(patch))
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
      const result = await window.yoai.keys.save(vendorId, key)
      if (!result.ok) {
        setNote({ text: result.reason ?? '这个 key 没能保存', tone: 'danger' })
        return
      }
    }

    await window.yoai.models.add({
      id: modelId(vendorId, name),
      vendorId,
      baseUrl: url,
      model: name
    })
    setApiKey('')
    setNote({ text: S.settingsSaved, tone: 'success' })
    setAddOpen(false)
    await refreshKeys()
    await onModelsChanged()
  }

  const closeAdd = (): void => {
    setAddOpen(false)
    setApiKey('')
    setNote(null)
  }

  const addLocal = async (): Promise<void> => {
    const picked = await window.yoai.models.pickLocal()
    if (picked === null) {
      return
    }
    await window.yoai.models.add({
      id: modelId(LOCAL_VENDOR_ID, picked.name),
      vendorId: LOCAL_VENDOR_ID,
      baseUrl: '',
      model: picked.name,
      kind: KIND_LOCAL,
      modelPath: picked.path
    })
    await onModelsChanged()
  }

  const drop = async (cfg: ModelConfig): Promise<void> => {
    await window.yoai.models.remove(cfg.id)
    await onModelsChanged()
  }

  const locals = models.filter(isLocal)
  const remotes = models.filter((m) => !isLocal(m))

  return (
    <div className="settings">
      {/* Outside the scrolling part, so the way back stays in view however far
          down the page is. */}
      <div className="topbar">
        <h2 style={{ margin: 0, fontSize: '1.0625rem' }}>{S.settingsTitle}</h2>
        <div className="topbar-spacer" />
        {speaking && (
          <button className="btn" onClick={onStopSpeech}>
            停止播放
          </button>
        )}
        <button className="btn" onClick={onClose}>
          返回对话
        </button>
      </div>
      <div className="settings-scroll">
        <div className="settings-inner">

          {/*------------------------------------------------------------------*/}
          <section className="section">
            <h3>{S.settingsModels}</h3>
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
                      {isLocal(m) && <div className="path">{m.modelPath}</div>}
                    </span>
                    {/* No button: the active model is preloaded on its own, at
                        startup and when it is switched to. What is left to show
                        is how that went, a failure above all. */}
                    {isLocal(m) && loadStates[m.id] !== undefined && (
                      <span
                        className="field-note"
                        data-tone={
                          loadStates[m.id].state === 'ready'
                            ? 'success'
                            : loadStates[m.id].state === 'failed'
                              ? 'danger'
                              : undefined
                        }
                      >
                        {loadStates[m.id].state === 'ready'
                          ? S.settingsLocalReady
                          : loadStates[m.id].state === 'failed'
                            ? loadStates[m.id].reason
                            : S.settingsLocalLoading}
                      </span>
                    )}
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
            {/* Here rather than above the list: a key is what adding a remote
                model asks for, and a local model has none. */}
            <p>{S.settingsIntro}</p>
            {!addOpen ? (
              <div className="row" style={{ padding: 0 }}>
                <span className="row-label">
                  {note !== null && (
                    <span className="field-note" data-tone={note.tone}>
                      {note.text}
                    </span>
                  )}
                </span>
                <button className="btn" onClick={() => setAddOpen(true)}>
                  {S.settingsAddOpen}
                </button>
              </div>
            ) : (
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
                      onClick={() => void window.yoai.openExternal(vendor.registerUrl)}
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
                  <button className="btn" onClick={closeAdd}>
                    {S.settingsCancel}
                  </button>
                  <button className="btn btn-primary" onClick={() => void addRemote()}>
                    {S.settingsSave}
                  </button>
                </div>
              </div>
            )}
          </section>

          {/*------------------------------------------------------------------*/}
          <section className="section">
            <h3>{S.settingsLocal}</h3>
            <p>{S.settingsLocalIntro}</p>
            {/* Only the way to add one. Those already added are listed, with
                their paths, under the configured models above. */}
            {status.localAvailable ? (
              <>
                {/* What a new user with no on-device model sees: one to
                    download, the way the speech models are offered below. */}
                {offer !== null && (locals.length === 0 || llmDownload?.running === true) && (
                  <div className="card" style={{ marginBottom: 12 }}>
                    {llmDownload?.running === true ? (
                      <DownloadProgress
                        d={llmDownload}
                        onCancel={() => void window.yoai.models.cancelDownload()}
                      />
                    ) : (
                      <div className="field">
                        <label>{S.settingsLocalOffer}</label>
                        <div className="path">
                          {offer.name} · {formatBytes(offer.size)}
                        </div>
                        <div style={{ display: 'flex', gap: 8, marginTop: 4 }}>
                          <button
                            className="btn btn-primary"
                            onClick={() => void startLlmDownload()}
                          >
                            {S.settingsTtsDownload}（{formatBytes(offer.size)}）
                          </button>
                        </div>
                        <span className="field-note">{S.settingsLocalOfferNote}</span>
                        <DownloadFailure d={llmDownload} />
                      </div>
                    )}
                  </div>
                )}
                <div className="row" style={{ padding: 0 }}>
                  <span className="row-label" />
                  <button className="btn" onClick={() => void addLocal()}>
                    {S.settingsLocalAdd}
                  </button>
                </div>
              </>
            ) : (
              <p className="field-note" data-tone="danger">
                这个版本没有编译本机推理引擎。用 <code>python examples/desktop/build.py llama</code>
                重新构建 addon 即可打开这条车道。
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
                  key={promptRev}
                  className="control"
                  rows={4}
                  defaultValue={prompt.systemPrompt}
                  placeholder={S.settingsPromptHint}
                  onBlur={(e) => void savePrompt(e.target.value)}
                />
              )}
            </div>
            {prompt !== null && (
              <div className="row" style={{ padding: 0, marginTop: 8, gap: 8 }}>
                <span className="field-note">
                  {prompt.custom ? S.settingsPromptCustom : S.settingsPromptDefault}
                </span>
                {prompt.custom && (
                  <button className="btn" onClick={() => void resetPrompt()}>
                    {S.settingsPromptReset}
                  </button>
                )}
              </div>
            )}
          </section>

          {/*------------------------------------------------------------------*/}
          <section className="section">
            <h3>{S.settingsTts}</h3>
            <p>{S.settingsTtsIntro}</p>
            {readerStatus !== null && !readerStatus.available ? (
              <p className="field-note" data-tone="danger">
                {S.settingsTtsUnavailable}
              </p>
            ) : (
              <>
                <div className="field">
                  <label>{S.settingsTtsMode}</label>
                  <div className="row" style={{ gap: 16 }}>
                    {(['base', 'custom'] as const).map((mode) => (
                      <label key={mode} style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                        <input
                          type="radio"
                          name="voiceMode"
                          checked={(ttsStatus?.voiceMode ?? 'base') === mode}
                          onChange={() => void setVoiceMode(mode)}
                        />
                        {mode === 'base' ? S.settingsTtsModeBase : S.settingsTtsModeCustom}
                      </label>
                    ))}
                  </div>
                  <span className="field-note">{S.settingsTtsModeNote}</span>
                </div>
                {/* What a new user sees first: the files this mode still needs,
                    and the two ways to get them. Whether the user already has
                    them is something only they know, so both are offered. */}
                {ttsStatus !== null &&
                  (ttsStatus.missing.length > 0 || download?.running === true) && (
                    <div className="card" style={{ marginBottom: 12 }}>
                      {download?.running === true ? (
                        <DownloadProgress
                          d={download}
                          onCancel={() => void window.yoai.tts.cancelDownload()}
                        />
                      ) : (
                        <div className="field">
                          <label>{S.settingsTtsSetup}</label>
                          {ttsStatus.missing.map((m) => (
                            <div className="path" key={m.slot}>
                              {m.name} · {formatBytes(m.size)}
                            </div>
                          ))}
                          <div style={{ display: 'flex', gap: 8, marginTop: 4 }}>
                            <button className="btn" onClick={() => void pickAnyTts()}>
                              {S.settingsTtsPickExisting}
                            </button>
                            <button className="btn btn-primary" onClick={() => void startDownload()}>
                              {S.settingsTtsDownload}（
                              {formatBytes(ttsStatus.missing.reduce((sum, m) => sum + m.size, 0))}）
                            </button>
                          </div>
                          <span className="field-note">{S.settingsTtsSetupNote}</span>
                          <DownloadFailure d={download} />
                        </div>
                      )}
                    </div>
                  )}
                <div className="card">
                  {[
                    ttsStatus?.voiceMode === 'custom'
                      ? {
                          which: 'customTalker' as const,
                          label: S.settingsTtsTalkerCustom,
                          path: ttsStatus?.customTalkerPath ?? ''
                        }
                      : {
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
                      <button
                        className="btn"
                        disabled={download?.running === true}
                        onClick={() => void pickTts(slot.which)}
                      >
                        {S.settingsTtsPick}
                      </button>
                    </div>
                  ))}
                  {ttsStatus?.voiceMode !== 'custom' && (
                  <div className="row">
                    <span className="row-label">
                      {S.settingsReaderRef}
                      {readerStatus !== null && readerStatus.reference.length > 0 && (
                        <div className="path">{readerStatus.reference}</div>
                      )}
                    </span>
                    {/* The voices under speaker/, so a new user has one to choose
                        without first finding or recording one. A picked file is
                        the extra first entry while it is in use. */}
                    {readerStatus !== null && readerStatus.bundledVoices.length > 0 && (
                      <select
                        className="control"
                        aria-label={S.settingsReaderRef}
                        disabled={refBusy}
                        value={readerStatus.reference.length > 0 ? '' : readerStatus.bundled}
                        onChange={(e) => {
                          if (e.target.value.length > 0) {
                            void chooseReference(e.target.value)
                          }
                        }}
                      >
                        {readerStatus.reference.length > 0 && (
                          <option value="">{S.settingsReaderRefOwn}</option>
                        )}
                        {readerStatus.bundledVoices.map((name) => (
                          <option key={name} value={name}>
                            {name}
                          </option>
                        ))}
                      </select>
                    )}
                    <button
                      className="btn"
                      disabled={refBusy || recording}
                      onClick={() => setRecording(true)}
                    >
                      {S.settingsReaderRefRecord}
                    </button>
                    <button
                      className="btn"
                      disabled={refBusy}
                      onClick={() => void chooseReference(null)}
                    >
                      {refBusy ? S.settingsReaderRefBusy : S.settingsReaderRefPick}
                    </button>
                  </div>
                  )}
                </div>
                {ttsStatus?.voiceMode === 'custom' && readerStatus !== null && (
                  <>
                    <div className="field" style={{ marginTop: 12 }}>
                      <label htmlFor="ttsSpeaker">{S.settingsTtsSpeaker}</label>
                      <select
                        id="ttsSpeaker"
                        className="control"
                        disabled={readerStatus.speakers.length === 0}
                        value={
                          readerStatus.speakers.includes(ttsStatus.speaker)
                            ? ttsStatus.speaker
                            : readerStatus.speakers.includes('serena')
                              ? 'serena'
                              : (readerStatus.speakers[0] ?? '')
                        }
                        onChange={(e) =>
                          void window.yoai.tts.setSpeaker(e.target.value).then(refreshTts)
                        }
                      >
                        {readerStatus.speakers.map((name) => (
                          <option key={name} value={name}>
                            {name}
                          </option>
                        ))}
                      </select>
                      <span className="field-note">{S.settingsTtsSpeakerNote}</span>
                    </div>
                    <div className="field" style={{ marginTop: 12 }}>
                      <label htmlFor="ttsStyle">{S.settingsTtsStyle}</label>
                      <select
                        id="ttsStyle"
                        className="control"
                        value={readerStatus.style}
                        onChange={(e) =>
                          void setReader({
                            style: e.target.value as '' | 'calm' | 'caring' | 'cheerful' | 'sorry'
                          })
                        }
                      >
                        <option value="">{S.settingsTtsStyleNone}</option>
                        <option value="calm">{S.settingsTtsStyleCalm}</option>
                        <option value="caring">{S.settingsTtsStyleCaring}</option>
                        <option value="cheerful">{S.settingsTtsStyleCheerful}</option>
                        <option value="sorry">{S.settingsTtsStyleSorry}</option>
                      </select>
                      <span className="field-note">{S.settingsTtsStyleNote}</span>
                    </div>
                  </>
                )}
                {recording && (
                  <VoiceRecorder
                    onUsed={() => {
                      setRecording(false)
                      void window.yoai.reader.status().then(setReaderStatus)
                    }}
                    onClose={() => setRecording(false)}
                  />
                )}
                {ttsStatus?.voiceMode !== 'custom' && (
                  <span className="field-note">{S.settingsReaderRefNote}</span>
                )}

                {/* No 载入 button: the voice loads by itself once both files are
                    picked, and this line says how that went. */}
                <div className="row" style={{ marginTop: 10, gap: 8 }}>
                  <span className="row-label">
                    {readerStatus === null ? null : readerStatus.loaded ? (
                      <span className="field-note" data-tone="success">
                        {S.settingsTtsLoaded} · {readerStatus.modelType}
                      </span>
                    ) : readerStatus.note.length > 0 ? (
                      <span className="field-note" data-tone="danger">
                        {readerStatus.note}
                      </span>
                    ) : ((ttsStatus?.voiceMode === 'custom'
                        ? ttsStatus?.customTalkerPath
                        : ttsStatus?.talkerPath) ?? '').length === 0 ||
                      (ttsStatus?.tokenizerPath ?? '').length === 0 ? (
                      <span className="field-note">{S.settingsTtsPickBoth}</span>
                    ) : (
                      <span className="field-note">{S.settingsTtsLoading}</span>
                    )}
                  </span>
                  {readerStatus?.loaded === true && (
                    <button className="btn" onClick={() => void testTts()}>
                      {S.settingsTtsTest}
                    </button>
                  )}
                </div>
                {/* Loaded but not anchored: a reference voice that failed. */}
                {readerStatus !== null && readerStatus.loaded && readerStatus.note.length > 0 && (
                  <span className="field-note" data-tone="danger">
                    {readerStatus.note}
                  </span>
                )}
                {readerNote !== null && (
                  <span className="field-note" data-tone={readerNote.tone}>
                    {readerNote.text}
                  </span>
                )}

                <div className="field" style={{ marginTop: 12 }}>
                  <label htmlFor="readerOn" style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
                    <input
                      id="readerOn"
                      type="checkbox"
                      checked={readerStatus?.enabled === true}
                      onChange={(e) => void setReader({ enabled: e.target.checked })}
                    />
                    {S.settingsReaderEnabled}
                  </label>
                  <span className="field-note">{S.settingsReaderEnabledNote}</span>
                </div>

                {readerStatus !== null && (
                  <div className="field" style={{ marginTop: 12 }}>
                    <label htmlFor="readerMin">{S.settingsReaderMin}</label>
                    <input
                      id="readerMin"
                      className="control"
                      type="number"
                      defaultValue={readerStatus.minChars}
                      onBlur={(e) =>
                        void setReader({ minChars: Number.parseInt(e.target.value, 10) })
                      }
                    />
                    <span className="field-note">{S.settingsReaderMinNote}</span>
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

                {readerStatus !== null && (
                  <div className="field" style={{ marginTop: 12 }}>
                    <label htmlFor="ttsTemperature">{S.settingsTtsTemperature}</label>
                    {/* CustomVoice always reads at the engine's 0.9 (reader.ts,
                        talkerTemperature). Keyed by mode because the field is
                        uncontrolled and has to show the other value on a switch. */}
                    <input
                      key={ttsStatus?.voiceMode === 'custom' ? 'custom' : 'base'}
                      id="ttsTemperature"
                      className="control"
                      type="number"
                      step="0.1"
                      min="0"
                      max="2"
                      disabled={ttsStatus?.voiceMode === 'custom'}
                      defaultValue={ttsStatus?.voiceMode === 'custom' ? 0.9 : readerStatus.temperature}
                      onBlur={(e) => void setReader({ temperature: Number.parseFloat(e.target.value) })}
                    />
                    <span className="field-note">
                      {ttsStatus?.voiceMode === 'custom'
                        ? S.settingsTtsTemperatureCustomNote
                        : S.settingsTtsTemperatureNote}
                    </span>
                  </div>
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
    </div>
  )
}
