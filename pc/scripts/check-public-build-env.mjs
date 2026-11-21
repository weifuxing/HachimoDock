/**
 * [Input] Build environment, Vite dotenv inputs and configured resource trees.
 * [Output] Redacted public input gate; final artifact content scanning is separate.
 * [Pos] Public source build boundary. See ../.folder.md.
 */
import { existsSync, lstatSync, readFileSync, readdirSync, realpathSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
const forbiddenVariables = [
  'PET_MANAGER_INTERNAL_ASR_API_KEY', 'VITE_INTERNAL_CONTENT_API_KEY',
  'PET_MANAGER_INTERNAL_BUILD_AUTHORIZED', 'PET_MANAGER_BUNDLED_MQTT_URL',
  'PET_MANAGER_BUNDLED_MQTT_USERNAME', 'PET_MANAGER_BUNDLED_MQTT_PASSWORD',
  'PET_MANAGER_INTERNAL_LLM_API_KEY', 'PET_MANAGER_INTERNAL_TTS_API_KEY',
  'PET_MANAGER_INTERNAL_LLM_BASE_URL', 'PET_MANAGER_INTERNAL_LLM_MODEL',
  'PET_MANAGER_INTERNAL_CA_PEM', 'PET_MANAGER_INTERNAL_HTTPS_PROXY',
  'PET_MANAGER_INTERNAL_CA_FILE', 'PET_MANAGER_INTERNAL_CA_SHA256',
  'PET_MANAGER_INTERNAL_CREDENTIALS_FILE',
  'PET_MANAGER_BUNDLED_SERVICE_KEYS_FILE',
];
function forbidden(name) {
  return forbiddenVariables.includes(name) || (name.startsWith('VITE_')
    && /(?:^|_)(?:KEY|APIKEY|ACCESSKEY|SECRET|TOKEN|PASSWORD|CREDENTIALS?)(?:_|$)/i.test(name));
}
const pcRoot = fileURLToPath(new URL('..', import.meta.url));
const tauriRoot = join(pcRoot, 'src-tauri');
function checkEnvironment() {
  for (const name of Object.keys(process.env)) {
    if (forbidden(name) && process.env[name]?.trim()) {
      throw new Error(`Public build refuses credential injection through ${name}`);
    }
  }
  for (const name of ['.internal-build-secrets.json', 'src-tauri/bundle-secrets.env']) {
    if (existsSync(join(pcRoot, name))) throw new Error('Remove local credential build files before building public sources');
  }
  // Vite loads these AFTER this gate, including mode-specific files. Examine
  // assignment names without evaluating values or dotenv variable expansion.
  for (const name of readdirSync(pcRoot).filter(name => /^\.env(?:\.|$)/.test(name)
    && !name.endsWith('.example'))) {
    const input = readFileSync(join(pcRoot, name), 'utf8');
    for (const match of input.matchAll(/^\s*(?:export\s+)?([\w.-]+)\s*(?:=|:\s)/gm)) {
      if (forbidden(match[1])) throw new Error(`Public build refuses credential assignment through dotenv: ${match[1]}`);
    }
  }
}

const privateNames = new Set(['.npmrc', '.internal-build-secrets.json', 'bundle-secrets.env',
  'library.json', 'secure.json', 'conversations.json', 'toolchains.local.json', 'id_rsa', 'id_ed25519']);
const privateDirectories = new Set(['.git', '.ssh', '.aws', '.codex', '.claude',
  'test', 'tests', '__tests__', '__pycache__']);
const visited = new Set();
function checkResource(path, resourceRoot = false) {
  let stat;
  try { stat = lstatSync(path); } catch (error) {
    // This gate also runs before resource preparation in a clean checkout.
    if (error.code === 'ENOENT') return;
    throw error;
  }
  if (visited.has(path)) return;
  visited.add(path);
  const name = basename(path).toLowerCase();
  if (privateNames.has(name) || /^\.env(?:\.|$)/.test(name)
    || /\.(?:p12|pfx|p8|keychain(?:-db)?|log|jsonl|sqlite\d*(?:-wal|-shm)?|db|pyc|dmp)$/.test(name)
    || (stat.isDirectory() && privateDirectories.has(name))) {
    throw new Error('Public build refuses private or test files in resource inputs (values redacted)');
  }
  if (stat.isSymbolicLink()) {
    // Local bootstrap resources may be mounted as root symlinks. Inspect the
    // resolved tree, never skip it. Nested links need separate staging review.
    if (!resourceRoot) throw new Error('Public build refuses nested symlinks in resource inputs');
    checkResource(realpathSync(path));
    return;
  }
  if (stat.isDirectory()) {
    for (const child of readdirSync(path)) checkResource(join(path, child));
  } else if (!stat.isFile()) throw new Error('Public build refuses special files in resource inputs');
}

function checkResources() {
  // Vite copies public/ verbatim; Tauri embeds dist/. Check both as well as
  // the default resource maps. A separate bundle invocation still needs the
  // final artifact audit; this gate does not certify arbitrary custom overlays.
  for (const name of ['public', 'dist']) checkResource(join(pcRoot, name), true);
  const configs = ['tauri.conf.json', 'tauri.macos.conf.json', 'tauri.windows.conf.json'];
  if (process.env.PET_MANAGER_VOICE_SERVICE_SOURCE) configs.push('tauri.voice-legacy.conf.json');
  for (const name of configs) {
    const path = join(tauriRoot, name);
    if (!existsSync(path)) continue;
    let config;
    try { config = JSON.parse(readFileSync(path, 'utf8')); }
    catch { throw new Error('Public build resource configuration is invalid'); }
    const resources = config.bundle?.resources || {};
    for (const source of Array.isArray(resources) ? resources : Object.keys(resources)) {
      if (/[*?\[\]{}]/.test(source)) throw new Error('Public build resource glob needs explicit input review');
      checkResource(resolve(tauriRoot, source), true);
    }
  }
}
try {
  checkEnvironment();
  checkResources();
  console.log('Public build inputs verified; final artifact privacy audit still required.');
} catch (error) {
  // I/O exceptions can contain paths or arbitrary file content; never echo them.
  console.error(error.message.startsWith('Public build') || error.message.startsWith('Remove local credential')
    ? error.message : 'Public build input inspection failed (details redacted).');
  process.exitCode = 1;
}
