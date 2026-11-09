// Execute the embedded setup page with a small DOM/fetch harness.
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync('main/setup.html','utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const fields=new Map();
const element=s=>{
    if(!fields.has(s)){
        const e={checked:false,textContent:'',type:'password',replaceChildren(...items){this.items=items;}};
        let value='';Object.defineProperty(e,'value',{get:()=>value,set:v=>{value=String(v);}});fields.set(s,e);
    }return fields.get(s);
};
const calls=[];
const context={document:{querySelector:element,createElement:()=>({value:''})},fetch:async(path,options)=>{
    calls.push({path,options});
    const data=path==='/api/status'?{ssid:'Cottage',broker:'mqtt.local',port:1883,tls:false,username:'pixel',base_topic:'kegscale/ab8070',mqtt_password_saved:true,battery_valid:true,battery_raw:633,battery_percent:75,battery_empty:565,battery_full:656,firmware:'1.0.9-tc001-dev',wifi:true,mqtt:true,connected:false}:
        path==='/api/wifi'?{networks:['Cottage','Guest']}:{message:'Saved'};
    return {ok:true,json:async()=>data};
}};
(async()=>{
    vm.runInNewContext(script,context);await new Promise(resolve=>setImmediate(resolve));
    assert.equal(element('#broker').value,'mqtt.local');assert.equal(element('#baseTopic').value,'kegscale/ab8070');
    assert.match(element('#batteryStatus').textContent,/75%/);assert.equal(element('#batteryEmpty').value,'565');
    assert.equal(element('#password').value,'');assert.equal(element('#mqttPassword').value,'');
    assert.match(element('#status').textContent,/waiting for fresh/);
    element('#tls').checked=true;element('#tls').onchange();assert.equal(element('#port').value,'8883');
    // Real DOM input.value converts assignments to strings.
    element('#port').value='8883';element('#tls').checked=false;element('#tls').onchange();assert.equal(element('#port').value,'1883');
    element('#password').value='wifi-example';element('#mqttPassword').value='broker-example';
    await element('#config').onsubmit({preventDefault(){}});
    const saved=JSON.parse(calls.at(-1).options.body);
    assert.equal(saved.base_topic,'kegscale/ab8070');assert.equal(saved.mqtt_password,'broker-example');assert.equal(saved.port,1883);
    assert.equal(saved.broker,'mqtt.local');assert.equal(saved.open,false);assert.equal(saved.tls,false);
    assert.equal(calls.at(-1).options.headers['X-Keg-Setup'],'__SETUP_TOKEN__');
    assert(!('host' in saved));assert(!('forget' in saved));
    await element('#batteryConfig').onsubmit({preventDefault(){}});
    const calibration=calls.find(c=>c.path==='/api/battery');assert.deepEqual(JSON.parse(calibration.options.body),{empty:565,full:656});
    await element('#scanWifi').onclick();assert.equal(element('#networks').items.length,2);
    console.log('PASS: setup status, password privacy, MQTT configuration submission, TLS ports, Wi-Fi scan and battery calibration');
})().catch(e=>{console.error(e);process.exitCode=1;});
