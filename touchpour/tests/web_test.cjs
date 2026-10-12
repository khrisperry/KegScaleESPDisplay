// Run with Playwright installed. Optional CHROMIUM_EXECUTABLE selects a local
// browser, and TOUCHPOUR_SCREENSHOTS writes screenshots for visual review.
const fs=require('fs'),http=require('http'),path=require('path'),assert=require('assert');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const html=path.join(__dirname,'../components/tap_control/index.html');
const settings={closed_us:1000,open_us:2000,stop_mm:80,min_mm:237,max_mm:267,tray_mm:299,stable_mm:15,jump_mm:80,stale_ms:350,max_pour_ms:30000,servo_calibrated:true,distance_calibrated:true};
const state={version:'0.1.10',build:'ESP-IDF v6.0.1',state:'IDLE',reason:'Emergency stop',pouring:false,ready:false,distance_mm:299,raw_distance_mm:299,filtered_distance_mm:299,filtered_valid:true,measurement_valid:true,sensor_status:11,age_ms:15,stable_samples:0,servo_us:1000,setup_mode:false,updating:false,history_saving:false,history_available:true,ota_max_bytes:6291456,ip:'192.168.91.20',settings,foam_test:{recording:false,samples:0,elapsed_ms:0},pours:[{id:7,duration_ms:4200,samples:11,manual_hold:false,reason:'Fill target reached <test>'}]};
const requests=[];
const server=http.createServer((q,r)=>{
 if(!q.url.startsWith('/api/')){r.setHeader('Content-Type','text/html');return r.end(fs.readFileSync(html));}
 const chunks=[];q.on('data',c=>chunks.push(c));q.on('end',()=>{
  const data=Buffer.concat(chunks);
  if(q.method==='POST'){assert.equal(q.headers['x-tap-control'],'1');requests.push({url:q.url,data:q.url==='/api/firmware'?data:JSON.parse(data)});}
  r.setHeader('Content-Type','application/json');
  if(q.url==='/api/status')return r.end(JSON.stringify(state));
  if(q.url==='/api/wifi/scan')return r.end(JSON.stringify([{ssid:'Test <network>',rssi:-40,secure:true}]));
  if(q.url==='/api/settings'){Object.assign(settings,JSON.parse(data));return r.end(JSON.stringify(state));}
  r.end(JSON.stringify({accepted:true,message:'Saved'}));
 });
});
(async()=>{
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({headless:true,...(process.env.CHROMIUM_EXECUTABLE?{executablePath:process.env.CHROMIUM_EXECUTABLE,args:process.env.CHROMIUM_ARGS?JSON.parse(process.env.CHROMIUM_ARGS):['--no-sandbox','--disable-dev-shm-usage']}: {})});
 try{
  const page=await browser.newPage({viewport:{width:1280,height:900}}),errors=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('dialog',d=>d.accept());
  await page.goto('http://127.0.0.1:'+server.address().port);
  await page.waitForFunction(()=>document.getElementById('version').textContent==='V0.1.10');
  assert.equal(await page.locator('#reason').textContent(),'No cup detected');
  assert(!(await page.locator('#start').isEnabled()));
  await page.locator('#stop').click();
  await page.waitForFunction(()=>document.getElementById('message').textContent==='Close requested.');
  assert(requests.some(x=>x.url==='/api/action'&&x.data.action==='stop'));
  await page.locator('#navSetup').click();
  const shots=process.env.TOUCHPOUR_SCREENSHOTS;if(shots)fs.mkdirSync(shots,{recursive:true});
  for(const width of [1280,390]){
   await page.setViewportSize({width,height:900});
   for(const group of ['servo','distance','advanced','wifi','history','foam','firmware']){
    await page.locator('[data-group="'+group+'"]').click();
    assert(await page.locator('[data-panel="'+group+'"]').isVisible());
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'Overflow '+width+' '+group);
    if(shots&&['distance','advanced','wifi'].includes(group))await page.screenshot({path:path.join(shots,group+'-'+width+'.png'),fullPage:true});
   }
  }
  await page.setViewportSize({width:1280,height:900});
  await page.locator('[data-group=distance]').click();
  const boxes=await page.locator('[data-panel=distance] input').evaluateAll(inputs=>inputs.map(i=>i.getBoundingClientRect().top));
  assert.equal(boxes[0],boxes[1]);assert.equal(boxes[2],boxes[3]);
  await page.locator('#captureTray').click();assert.equal(await page.locator('[name=tray_mm]').inputValue(),'299');
  await page.locator('[data-group=wifi]').click();await page.locator('#scan').click();
  await page.waitForFunction(()=>document.getElementById('networks').options.length===2);
  await page.locator('#networks').selectOption('Test <network>');assert.equal(await page.locator('#ssid').inputValue(),'Test <network>');
  await page.locator('#password').fill('testsecret');await page.locator('#showPassword').check();assert.equal(await page.locator('#password').getAttribute('type'),'text');
  await page.locator('[data-group=history]').click();assert((await page.locator('#pourList').textContent()).includes('<test>'));assert.equal(await page.locator('#pourList img').count(),0);
  assert.equal(await page.locator('#pourList a').getAttribute('href'),'/api/pours/download?id=7');
  await page.locator('[data-group=firmware]').click();
  const image=Buffer.alloc(512);image[0]=0xe9;image[12]=9;image.write('touchpour_display',80,'utf8');
  const wrong=Buffer.from(image);wrong[12]=5;
  await page.locator('#firmwareFile').setInputFiles({name:'wrong.bin',mimeType:'application/octet-stream',buffer:wrong});
  await page.locator('#uploadFirmware').click();await page.waitForFunction(()=>document.getElementById('message').textContent.includes('ESP32-S3'));
  assert(!requests.some(x=>x.url==='/api/firmware'));
  await page.locator('#firmwareFile').setInputFiles({name:'touchpour_display.bin',mimeType:'application/octet-stream',buffer:image});
  await page.locator('#uploadFirmware').click();await page.waitForFunction(()=>document.getElementById('uploadStatus').textContent.includes('Update complete'));
  assert(requests.some(x=>x.url==='/api/firmware'&&x.data.equals(image)));
  state.pouring=true;state.ready=false;state.state='POURING';state.reason='Pouring';
  await page.waitForFunction(()=>document.getElementById('state').textContent==='POURING');
  assert(!(await page.locator('#testOpen').isEnabled()));assert(!(await page.locator('#save').isEnabled()));
  assert(await page.locator('#stop').isVisible());assert(await page.locator('#stop').isEnabled());
  assert.deepEqual(errors,[]);
  console.log('PASS: real web UI desktop/mobile groups, no overflow, field alignment, idle status, stop, filtered capture, Wi-Fi/password, history escaping, correct S3 upload, pour lockouts');
 }finally{await browser.close();server.close();}
})().catch(e=>{console.error(e);server.close();process.exit(1)});
