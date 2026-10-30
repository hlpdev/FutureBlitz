// Copies what the extension ships from the rest of the repo: fb's lldb script, for when fb's own
// isn't found, the license, and the Marketplace's icon, the logo's 128 px tile. They're copied at
// build time, so they never drift.
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const repo = join(root, "..", "..");

mkdirSync(join(root, "lldb"), { recursive: true });
copyFileSync(join(repo, "tools", "lldb", "futureblitz.py"), join(root, "lldb", "futureblitz.py"));
copyFileSync(join(repo, "LICENSE"), join(root, "LICENSE"));
mkdirSync(join(root, "images"), { recursive: true });
copyFileSync(join(repo, "assets", "brand", "mark-tile-128.png"), join(root, "images", "icon.png"));
