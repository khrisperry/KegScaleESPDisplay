const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync('main/battery.html','utf8');const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const fields=new Map();const q=s=>{if(!fields.has(s)){let value='';const e={textContent:'',disabled:false};Object.defineProperty(e,'value',{get:()=>value,set:v=>value=String(v)});fields.set(s,e);}return fields.get(s);};
let poll,fail=false,valid=true,raw=619,empty=565,full=656;const calls=[];
const env={document:{querySelector:q},setInterval:f=>{poll=f;},fetch:async(path,options)=>{
 calls.push({path,options});if(fail)throw Error('Clock offline');
 if(options.method==='POST'){const d=JSON.parse(options.body);empty=d.empty;full=d.full;return{ok:true,json:async()=>({message:'Battery calibration saved. No restart needed.'})};}
 return{ok:true,json:async()=>({hostname:'KegPixel-1234',firmware:'1.0.10-tc001-dev',battery_valid:valid,battery_percent:59,battery_raw:raw,battery_empty:empty,battery_full:full})};
}};
const flush=()=>new Promise(r=>setImmediate(r));
(async()=>{
 vm.runInNewContext(script,env);await flush();
 assert.match(q('#percent').textContent,/59%/);assert.equal(q('#empty').value,'565');assert.equal(q('#full').value,'656');
 assert.equal(q('#useFull').disabled,false);assert.equal(calls[0].options.headers['X-Keg-Setup'],'__SETUP_TOKEN__');
 q('#empty').value=570;q('#full').value=630;raw=620;await poll();await flush();
 assert.equal(q('#empty').value,'570');assert.equal(q('#full').value,'630');assert.match(q('#raw').textContent,/620/);
 const before=calls.filter(c=>c.options.method==='POST').length;
 q('#useFull').onclick();assert.equal(q('#full').value,'620');assert.equal(calls.filter(c=>c.options.method==='POST').length,before);
 await q('#calibration').onsubmit({preventDefault(){}});
 const saved=JSON.parse(calls.find(c=>c.options.method==='POST').options.body);
 assert.deepEqual(saved,{empty:570,full:620});assert.equal(q('#empty').value,'570');assert.equal(q('#full').value,'620');
 assert.match(q('#result').textContent,/No restart needed/);
 q('#full').value=575;await q('#calibration').onsubmit({preventDefault(){}});
 assert.equal(calls.filter(c=>c.options.method==='POST').length,before+1);assert.match(q('#result').textContent,/at least 20/);
 valid=false;await poll();await flush();assert.equal(q('#percent').textContent,'--%');assert.equal(q('#useFull').disabled,true);
 fail=true;await poll();await flush();assert.match(q('#updated').textContent,/Clock offline/);
 console.log('PASS: LAN battery polling, edit preservation, full-field prefill without save, calibration submission/validation, invalid/offline states');
})().catch(e=>{console.error(e);process.exitCode=1;});
