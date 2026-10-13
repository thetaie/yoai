/**
 * The models this app can talk to: what exists, and how to add one.
 *
 * Two ways to add, because they have nothing in common at this level: a remote
 * model is a URL plus a pasted key, an on-device one is a file the user points
 * at -- or, with no on-device model yet, the one this app offers to download.
 * They converge immediately -- both end up as a ModelConfig in [ModelStore],
 * listed in ONE list here and picked from one menu in the composer -- which is
 * why remote and on-device share this page instead of getting one each.
 * Splitting them would mean two half-lists of the same thing.
 *
 * The remote add flow is an inline form rather than a dialog on purpose: it has
 * four fields and one of them is a pasted key, which is exactly the situation
 * where a dialog that dismisses on an outside tap loses the user's work.
 *
 * A key is written straight to [SecureKeyStore] and never held in a field
 * afterwards: the form's `pastedKey` state is cleared as soon as it is saved,
 * and the list below shows only whether a key EXISTS for a service, never the
 * key itself.
 *
 * That state is `pastedKey` rather than the obvious `key` because every
 * CustomComponent already inherits a `key(value: string)` attribute method, and
 * a member of the same name is rejected as an incompatible override.
 */
import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import { ModelConfig, Vendor, VENDORS, findVendor, isLocal, modelLabel } from '../model/Provider';
import { DownloadState, formatBytes } from '../core/Download';
import { ICON_INLINE, ICON_TOOLBAR, icon } from '../core/Icons';
import { LocalModelStore, PickedModel } from '../core/LocalModelStore';
import { ModelDownloads } from '../core/ModelDownloads';
import { LLM_OFFER } from '../core/ModelFiles';
import { LOAD_REV, ModelLoadState, ModelStore } from '../core/ModelStore';
import { DEFAULT_FAMILY, Palette, resolvePalette } from '../core/Palettes';
import { SecureKeyStore } from '../core/SecureKeyStore';

@Entry
@Component
struct SettingsModels {
  @State private models: ModelConfig[] = [];

  /** How each on-device model's automatic preload went, by id. */
  @StorageProp(LOAD_REV) @Watch('readLoads') loadRev: number = 0;
  @State private loads: Map<string, ModelLoadState> = new Map<string, ModelLoadState>();

  /** The on-device model's download, polled while it runs. */
  @State private download: DownloadState = ModelDownloads.llm.status();
  private poller: number = -1;

  @State private keyedVendors: string[] = [];

  @State private adding: boolean = false;

  @State private vendorId: string = VENDORS[0].id;

  @State private baseUrl: string = VENDORS[0].baseUrl;

  @State private model: string = '';

  @State private pastedKey: string = '';

  /** See the same block in Index -- family is the choice, isDark the outcome. */
  @StorageLink('paletteFamily') @Watch('resolveTheme') paletteFamily: string = DEFAULT_FAMILY;

  @StorageProp('isDark') @Watch('resolveTheme') isDark: boolean = false;

  @State private theme: Palette = resolvePalette(DEFAULT_FAMILY, false);

  private resolveTheme(): void {
    this.theme = resolvePalette(this.paletteFamily, this.isDark);
  }

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private async reload(): Promise<void> {
    this.models = await ModelStore.list(this.ctx());
    const ids: string[] = [];
    for (const v of VENDORS) {
      ids.push(v.id);
    }
    const have: Set<string> = await ModelStore.vendorsWithKeys(ids);
    const keyed: string[] = [];
    have.forEach((id: string) => keyed.push(id));
    this.keyedVendors = keyed;
  }

  private hasKey(vendorId: string): boolean {
    return this.keyedVendors.includes(vendorId);
  }

  //----------------------------------------------------------------------------
  // Add flow
  //----------------------------------------------------------------------------

  private startAdd(): void {
    this.adding = true;
    this.pickVendor(VENDORS[0].id);
  }

  private pickVendor(id: string): void {
    this.vendorId = id;
    const v: Vendor | undefined = findVendor(id);
    // Prefill from the built-in record, and leave both fields editable: the
    // built-ins are a shortcut, not a constraint.
    this.baseUrl = v === undefined ? '' : v.baseUrl;
    this.model = (v === undefined || v.models.length === 0) ? '' : v.models[0];
    this.pastedKey = '';
  }

