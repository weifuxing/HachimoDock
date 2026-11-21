/**
 * [Input] A locally bundled macOS Hachimodock.app, optionally supplied as a positional argument, plus optional --verify-only.
 * [Output] A fixed Developer ID signed app with certificate-bound default DR and verified receipts.
 * [Pos] Local macOS signing only; no notarization, timestamp service or ad-hoc fallback.
 * [Sync] If this file changes, update `scripts/.folder.md`.
 */

import { existsSync, readFileSync, writeFileSync, mkdtempSync, rmSync } from "node:fs";
import { dirname, resolve, join } from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import { verifyVoiceBundle } from "./voice-runtime-resources.mjs";
import {desktopBuildPaths,macAppIdentity,verifyBuildReceipt,readBuildReceipt,receiptArgument} from './desktop-build-artifact.mjs';

if (process.platform !== "darwin") {
  console.error("sign-macos-local-app only supports macOS");
  process.exit(1);
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDir, "../..");
const args = process.argv.slice(2);
const verifyOnly = args.includes("--verify-only");
const receiptPath=receiptArgument(args);
const outputIndex=args.indexOf('--output-receipt');
const outputReceipt=outputIndex<0?null:args[outputIndex+1];
if(outputIndex>=0&&(!outputReceipt||outputReceipt.startsWith('--')||verifyOnly||existsSync(resolve(outputReceipt))))throw Error('invalid output receipt');
const positional=args.filter((arg,i)=>!arg.startsWith('--')&&!['--receipt','--output-receipt'].includes(args[i-1]));
if(positional.length>1||args.some(arg=>arg.startsWith('--')&&!['--verify-only','--receipt','--relocated','--output-receipt'].includes(arg)))throw Error('invalid signer arguments');
const paths=desktopBuildPaths({repositoryRoot,target:'macos'});
const {version,productName}=JSON.parse(readFileSync(resolve(repositoryRoot,'pc/src-tauri/tauri.conf.json'),'utf8'));
const appPath=resolve(positional[0]||resolve(paths.bundleRoot,'macos',`${productName}.app`));
const assertIdentity=()=>{
  const identity=macAppIdentity(appPath,version);
  if(receiptPath)verifyBuildReceipt(identity,readBuildReceipt(receiptPath),{relocated:args.includes('--relocated')});
  return identity;
};
assertIdentity(); // reject wrong version/hash before modifying any signature

if (!existsSync(appPath)) {
  console.error(`Pet Manager app bundle not found: ${appPath}`);
  process.exit(1);
}

// Verify install-relative service code/dependencies before either sign or verify.
console.log(JSON.stringify({voiceRuntime:verifyVoiceBundle(resolve(appPath,"Contents/Resources"),{target:"macos",arch:process.arch})}));

// Use the very same resolver as desktop-local.py and release-local.py.
// -B prevents writing Python bytecode during signing or verification.
const resolvedConfig = spawnSync('python3', ['-B', resolve(repositoryRoot, 'scripts/build/local_config.py'), '--source', repositoryRoot],
  {encoding:'utf8', timeout:10000, env:process.env});
if (resolvedConfig.error || resolvedConfig.status !== 0) throw Error('Cannot resolve local signing workspace/config');
const {configPath} = JSON.parse(resolvedConfig.stdout);
const fixedIdentity = JSON.parse(readFileSync(configPath, 'utf8')).macos?.signingIdentity;
const sha1 = value => typeof value === 'string' && /^[A-Fa-f0-9]{40}$/.test(value);
if (!sha1(fixedIdentity)) throw Error('macos.signingIdentity must be a fixed Developer ID certificate SHA1');
const identity = fixedIdentity.toUpperCase();
if (process.env.HACHIMI_MAC_SIGNING_IDENTITY !== undefined
    && (!sha1(process.env.HACHIMI_MAC_SIGNING_IDENTITY)
        || process.env.HACHIMI_MAC_SIGNING_IDENTITY.toUpperCase() !== identity)) {
  throw Error('Signing identity override differs from fixed local certificate');
}
const command = (tool, argv) => {
  const result = spawnSync(tool, argv, {encoding:'utf8', timeout:120000});
  if (result.error || result.status !== 0) throw Error(`macOS signing command failed: ${tool}`);
  return `${result.stdout || ''}\n${result.stderr || ''}`;
};
const identities = command('/usr/bin/security', ['find-identity', '-v', '-p', 'codesigning']);
const matches = [...identities.matchAll(/\b([A-Fa-f0-9]{40})\s+"(Developer ID Application: [^"\r\n]+)"/g)]
  .filter(match => match[1].toUpperCase() === identity);
if (matches.length !== 1) throw Error('Fixed Developer ID Application certificate is not uniquely valid in local keychains');
const bundleId = 'com.petmanager.desktop';
const plistId = command('/usr/bin/plutil', ['-extract', 'CFBundleIdentifier', 'raw', '-o', '-', join(appPath,'Contents/Info.plist')]).trim();
if (plistId !== bundleId) throw Error('App BundleID does not match expected desktop identity');
if (!verifyOnly) {
  // Let codesign derive its normal certificate-bound DR. Never supply -r/--requirements.
  command('/usr/bin/codesign', ['--force', '--options', 'runtime', '--sign', identity, '--identifier', bundleId, '--timestamp=none', appPath]);
}
// Independently require the Apple Developer ID leaf extension AND exact leaf
// fingerprint. A valid ad-hoc signature, another cert or a spoofed BundleID fails.
const requirement = `=identifier "${bundleId}" and anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf = H"${identity}"`;
command('/usr/bin/codesign', ['--verify', '--deep', '--strict', '--verbose=2', '-R', requirement, appPath]);
const inspection = command('/usr/bin/codesign', ['--display', '--verbose=4', '--requirements', '-', appPath]);
if (!inspection.split(/\r?\n/).includes(`Identifier=${bundleId}`)
    || /Signature=adhoc/.test(inspection)
    || !/designated =>[^\n]*anchor apple generic/.test(inspection)
    || !/designated =>[^\n]*certificate leaf/.test(inspection)) {
  throw Error('Missing certificate-bound Developer ID designated requirement or mismatched identifier');
}
const certificateDir = mkdtempSync(join(tmpdir(), 'hachimodock-signature-'));
try {
  const prefix = join(certificateDir, 'certificate');
  command('/usr/bin/codesign', ['--display', `--extract-certificates=${prefix}`, appPath]);
  const leaf = createHash('sha1').update(readFileSync(`${prefix}0`)).digest('hex').toUpperCase();
  if (leaf !== identity) throw Error('Signed leaf certificate differs from fixed local certificate');
} finally {
  rmSync(certificateDir, {recursive:true, force:true});
}

// codesign is expected to change the embedded Mach-O signature. The incoming
// receipt was checked before signing; retain it and emit a fresh signed identity
// for subsequent copy/DMG verification, never overwrite the build receipt.
const signedIdentity=macAppIdentity(appPath,version);
if(verifyOnly)assertIdentity();
if(outputReceipt)writeFileSync(resolve(outputReceipt),JSON.stringify(signedIdentity,null,2)+'\n',{flag:'wx'});

console.log(`${verifyOnly ? "verified" : "applied"} fixed Developer ID macOS signature: ${appPath}`);
