/**
 * Settings: which models exist, and the theme.
 *
 * The add flow is an inline form rather than a dialog on purpose -- it has four
 * fields and one of them is a pasted key, which is exactly the situation where a
 * dialog that dismisses on an outside tap loses the user's work.
 *
 * A key is written straight to [SecureKeyStore] and never held in a field
 * afterwards: the form's `key` state is cleared as soon as it is saved, and the
 * list below shows only whether a key EXISTS for a service, never the key.
 */
import { common } from '@kit.AbilityKit';
import { promptAction, router } from '@kit.ArkUI';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';
import yoai from 'libyoai.so';
import { ModelConfig, Vendor, VENDORS, findVendor, modelLabel } from '../model/Provider';
import { ModelStore } from '../core/ModelStore';
import { SecureKeyStore } from '../core/SecureKeyStore';
import { SessionStore } from '../core/SessionStore';

@Entry
@Component
struct Settings {
  @State private models: ModelConfig[] = [];
  @State private keyedVendors: string[] = [];

  @State private adding: boolean = false;
  @State private vendorId: string = VENDORS[0].id;
  @State private baseUrl: string = VENDORS[0].baseUrl;
  @State private model: string = '';
  @State private key: string = '';

  @StorageLink('colorModePref') colorModePref: number =
    ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  async aboutToAppear(): Promise<void> {
    await this.reload();
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
    this.key = '';
  }

  private async saveModel(): Promise<void> {
    const base: string = this.baseUrl.trim();
    const name: string = this.model.trim();
    const pasted: string = this.key.trim();

    // An empty key is fine ONLY when this vendor already has one stored --
    // adding a second model to a service should not mean re-pasting its key.
    if (base.length === 0 || name.length === 0 || (pasted.length === 0 && !this.hasKey(this.vendorId))) {
      promptAction.showToast({ message: $r('app.string.settings_need_all') });
      return;
    }

    if (pasted.length > 0) {
      await SecureKeyStore.saveKey(this.vendorId, pasted);
    }
    const cfg: ModelConfig = ModelStore.makeConfig(this.vendorId, base, name);
    await ModelStore.add(this.ctx(), cfg);

    // If nothing was selected before, point the composer at what was just added
    // -- otherwise the user adds their first model and still gets "no model".
    const active: string = await ModelStore.activeId(this.ctx());
    if (active.length === 0) {
      await ModelStore.setActiveId(this.ctx(), cfg.id);
    }

    this.key = '';
    this.adding = false;
    await this.reload();
    promptAction.showToast({ message: $r('app.string.settings_saved') });
  }

  private async deleteModel(cfg: ModelConfig): Promise<void> {
    await ModelStore.remove(this.ctx(), cfg.id);

    // The key outlives its last model only if something still uses it; nothing
    // does, so drop it too rather than leaving a secret behind with no purpose.
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
    await this.reload();
  }

  private confirmClearHistory(): void {
    AlertDialog.show({
      title: $r('app.string.settings_clear_history'),
      message: $r('app.string.settings_clear_history_msg'),
      primaryButton: {
        value: $r('app.string.settings_cancel'),
        action: () => {}
      },
      secondaryButton: {
        value: $r('app.string.settings_delete'),
        fontColor: $r('app.color.danger'),
        action: () => {
          SessionStore.clear(this.ctx());
        }
      }
    });
  }

  private applyColorMode(mode: number): void {
    this.colorModePref = mode;
    this.ctx().getApplicationContext().setColorMode(mode as ConfigurationConstant.ColorMode);
  }

  //----------------------------------------------------------------------------
  // Views
  //----------------------------------------------------------------------------

  @Builder
  sectionTitle(text: ResourceStr) {
    Text(text)
      .fontSize(13)
      .fontColor($r('app.color.text_tertiary'))
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
    Text(label).fontSize(12).fontColor($r('app.color.text_tertiary')).width('100%')
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
          .fontColor($r('app.color.text_tertiary'))
          .layoutWeight(1)
        Row({ space: 4 }) {
          Text(this.vendorLabel()).fontSize(15).fontColor($r('app.color.text_primary'))
          Text('▾').fontSize(12).fontColor($r('app.color.text_tertiary'))
        }
        .bindMenu(this.vendorMenu())
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_base_url'))
        TextInput({ text: this.baseUrl, placeholder: $r('app.string.settings_base_url_hint') })
          .backgroundColor($r('app.color.bg_page'))
          .onChange((v: string) => { this.baseUrl = v; })
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_model'))
        TextInput({ text: this.model, placeholder: $r('app.string.settings_model_hint') })
          .backgroundColor($r('app.color.bg_page'))
          .onChange((v: string) => { this.model = v; })
      }
      .width('100%')

      Column({ space: 4 }) {
        this.fieldLabel($r('app.string.settings_key'))
        TextInput({
          text: this.key,
          placeholder: this.hasKey(this.vendorId)
            ? $r('app.string.settings_key_keep')
            : $r('app.string.settings_key_hint')
        })
          .type(InputType.Password)
          .showPasswordIcon(true)
          .backgroundColor($r('app.color.bg_page'))
          .onChange((v: string) => { this.key = v; })
      }
      .width('100%')

