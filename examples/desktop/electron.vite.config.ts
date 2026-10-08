import { resolve } from 'node:path'
import react from '@vitejs/plugin-react'
import { defineConfig, externalizeDepsPlugin } from 'electron-vite'

/**
 * Three builds, because Electron is three programs.
 *
 * electron-vite's defaults already point at src/main, src/preload and
 * src/renderer, so most of what is here is the one alias the three share --
 * `@shared`, which is how the renderer reaches src/shared without reaching
 * src/main along with it.
 *
 * The addon is NOT built here. It is C++ with a CMake build of its own
 * (native/), driven by build.py, and bundling a .node is not
 * something a JavaScript bundler should be asked to have an opinion about.
 */
const shared = resolve(__dirname, 'src/shared')

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()],
    resolve: {
      alias: { '@shared': shared }
    }
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    resolve: {
      alias: { '@shared': shared }
    }
  },
  renderer: {
    plugins: [react()],
    resolve: {
      alias: {
        '@shared': shared,
        '@renderer': resolve(__dirname, 'src/renderer/src')
      }
    }
  }
})
