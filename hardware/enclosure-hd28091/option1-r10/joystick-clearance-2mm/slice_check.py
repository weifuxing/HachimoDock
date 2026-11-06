"""Validate one full-front slice; retain only compact evidence."""
from pathlib import Path
import subprocess,json
ROOT=Path(__file__).resolve().parent
NAME='HD28091-R10-joystick-plus2mm'
OUT=ROOT/'slice-check';OUT.mkdir(exist_ok=True)
args=[r'C:\Program Files\Bambu Studio\bambu-studio.exe','--slice','0',
    '--load-settings',str(ROOT/'a1-machine.json')+';'+str(ROOT/'a1-process.json'),
    '--load-filaments',str(ROOT/'generic-abs.json'),'--orient','0','--arrange','1','--ensure-on-bed',
    '--export-3mf','slice-check.3mf','--outputdir',str(OUT),str(ROOT/(NAME+'-screen-up.stl'))]
r=subprocess.run(args,capture_output=True,text=True,encoding='utf8',errors='replace',timeout=180)
log=r.stdout+'\n'+r.stderr
(OUT/'slicer.log').write_text(log,encoding='utf8')
info={'return_code':r.returncode,'internal_error_log':'[error]' in log}
if (OUT/'result.json').exists():
    d=json.loads((OUT/'result.json').read_text())
    if d.get('sliced_plates'):
        p=d['sliced_plates'][0]
        info.update({'estimated_minutes':round(p['total_predication']/60),
                     'estimated_grams':p['filaments'][0]['total_used_g'],'model_count':len(p['objects'])})
p=ROOT/'revision-report.json';d=json.loads(p.read_text());d['slice_check']=info
p.write_text(json.dumps(d,indent=2),encoding='utf8')
assert r.returncode==0 and not info['internal_error_log'],info
for file in OUT.iterdir():
    if file.suffix in ['.gcode','.3mf']: file.unlink()
print(json.dumps(info,indent=2),flush=True)
