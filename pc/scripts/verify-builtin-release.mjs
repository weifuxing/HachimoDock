// Compare distributable resources against reviewed sources; do not touch user data.
import {readFileSync,readdirSync,writeFileSync,existsSync,lstatSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {join,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const pc=fileURLToPath(new URL('../',import.meta.url));
const args=process.argv.slice(2);
const option=name=>args[args.indexOf(name)+1];
assert.ok(args.includes('--resources')&&args.includes('--output'),'supply --resources and --output');
const resources=resolve(option('--resources'));
const config=JSON.parse(readFileSync(join(pc,'../firmware/factory-config.json')));
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
function files(dir,prefix='') {
  return readdirSync(dir,{withFileTypes:true}).flatMap(e=>{
    assert.ok(!e.isSymbolicLink(),`unexpected resource symlink: ${prefix}${e.name}`);
    return e.isDirectory()?files(join(dir,e.name),`${prefix}${e.name}/`):[prefix+e.name];
  }).sort();
}
function exactTree(source,dest) {
  assert.ok(existsSync(dest),`missing ${dest}`);
  const names=files(source);assert.deepEqual(files(dest),names,'resource file list differs');
  for(const name of names) assert.equal(sha(readFileSync(join(dest,name))),sha(readFileSync(join(source,name))),`resource differs: ${name}`);
  return names.length;
}
const report={version:JSON.parse(readFileSync(join(pc,'package.json'))).version,components:[],petuiFiles:0,firmware:[],privateFiles:[]};
const desktopIds=config.components.ids.filter(id=>!config.components.firmwareOnlyIds?.includes(id));
const packageRoot=join(resources,'builtin-clawpkgs');
const packageEntries=readdirSync(packageRoot,{withFileTypes:true});
assert.ok(packageEntries.every(entry=>entry.isDirectory()&&!entry.isSymbolicLink()),'unexpected file or symlink in bundled component root');
assert.deepEqual(packageEntries.map(entry=>entry.name).sort(),[...desktopIds].sort(),'bundled component list differs from the factory catalog');
for(const id of desktopIds) {
  const src=join(pc,'builtin-clawpkgs',id),dst=join(resources,'builtin-clawpkgs',id);
  for(const [location,root] of [['source',src],['installer',dst]]) {
    for(const dir of ['runtime','assets']) {
      const directory=join(root,dir);
      assert.ok(lstatSync(directory,{throwIfNoEntry:false})?.isDirectory(),`missing ${location} component directory: ${id}/${dir}/`);
      assert.ok(files(directory).length,`empty ${location} component directory: ${id}/${dir}/`);
    }
  }
  report.components.push({id,version:JSON.parse(readFileSync(join(src,'component.json'))).version,files:exactTree(src,dst)});
}
assert.equal(report.components.length,desktopIds.length);
assert.deepEqual(report.components.slice(-3).map(x=>x.id),['decision-capsule','answer-book','hope-train']);
report.petuiFiles=exactTree(join(pc,'skills/petui'),join(resources,'skills/petui'));
for(const name of ['firmware.bin','firmware-v3.bin','firmware-pico-v1.bin','firmware-pico-v3.bin']) {
  const bytes=readFileSync(join(resources,'firmware/esp32-p4',name));
  assert.equal(sha(bytes),sha(readFileSync(join(pc,'src-tauri/firmware/esp32-p4',name))));
  assert.ok(bytes.includes(Buffer.from('0.7.103-p4')));
  report.firmware.push({file:name,bytes:bytes.length,sha256:sha(bytes)});
}
for(const name of files(resources)) {
  if(/(^|\/)(\.env($|\.)|id_rsa|id_ed25519|\.internal-build-secrets\.json|__pycache__|\.DS_Store)(\/|$)|\.(p12|pfx|key|pyc)$/i.test(name)) report.privateFiles.push(name);
}
assert.deepEqual(report.privateFiles,[],'unwanted private/build files in package');
report.status='passed';
writeFileSync(resolve(option('--output')),JSON.stringify(report,null,2)+'\n');
console.log(`Verified ${report.components.length} complete components, ${report.petuiFiles} PetUI files and four firmware images.`);