  private async saveModel(): Promise<void> {
    const base: string = this.baseUrl.trim();
    const name: string = this.model.trim();
    const pasted: string = this.pastedKey.trim();

    // An empty key is fine ONLY when this vendor already has one stored --
    // adding a second model to a service should not mean re-pasting its key.
    if (base.length === 0 || name.length === 0 || (pasted.length === 0 && !this.hasKey(this.vendorId))) {
      promptAction.showToast({ message: $r('app.string.settings_need_all') });
      return;
    }

    if (pasted.length > 0) {
      await SecureKeyStore.saveKey(this.vendorId, pasted);
    }
    // Selected by add() when nothing was -- otherwise the user adds their first
    // model and still gets "no model".
    const cfg: ModelConfig = ModelStore.makeConfig(this.vendorId, base, name);
    await ModelStore.add(this.ctx(), cfg);

    this.pastedKey = '';
    this.adding = false;
    await this.reload();
    promptAction.showToast({ message: $r('app.string.settings_saved') });
  }

  //----------------------------------------------------------------------------
  // On-device models
  //----------------------------------------------------------------------------

  /**
   * Pick a .gguf and add it as a model like any other -- selected when nothing
   * else is (ModelStore.add), and then read at once, as switching to it would.
   *
   * The weights are read now rather than on the first turn: seconds and several
   * GB off disk, and paying for it here, with the list showing it, is the whole
   * difference between a first reply that starts and one that appears to hang.
   * A load that fails leaves the entry in place, with the reason beside it: a
   * load that failed once (low memory, say) may well succeed later.
   */
  private async addLocalModel(): Promise<void> {
    const picked: PickedModel | undefined = await LocalModelStore.importModel(this.ctx());
    if (picked === undefined) {
      return;   // cancelled, or the file was not reachable (already logged)
    }
    await ModelStore.add(this.ctx(), picked.config);
    if ((await ModelStore.activeId(this.ctx())) === picked.config.id) {
      ModelStore.autoPreload(this.ctx());
    }
    await this.reload();
  }

  private readLoads(): void {
    this.loads = ModelStore.loadStates();
  }

  /** The preload line under a local model: '' when none was started for it. */
  private loadLine(cfg: ModelConfig): ResourceStr {
    const st: ModelLoadState | undefined = this.loads.get(cfg.id);
    if (st === undefined) {
      return '';
    }
    if (st.state === 'ready') {
      return $r('app.string.settings_local_ready');
    }
    if (st.state === 'failed') {
      return st.reason.length > 0 ? st.reason : $r('app.string.settings_local_failed');
    }
    return $r('app.string.settings_local_loading');
  }

  private loadFailed(cfg: ModelConfig): boolean {
    const st: ModelLoadState | undefined = this.loads.get(cfg.id);
    return st !== undefined && st.state === 'failed';
  }

  //----------------------------------------------------------------------------
  // The offered download
  //----------------------------------------------------------------------------

  private async startDownload(): Promise<void> {
    if (await ModelDownloads.downloadLlm(this.ctx())) {
      this.follow();
    }
  }

  /**
   * Poll the download while it runs: it runs outside this page, and may already
   * be under way when the page opens. Once over, the model it added is listed.
   */
  private follow(): void {
    if (this.poller !== -1) {
      return;
    }
    this.download = ModelDownloads.llm.status();
    this.poller = setInterval(() => {
      const d: DownloadState = ModelDownloads.llm.status();
      this.download = d;
      if (!d.running) {
        clearInterval(this.poller);
        this.poller = -1;
        this.reload();
      }
    }, 500);
  }

  private hasLocal(): boolean {
    for (const m of this.models) {
      if (isLocal(m)) {
        return true;
      }
    }
    return false;
  }

