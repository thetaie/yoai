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
import type { CoreStatus, InterfacePrefs } from '@shared/store'
import { FAMILY_LABEL, S } from '../strings'

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

  const vendor = findVendor(vendorId)
  const hasKey = keyed.includes(vendorId)

  const refreshKeys = async (): Promise<void> => {
    setKeyed(await window.bodyclock.models.vendorsWithKeys(VENDORS.map((v) => v.id)))
  }

  useEffect(() => {
    void refreshKeys()
  }, [])

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
    const reason = await window.bodyclock.models.preload(id)
    setLoadingLocal('')
    setNote(
      reason.length === 0
        ? { text: S.settingsLocalReady, tone: 'success' }
        : { text: reason, tone: 'danger' }
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
                  {isLocal(m) && (
                    <button
                      className="btn"
                      disabled={loadingLocal.length > 0}
                      onClick={() => void preload(m.id)}
                    >
                      {loadingLocal === m.id ? S.settingsLocalLoading : S.settingsLocalPreload}
                    </button>
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
