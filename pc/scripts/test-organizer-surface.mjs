import fs from 'node:fs';import path from 'node:path';import assert from 'node:assert/strict';import {chromium} from 'playwright';
const out=process.env.PET_UI_REPORT_DIR;assert(out);fs.mkdirSync(out,{recursive:true});
const browser=await chromium.launch({executablePath:'/Applications/360Chrome.app/Contents/MacOS/360Chrome',headless:true});
try{
 const page=await browser.newPage({viewport:{width:800,height:600}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.goto('http://127.0.0.1:4217/tests/manual/host-surface.html');await page.waitForFunction(()=>window.__hostSurface);
 const item=await page.evaluate(async()=> (await (await fetch('/__app_fixtures')).json()).find(x=>x.manifest.id==='hachimi-notes'));assert(item);
 const fixtures=[
 {name:'empty',values:{metricValue:'想法收件箱',eyebrow:'笔记与待办保存在本机',headline:'先记下来，慢慢完成',note:'按录制说出想法，转写后保存；想去做的事，一键加入待办。'},ui:{mode:0,actions:['host.record','host.tasks']}},
 {name:'draft',values:{metricValue:'转写完成 · 待保存',eyebrow:'这是一条演示想法',headline:'想在周末做一份旅行手账，记录路上的小发现。',note:'选几张喜欢的照片，再写上最想记住的一句话。',badge:'1/1'},ui:{mode:4,actions:['host.save','host.todo'],context:'draft-demo'}},
 {name:'tasks',values:{metricValue:'待完成 · 3',badge:'1/1',eyebrow:'> 整理旅行照片',headline:'  给植物换水',metricLabel:'  写下本周小结'},ui:{mode:2,actions:['host.record','host.tasks','host.save','host.row_a','host.row_b','host.row_c','host.filter']}},
 {name:'detail',values:{metricValue:'事项全文',eyebrow:'待完成 · 未定期',headline:'整理旅行照片',note:'',badge:'1/1',metricUnit:'标记完成'},ui:{mode:3,actions:['host.record','host.tasks','host.save','host.toggle','host.filter'],context:'todo-demo'}},
 {name:'completed',values:{metricValue:'事项全文',eyebrow:'已完成 · 未定期',headline:'整理旅行照片',note:'',badge:'1/1',metricUnit:'撤销完成'},ui:{mode:3,actions:['host.record','host.tasks','host.save','host.toggle','host.filter'],context:'done-demo'}}];
 for(const fixture of fixtures){
  fixture.values.footer=fixture.name==='draft'?'2 保存想法 · 点按转为待办 · 左右翻页':fixture.name==='tasks'?'2 查看事项 · 上下选择 · 左右翻页 · 中按切换':fixture.name==='empty'?'1 录下想法 · 中按查看待办':'2 返回清单 · 点按完成或撤销';
  await page.evaluate(({item,fixture})=>window.__hostSurface.show(item,fixture),{item,fixture});await page.waitForTimeout(100);
  const screen=page.locator('.screen');await screen.screenshot({path:path.join(out,fixture.name+'.png')});
  for(const b of await screen.getByRole('button').all()){const r=await b.boundingBox();assert(r.width>=44&&r.height>=44);}
  if(fixture.name==='tasks'){await screen.getByRole('button',{name:fixture.values.eyebrow,exact:true}).click();assert.equal((await page.evaluate(()=>window.__hostSurface.calls)).at(-1).action,'host.row_a');}
  if(fixture.name==='detail'){await screen.getByRole('button',{name:'标记完成',exact:true}).click();assert.equal(await screen.getByRole('dialog').count(),1);await screen.getByRole('button',{name:'取消',exact:true}).click();assert.deepEqual(await page.evaluate(()=>window.__hostSurface.calls),[]);}
 }
 assert.deepEqual(errors,[]);fs.writeFileSync(path.join(out,'report.json'),JSON.stringify({states:fixtures.map(f=>f.name),errors,realDevice:false,fixtures:'synthetic, no personal records',touch:'44px minimum; row opens; mutation cancellation does not dispatch'},null,2));console.log('PASS five UI states, touch sizing, row dispatch and cancellation');
}finally{await browser.close();}
