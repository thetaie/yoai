/**
 * API keys, on device only.
 *
 * The hard rule this file exists to hold: a key the user pastes lives in the
 * platform's encrypted asset store and nowhere else. It is never written to
 * preferences, never logged, never put in a crash report, and there is no YoAI
 * server for it to be sent to.
 *
 * Backed by ASSET Kit, one asset per vendor, ALIAS = "key.<vendorId>".
 */
import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';

const ALIAS_PREFIX: string = 'key.';

function aliasFor(vendorId: string): Uint8Array {
  return new util.TextEncoder().encodeInto(ALIAS_PREFIX + vendorId);
}

function toBytes(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function fromBytes(bytes: Uint8Array): string {
  return util.TextDecoder.create('utf-8').decodeToString(bytes);
}

export class SecureKeyStore {
  /** Store (or replace) the API key for a vendor. */
  static async saveKey(vendorId: string, key: string): Promise<void> {
    // Remove any existing asset first: add() rejects duplicates, so without
    // this a re-paste would fail rather than overwrite.
    await SecureKeyStore.deleteKey(vendorId);

    // No IS_PERSISTENT tag: that needs ohos.permission.STORE_PERSISTENT_DATA
    // and makes add() throw without it. Non-persistent is the right default
    // anyway -- the key is cleared when the app is uninstalled.
    const attr: asset.AssetMap = new Map();
    attr.set(asset.Tag.SECRET, toBytes(key));
    attr.set(asset.Tag.ALIAS, aliasFor(vendorId));
    attr.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    await asset.add(attr);
  }

  /** The stored key for a vendor, or '' when there is none. */
  static async getKey(vendorId: string): Promise<string> {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.ALIAS, aliasFor(vendorId));
    query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
    try {
      const res: Array<asset.AssetMap> = await asset.query(query);
      if (res.length === 0) {
        return '';
      }
      const secret = res[0].get(asset.Tag.SECRET) as Uint8Array;
      return fromBytes(secret);
    } catch (err) {
      const e = err as BusinessError;
      if (e.code === asset.ErrorCode.NOT_FOUND) {
        return '';
      }
      throw err as Error;
    }
  }

  static async hasKey(vendorId: string): Promise<boolean> {
    const key: string = await SecureKeyStore.getKey(vendorId);
    return key.length > 0;
  }

  /** Remove the key for a vendor; a no-op when there is none. */
  static async deleteKey(vendorId: string): Promise<void> {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.ALIAS, aliasFor(vendorId));
    try {
      await asset.remove(query);
    } catch (err) {
      const e = err as BusinessError;
      if (e.code === asset.ErrorCode.NOT_FOUND) {
        return;
      }
      throw err as Error;
    }
  }
}