  private async deleteModel(cfg: ModelConfig): Promise<void> {
    await ModelStore.remove(this.ctx(), cfg.id);

    if (!isLocal(cfg)) {
      // The key outlives its last model only if something still uses it; nothing
      // does, so drop it too rather than leaving a secret behind with no purpose.
      // Local models have no key -- and no vendor whose other models could be
      // affected -- so this whole question does not arise for them.
      let stillUsed: boolean = false;
      const rest: ModelConfig[] = await ModelStore.list(this.ctx());
      for (const m of rest) {
        if (m.vendorId === cfg.vendorId) {
          stillUsed = true;
          break;
        }
      }
      if (!stillUsed) {
        await SecureKeyStore.deleteKey(cfg.vendorId);
      }
    }
    await this.reload();
  }

  @Builder
  sectionTitle(text: ResourceStr) {
    Text(text)
      .fontSize(13)
      .fontColor(this.theme.text_tertiary)
      .width('100%')
      .padding({ left: 16, right: 16, top: 18, bottom: 6 })
  }

  @Builder
  vendorMenu() {
    Menu() {
      ForEach(VENDORS, (v: Vendor) => {
        MenuItem({ content: v.label }).onClick(() => this.pickVendor(v.id))
      }, (v: Vendor) => v.id)
    }
  }

  @Builder
  fieldLabel(label: ResourceStr) {
    Text(label).fontSize(12).fontColor(this.theme.text_tertiary).width('100%')
  }

