import { hapTasks } from '@ohos/hvigor-ohos-plugin';
import * as fs from 'fs';
import * as path from 'path';

/**
 * The voices that come with the app: every .spk under the repository's
 * speaker/<name>/, copied into resources/rawfile/speaker/<name>/ each time the
 * build is configured, so a HAP built from DevEco and one built by build.py
 * carry the same files. speaker/ is the one copy kept in git; the copy here is
 * ignored. Only the .spk files -- a voice's demo recording is not something the
 * app plays.
 */
function copySpeakers(): void {
  const from: string = path.resolve(__dirname, '..', '..', '..', 'speaker');
  const to: string = path.resolve(__dirname, 'src', 'main', 'resources', 'rawfile', 'speaker');
  if (!fs.existsSync(from)) {
    return;
  }
  for (const name of fs.readdirSync(from)) {
    const dir: string = path.join(from, name);
    if (!fs.statSync(dir).isDirectory()) {
      continue;
    }
    for (const file of fs.readdirSync(dir)) {
      if (!file.endsWith('.spk')) {
        continue;
      }
      const src: string = path.join(dir, file);
      const dest: string = path.join(to, name, file);
      if (fs.existsSync(dest) && fs.statSync(dest).size === fs.statSync(src).size) {
        continue;
      }
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      fs.copyFileSync(src, dest);
    }
  }
}

copySpeakers();

export default {
    system: hapTasks,  /* Built-in plugin of Hvigor. It cannot be modified. */
    plugins:[]         /* Custom plugin to extend the functionality of Hvigor. */
}