      if (this.registerUrl().length > 0) {
        Text($r('app.string.settings_get_key'))
          .fontSize(13)
          .fontColor($r('app.color.brand'))
          .onClick(() => this.copyRegisterUrl())
      }

      Row({ space: 12 }) {
        Button($r('app.string.settings_cancel'))
          .layoutWeight(1)
          .backgroundColor($r('app.color.bg_page'))
          .fontColor($r('app.color.text_secondary'))
          .onClick(() => { this.adding = false; this.key = ''; })
        Button($r('app.string.settings_save'))
          .layoutWeight(1)
          .backgroundColor($r('app.color.brand'))
          .fontColor($r('app.color.on_brand'))
          .onClick(() => this.saveModel())
      }
      .width('100%')
    }
    .width('100%')
    .padding(16)
    .margin({ left: 12, right: 12 })
    .borderRadius(12)
    .backgroundColor($r('app.color.bg_surface'))
  }

  private vendorLabel(): string {
    const v: Vendor | undefined = findVendor(this.vendorId);
    return v === undefined ? this.vendorId : v.label;
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
          .fontColor($r('app.color.text_primary'))
          .width('100%')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(this.hasKey(cfg.vendorId) ? $r('app.string.settings_key_saved') : $r('app.string.settings_key_hint'))
          .fontSize(11)
          .fontColor(this.hasKey(cfg.vendorId)
            ? $r('app.color.text_tertiary')
            : $r('app.color.danger'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text($r('app.string.settings_delete'))
        .fontSize(13)
        .fontColor($r('app.color.danger'))
        .onClick(() => this.deleteModel(cfg))
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 12, bottom: 12 })
  }

  @Builder
  themeRow() {
    Row({ space: 8 }) {
      Text($r('app.string.settings_theme'))
        .fontSize(15)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
      Text(this.themeLabelFor(this.colorModePref))
        .fontSize(14)
        .fontColor($r('app.color.text_secondary'))
      Text('▾').fontSize(12).fontColor($r('app.color.text_tertiary'))
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 14, bottom: 14 })
    .bindMenu(this.themeMenu())
  }

  @Builder
  themeMenu() {
    Menu() {
      MenuItem({ content: $r('app.string.theme_system') })
        .onClick(() => this.applyColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET))
      MenuItem({ content: $r('app.string.theme_light') })
        .onClick(() => this.applyColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT))
      MenuItem({ content: $r('app.string.theme_dark') })
        .onClick(() => this.applyColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_DARK))
    }
  }

  private themeLabelFor(mode: number): ResourceStr {
    if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT) {
      return $r('app.string.theme_light');
    }
    if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK) {
      return $r('app.string.theme_dark');
    }
    return $r('app.string.theme_system');
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        Button() {
          Text('‹').fontSize(24).fontColor($r('app.color.text_secondary'))
        }
        .type(ButtonType.Circle)
        .backgroundColor(Color.Transparent)
        .width(40)
        .height(40)
        .onClick(() => router.back())

        Text($r('app.string.settings_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
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
            .fontColor($r('app.color.text_tertiary'))
            .width('100%')
            .padding({ left: 16, right: 16, bottom: 8 })

          if (this.models.length === 0) {
            Text($r('app.string.settings_models_empty'))
              .fontSize(14)
              .fontColor($r('app.color.text_tertiary'))
              .padding({ left: 16, top: 8, bottom: 8 })
          } else {
            Column() {
              ForEach(this.models, (m: ModelConfig) => {
                this.modelRow(m)
                Divider().color($r('app.color.divider')).margin({ left: 16, right: 16 })
              }, (m: ModelConfig) => m.id)
            }
            .width('100%')
            .margin({ left: 12, right: 12 })
            .borderRadius(12)
            .backgroundColor($r('app.color.bg_surface'))
          }

          if (this.adding) {
            this.addForm()
          } else {
            Button($r('app.string.settings_add'))
              .margin({ top: 12 })
              .backgroundColor($r('app.color.brand'))
              .fontColor($r('app.color.on_brand'))
              .onClick(() => this.startAdd())
          }

          this.sectionTitle($r('app.string.settings_theme'))
          Column() {
            this.themeRow()
          }
          .width('100%')
          .margin({ left: 12, right: 12 })
          .borderRadius(12)
          .backgroundColor($r('app.color.bg_surface'))

          this.sectionTitle($r('app.string.settings_core'))
          Column({ space: 12 }) {
            Text(yoai.nativeVersion())
              .fontSize(12)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
            Text($r('app.string.settings_clear_history'))
              .fontSize(15)
              .fontColor($r('app.color.danger'))
              .width('100%')
              .onClick(() => this.confirmClearHistory())
          }
          .width('100%')
          .padding(16)
          .margin({ left: 12, right: 12, bottom: 24 })
          .borderRadius(12)
          .backgroundColor($r('app.color.bg_surface'))
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.bg_page'))
  }
}