  /**
   * The three fields are written out rather than produced by one @Builder taking
   * an onChange callback: a builder parameterized by a closure is exactly the
   * shape ArkUI's builder parameter passing handles least predictably, and this
   * form has to work on the first run.
   */
  @Builder
  addForm() {
    Column({ space: 12 }) {
      Row() {
        Text($r('app.string.settings_vendor'))
          .fontSize(12)
          .fontColor(this.theme.text_tertiary)
          .layoutWeight(1)
        Row({ space: 4 }) {
          Text(this.vendorLabel()).fontSize(15).fontColor(this.theme.text_primary)
          icon('chevron-down', ICON_INLINE, this.theme.text_tertiary)
        }
        .bindMenu(this.vendorMenu())
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_base_url'))
        TextInput({ text: this.baseUrl, placeholder: $r('app.string.settings_base_url_hint') })
          .backgroundColor(this.theme.bg_page)
          .onChange((v: string) => { this.baseUrl = v; })
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_model'))
        TextInput({ text: this.model, placeholder: $r('app.string.settings_model_hint') })
          .backgroundColor(this.theme.bg_page)
          .onChange((v: string) => { this.model = v; })
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_key'))
        TextInput({
          text: this.pastedKey,
          placeholder: this.hasKey(this.vendorId)
            ? $r('app.string.settings_key_keep')
            : $r('app.string.settings_key_hint')
        })
          .type(InputType.Password)
          .showPasswordIcon(true)
          .backgroundColor(this.theme.bg_page)
          .onChange((v: string) => { this.pastedKey = v; })
      }
      .width('100%')

      if (this.registerUrl().length > 0) {
        Text($r('app.string.settings_get_key'))
          .fontSize(13)
          .fontColor(this.theme.brand)
          .onClick(() => this.copyRegisterUrl())
      }

      Row({ space: 12 }) {
        Button($r('app.string.settings_cancel'))
          .layoutWeight(1)
          .backgroundColor(this.theme.bg_page)
          .fontColor(this.theme.text_secondary)
          .onClick(() => { this.adding = false; this.pastedKey = ''; })
        Button($r('app.string.settings_save'))
          .layoutWeight(1)
          .backgroundColor(this.theme.brand)
          .fontColor(this.theme.on_brand)
          .onClick(() => this.saveModel())
      }
      .width('100%')
    }
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  private vendorLabel(): string {
    const v: Vendor | undefined = findVendor(this.vendorId);
    return v === undefined ? this.vendorId : v.label;
  }

  /**
   * The small line under a model's name in the list.
   *
   * For a remote model that is its key state. For an on-device one it is the
   * file's path, shown in full rather than summarized as "on device": when a
   * model stops working the path IS the diagnosis -- it says which file the app
   * is holding and where to go looking for it. A local model also has no key, so
   * the key state would be a permanent red "no key saved" on a row that works.
   *
   * Declared `ResourceStr` and returned from a method rather than written inline
   * in the builder: one branch is a plain string and the others are resources,
   * and ArkTS will not infer that union for you inside a Text().
   */
  private rowSubtitle(cfg: ModelConfig): ResourceStr {
    if (isLocal(cfg)) {
      const path: string | undefined = cfg.modelPath;
      return path === undefined ? '' : path;
    }
    return this.hasKey(cfg.vendorId) ? $r('app.string.settings_key_saved')
                                     : $r('app.string.settings_key_hint');
  }

  private registerUrl(): string {
    const v: Vendor | undefined = findVendor(this.vendorId);
    return v === undefined ? '' : v.registerUrl;
  }

  /**
   * Put the vendor's signup URL on the clipboard rather than opening a browser.
   * Opening one needs an ability-launch call whose failure modes (no browser,
   * blocked implicit want) are worse than a copied link the user can paste.
   */
  private copyRegisterUrl(): void {
    promptAction.showToast({ message: this.registerUrl() });
  }

  @Builder
  modelRow(cfg: ModelConfig) {
    Row() {
      Column({ space: 2 }) {
        Text(modelLabel(cfg))
          .fontSize(15)
          .fontColor(this.theme.text_primary)
          .width('100%')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(this.rowSubtitle(cfg))
          .fontSize(11)
          .fontColor(isLocal(cfg) || this.hasKey(cfg.vendorId)
            ? this.theme.text_tertiary
            : this.theme.danger)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        // No load button: the active model is read by itself, at startup and
        // when it is switched to. What is left to show is how that went.
        if (isLocal(cfg) && this.loads.has(cfg.id)) {
          Text(this.loadLine(cfg))
            .fontSize(11)
            .fontColor(this.loadFailed(cfg) ? this.theme.danger : this.theme.text_secondary)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text($r('app.string.settings_delete'))
        .fontSize(13)
        .fontColor(this.theme.danger)
        .onClick(() => this.deleteModel(cfg))
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 12, bottom: 12 })
  }

  async aboutToAppear(): Promise<void> {
    this.resolveTheme();   // @Watch does not fire on construction; see Index
    this.readLoads();
    await this.reload();
    if (ModelDownloads.llm.status().running) {
      this.follow();
    }
  }

  aboutToDisappear(): void {
    if (this.poller !== -1) {
      clearInterval(this.poller);
      this.poller = -1;
    }
  }

  /**
   * The offer a new user with no on-device model sees, or the download it
   * started. One model, named with its size, the way the desktop offers its own.
   */
  @Builder
  offerCard() {
    Column({ space: 8 }) {
      if (this.download.running) {
        Text(this.download.retry > 0
          ? $r('app.string.download_retrying', this.download.retryIn, this.download.retry,
               this.download.retries)
          : this.download.file.length === 0
            ? $r('app.string.download_connecting')
            : this.download.verifying
              ? $r('app.string.download_verifying', this.download.file)
              : $r('app.string.download_downloading', this.download.file))
          .fontSize(13)
          .fontColor(this.theme.text_primary)
          .width('100%')
        Progress({ value: this.download.received, total: Math.max(this.download.total, 1),
                   type: ProgressType.Linear })
          .width('100%')
          .color(this.theme.brand)
        Row({ space: 8 }) {
          Text(formatBytes(this.download.received) + ' / ' + formatBytes(this.download.total) +
               (this.download.host.length > 0 ? ' · ' + this.download.host : ''))
            .fontSize(11)
            .fontColor(this.theme.text_tertiary)
            .layoutWeight(1)
          Button($r('app.string.settings_cancel'))
            .height(30)
            .fontSize(12)
            .backgroundColor(this.theme.bg_page)
            .fontColor(this.theme.text_primary)
            .onClick(() => { ModelDownloads.llm.cancel(); })
        }
        .width('100%')
      } else {
        Text($r('app.string.settings_local_offer'))
          .fontSize(13)
          .fontColor(this.theme.text_primary)
          .width('100%')
        Text(LLM_OFFER.name + ' · ' + formatBytes(LLM_OFFER.size))
          .fontSize(12)
          .fontColor(this.theme.text_secondary)
          .width('100%')
        Button($r('app.string.download_button', formatBytes(LLM_OFFER.size)))
          .height(36)
          .fontSize(13)
          .backgroundColor(this.theme.brand)
          .fontColor(this.theme.on_brand)
          .onClick(() => { this.startDownload(); })
        Text(LocalModelStore.folderPickable()
          ? $r('app.string.settings_local_offer_note_folder')
          : $r('app.string.settings_local_offer_note_app'))
          .fontSize(11)
          .fontColor(this.theme.text_tertiary)
          .width('100%')
        if (this.download.error.length > 0) {
          Text(this.downloadError())
            .fontSize(11)
            .fontColor(this.download.error === 'canceled' ? this.theme.text_tertiary : this.theme.danger)
            .width('100%')
          if (this.download.detail.length > 0) {
            Text(this.download.detail)
              .fontSize(11)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
          }
        }
      }
    }
    .alignItems(HorizontalAlign.Start)
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12, bottom: 12 })
    .borderRadius(12)
    .backgroundColor(this.theme.bg_surface)
  }

