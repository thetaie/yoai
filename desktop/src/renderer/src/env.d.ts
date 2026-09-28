/// <reference types="vite/client" />

import type { YoaiApi } from '@shared/api'

declare global {
  interface Window {
    /**
     * Everything this page can do outside its own document, bound by
     * src/preload/index.ts. There is no second door: no require, no process,
     * no ipcRenderer.
     */
    yoai: YoaiApi
  }
}

export {}
