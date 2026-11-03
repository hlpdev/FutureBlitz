// Bundles the extension into dist/extension.js, the one script it ships, with vscode external and
// vscode-languageclient inside, and writes THIRD-PARTY-NOTICES, the license of each package in the
// bundle, which a build fails without. With --watch, it bundles each edit.
import { context } from "esbuild";
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

// The package an input of the bundle is in, like "vscode-jsonrpc" or "@isaacs/name", or undefined.
function packageOf(input) {
  const parts = input.split("node_modules/");
  if (parts.length < 2) {
    return undefined;
  }

  const names = parts.at(-1).split("/");
  return names[0].startsWith("@") ? `${names[0]}/${names[1]}` : names[0];
}

function writeNotices(metafile) {
  const packages = [...new Set(Object.keys(metafile.inputs).map(packageOf))].filter(Boolean).sort();
  let notices = "The FutureBlitz extension's dist/extension.js has these packages' code in it:\n";
  for (const name of packages) {
    const folder = join(root, "node_modules", name);
    const license = readdirSync(folder).find((file) => /^licen[sc]e/i.test(file));
    if (license === undefined) {
      throw new Error(`${name}, which the bundle has, has no license file`);
    }

    notices += `\n${"-".repeat(72)}\n${name}\n\n${readFileSync(join(folder, license), "utf8").trim()}\n`;
  }

  writeFileSync(join(root, "THIRD-PARTY-NOTICES"), notices);
}

const notices = {
  name: "notices",
  setup(build) {
    build.onEnd((result) => {
      if (result.errors.length === 0) {
        writeNotices(result.metafile);
      }
    });
  },
};

const bundling = await context({
  absWorkingDir: root,
  entryPoints: ["src/extension.ts"],
  bundle: true,
  platform: "node",
  format: "cjs",
  target: "node20",
  external: ["vscode"],
  minify: true,
  sourcemap: true,
  metafile: true,
  outfile: "dist/extension.js",
  plugins: [notices],
});

if (process.argv.includes("--watch")) {
  await bundling.watch();
} else {
  await bundling.rebuild();
  await bundling.dispose();
}