  private downloadError(): ResourceStr {
    const e: string = this.download.error;
    if (e === 'space') {
      return $r('app.string.download_err_space');
    }
    if (e === 'network') {
      return $r('app.string.download_err_network');
    }
    if (e === 'checksum') {
      return $r('app.string.download_err_checksum');
    }
    if (e === 'disk') {
      return $r('app.string.download_err_disk');
    }
    return $r('app.string.download_err_canceled');
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Button() {
          icon('chevron-left', ICON_TOOLBAR, this.theme.text_secondary)
        }
        .type(ButtonType.Circle)
        .backgroundColor(Color.Transparent)
        .width(40)
        .height(40)
        .onClick(() => router.back())

        Text($r('app.string.settings_page_models'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor(this.theme.text_primary)
          .layoutWeight(1)
      }
      .width('100%')
      .height(56)
      .padding({ left: 8, right: 16 })

      Scroll() {
        Column() {
          this.sectionTitle($r('app.string.settings_models'))
          Text($r('app.string.settings_intro'))
            .fontSize(12)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
            .padding({ left: 16, right: 16, bottom: 8 })

          if (this.models.length === 0) {
            Text($r('app.string.settings_models_empty'))
              .fontSize(14)
              .fontColor(this.theme.text_tertiary)
              .padding({ left: 16, top: 8, bottom: 8 })
          } else {
            Column() {
              ForEach(this.models, (m: ModelConfig) => {
                this.modelRow(m)
                Divider().color(this.theme.divider).margin({ left: 16, right: 16 })
              }, (m: ModelConfig) => m.id)
            }
            .width('100%')
            .margin({ left: 12, right: 12 })
            .borderRadius(12)
            .backgroundColor(this.theme.bg_surface)
          }

          if (this.adding) {
            this.addForm()
          } else {
            Button($r('app.string.settings_add'))
              .margin({ top: 12 })
              .backgroundColor(this.theme.brand)
              .fontColor(this.theme.on_brand)
              .onClick(() => this.startAdd())
          }

          this.sectionTitle($r('app.string.settings_local'))
          Text($r('app.string.settings_local_intro'))
            .fontSize(12)
            .fontColor(this.theme.text_tertiary)
            .width('100%')
            .padding({ left: 16, right: 16, bottom: 8 })

          if (!LocalModelStore.available()) {
            // Said here rather than after a file has been picked: letting someone
            // hunt down a 3 GB model and only then be told the engine is missing
            // is the worse order to find out.
            Text($r('app.string.settings_local_unavailable'))
              .fontSize(13)
              .fontColor(this.theme.text_tertiary)
              .width('100%')
              .padding({ left: 16, right: 16, bottom: 8 })
          } else {
            // What a new user with no on-device model sees: one to download,
            // the way the speech models are offered on their page.
            if (!this.hasLocal() || this.download.running) {
              this.offerCard()
            }
            Button($r('app.string.settings_local_add'))
              .margin({ top: 4, bottom: 24 })
              .backgroundColor(this.theme.bg_surface)
              .fontColor(this.theme.text_primary)
              .onClick(() => this.addLocalModel())
          }
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor(this.theme.bg_page)
  }
}
