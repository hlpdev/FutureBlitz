// Copies what the extension ships from the rest of the repo: fb's lldb script, for when fb's own
// isn't found, the license, the Marketplace's icon, the logo's 128 px tile, and .fb files' icon,
// the logo on no background: as it is for dark themes, and with its light stroke in the brand's
// ink for light ones. They're copied at build time, so they never drift.
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const repo = join(root, "..", "..");

mkdirSync(join(root, "lldb"), { recursive: true });
copyFileSync(join(repo, "tools", "lldb", "futureblitz.py"), join(root, "lldb", "futureblitz.py"));
copyFileSync(join(repo, "LICENSE"), join(root, "LICENSE"));
mkdirSync(join(root, "images"), { recursive: true });
copyFileSync(join(repo, "assets", "brand", "mark-tile-128.png"), join(root, "images", "icon.png"));
const mark = readFileSync(join(repo, "assets", "brand", "mark.svg"), "utf8");
if (!mark.includes('fill="#F4F6FB"')) {
  throw new Error("assets/brand/mark.svg's light stroke isn't #F4F6FB, which the light theme's icon swaps");
}
writeFileSync(join(root, "images", "file-dark.svg"), mark);
writeFileSync(join(root, "images", "file-light.svg"), mark.replaceAll('fill="#F4F6FB"', 'fill="#08090D"'));
