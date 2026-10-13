#!/usr/bin/env python3
"""Exercise the embedded dashboard, upload contract and HTTP SHA-256 fallback."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware/servers/tconnectpro_868/web_ui.h"
source = HEADER.read_text()
html = source.split('R"HTML(', 1)[1].split(')HTML";', 1)[0]
script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
# Keep listeners and actual production functions, but invoke initial polling in tests.
script = re.sub(r"load\(\);\s*$", "", script)
assert not re.search(r'<(?:script|link)\b[^>]*(?:src|href)\s*=\s*["\']https?://', html)
assert '@media(max-width:760px)' in html
assert 'role="tabpanel"' in html and 'role="tablist"' in html
assert 'data-label="Temperature"' in script
assert 'https://github.com/djeZo888/WirelessSPDSystem/blob/main/docs/home-assistant.md' in html
assert 'MQTT Discovery' in html and 'recommended' in html
node = shutil.which("node")
if not node:
    raise SystemExit("Dashboard checks require Node.js.")

HARNESS = r"""
const assert=require('node:assert/strict'),vm=require('node:vm'),cryptoLib=require('node:crypto');
const nodes=new Map(),timers=new Map();let timerId=0,fetchCalls=0,lastXhr=null;
function node(id){if(!nodes.has(id))nodes.set(id,{id,textContent:'',innerHTML:'',value:id==='statusFilter'?'all':'',hidden:false,disabled:false,checked:false,files:[],dataset:{},listeners:{},attributes:{},className:'',style:{},classList:{values:new Set(),toggle(name,enabled){if(enabled)this.values.add(name);else this.values.delete(name);}},addEventListener(event,handler){this.listeners[event]=handler;},setAttribute(key,value){this.attributes[key]=value;},focus(){this.focused=true;}});return nodes.get(id);}
const tabs=['overview','integration','firmware'].map(page=>{const button=node('tab-'+page);button.dataset.page=page;return button;});
class FormDataMock{constructor(){this.values=[];}append(...args){this.values.push(args);}}
class XhrMock{constructor(){lastXhr=this;this.headers={};this.upload={};}open(method,url){this.method=method;this.url=url;}setRequestHeader(key,value){this.headers[key]=value;}send(body){this.body=body;}}
const context={console,Uint8Array,Uint32Array,DataView,ArrayBuffer,Set,Date,Math,Number,String,Boolean,JSON,Error,AbortController,Promise,FormData:FormDataMock,XMLHttpRequest:XhrMock,location:{origin:'http://192.168.4.1'},navigator:{},btoa:value=>{if(/[^\x00-\xff]/.test(value))throw new Error('Invalid character');return Buffer.from(value,'binary').toString('base64');},setTimeout:callback=>{const id=++timerId;timers.set(id,callback);return id;},clearTimeout:id=>timers.delete(id),fetch:async()=>{fetchCalls++;throw new Error('offline');},document:{getElementById:node,querySelectorAll:()=>tabs},assert,cryptoLib,node,nodes,tabs,getLastXhr:()=>lastXhr,getFetchCalls:()=>fetchCalls};
vm.createContext(context);
vm.runInContext(SCRIPT,context);
vm.runInContext(TESTS,context).then(()=>console.log('Dashboard regression checks passed: rendering, filters, network failures, mute, OTA and SHA-256.')).catch(error=>{console.error(error);process.exitCode=1;});
"""

TESTS = r"""
(async()=>{
function fixture(total=128){return {gateway:{firmware_id:'v0.2-tconnpro',ip:'192.168.4.1',uptime_seconds:100,lora_freq_mhz:868,lora_sf:9,total_valid_packets:50,local_spd_pin:7,stale_after_seconds:120,alarm_active:false,alarm_muted:false,alarm_kind:'none',ota:{enabled:false,busy:false,slot_size:3145728}},summary:{},spds:Array.from({length:total},(_,id)=>({spd_id:id,friendly_name:id===1?'<img src=x onerror="alert(1)">':`Beacon ${id}`,local:id===0,source:id===0?'local':'lora',status:['OK','FAIL','STALE','UNKNOWN'][id%4],stale:id%4===2,status_code:id%4===0?1:null,temperature_c:id===1?-127:22,hardware_version:2,firmware_version:1,battery_voltage_v:id===0?null:3.31,battery_low_alarm:id%9===0&&id!==0,rssi_dbm:-93.5,snr_db:8.2,packet_count:12,packet_loss_window_total:20,packet_loss_window_missed:1,packet_loss_percent:5,total_skipped:1,age_seconds:18,last_reported_status:'OK',nonce_hex:'<nonce>',raw_hex:'<packet>'}))};}
latest=fixture();connected=true;lastSuccess=new Date();render();renderConnection();
assert.equal((node('body').innerHTML.match(/class="spdrow/g)||[]).length,128);
assert.match(node('listCount').textContent,/Showing 128 of 128/);
assert(!node('body').innerHTML.includes('<img src=x'));
assert(node('body').innerHTML.includes('&lt;img src=x onerror=&quot;alert(1)&quot;&gt;'));
assert(!node('body').innerHTML.includes('<nonce>'));
assert.equal(visibleSpds()[0].spd_id,0);assert.equal(visibleSpds()[127].spd_id,127);
assert.equal(temperature({temperature_c:-128}),'\x3cspan class="sensor-error">ADC error</span>');
assert.match(temperature({temperature_c:-127}),/NTC open/);assert.match(temperature({temperature_c:-126}),/NTC short/);
assert.equal(temperature({local:true,temperature_c:0}),'—');assert.equal(temperature({temperature_c:null}),'—');
assert.equal(age(null),'Not yet seen');assert.equal(age(0),'0 s ago');assert.equal(age(61),'1 min ago');
assert.equal(rowClass({status:'FAIL',stale:true}),'stale');
assert.match(node('footerVersion').textContent,/v0.2-tconnpro/);
assert.match(node('apiCurl').textContent,/curl http:\/\/192\.168\.4\.1\/api\/v1\/get/);
assert.match(node('mqttConnectionText').textContent,/MQTT is disabled/);
latest.gateway.mqtt={enabled:true,connected:false};render();assert.match(node('mqttConnectionText').textContent,/broker disconnected/);
latest.gateway.mqtt.connected=true;latest.gateway.gateway_id='wspd-test123';render();assert.match(node('mqttConnectionText').textContent,/broker connected/);assert.equal(node('gatewayIdentity').textContent,'wspd-test123');
latest.gateway.alarm_active=true;render();assert.equal(node('alarmSubtitle').textContent,'The relay uses its configured alarm pattern.');latest.gateway.alarm_active=false;
for(const state of ['fail','ok','stale','unknown','battery']){node('statusFilter').value=state;const expected=latest.spds.filter(x=>state==='battery'?x.battery_low_alarm:rowClass(x)===state);assert.equal(visibleSpds().length,expected.length);renderRows();assert.match(node('listCount').textContent,new RegExp('Showing '+expected.length+' of 128'));}
node('statusFilter').value='all';node('search').value='001';assert.equal(visibleSpds()[0].spd_id,1);
node('search').value='BEACON 127';assert.equal(visibleSpds().length,1);assert.equal(visibleSpds()[0].spd_id,127);
node('search').value='missing';renderRows();assert.equal(node('empty').hidden,false);assert.match(node('empty').innerHTML,/No SPDs match/);
node('search').value='';expandedIds.add('3');renderRows();assert.match(node('body').innerHTML,/data-details="3" aria-expanded="true"/);assert(node('body').innerHTML.includes('id="details-3"><td'));
selectPage('firmware');assert.equal(node('overview').hidden,true);assert.equal(node('firmware').hidden,false);assert.equal(tabs[2].attributes['aria-selected'],'true');assert.equal(tabs[2].tabIndex,0);
tabs[2].listeners.keydown({key:'ArrowRight',preventDefault(){}});assert.equal(node('overview').hidden,false);assert.equal(tabs[0].focused,true);
const snapshot=node('body').innerHTML;fetch=async()=>{throw new Error('lost connection');};await load();assert.equal(connected,false);assert.equal(node('body').innerHTML,snapshot);assert.match(node('networkNotice').textContent,/last received readings/);assert.equal(node('muteBtn').disabled,true);
connected=true;fetch=async()=>({ok:false,status:500});await toggleMute();assert.equal(muting,false);assert.equal(node('actionError').hidden,false);assert.match(node('actionError').textContent,/Could not change/);
fetch=async()=>({ok:true,json:async()=>fixture()});await load();assert.equal(connected,true);assert.equal(node('networkNotice').hidden,true);
const encoder=new TextEncoder();for(const input of ['', 'abc', 'The quick brown fox jumps over the lazy dog']){const bytes=encoder.encode(input);assert.equal(sha256Fallback(bytes),cryptoLib.createHash('sha256').update(bytes).digest('hex'));}
for(const size of [1,55,56,63,64,65,1000,3145728]){const bytes=new Uint8Array(size);for(let i=0;i<size;i++)bytes[i]=(i*17+13)&255;assert.equal(sha256Fallback(bytes),cryptoLib.createHash('sha256').update(bytes).digest('hex'));}
const appBytes=new Uint8Array(256);appBytes[0]=0xe9;const appFile={name:'tconnectpro_868.ino.bin',size:appBytes.length,arrayBuffer:async()=>appBytes.buffer};
latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();assert(preparedFile);assert.equal(preparedFile.hash,cryptoLib.createHash('sha256').update(appBytes).digest('hex'));assert.equal(node('uploadBtn').disabled,true);
node('otaUsername').value='admin';node('otaPassword').value='synthetic-password';node('updateConfirm').checked=true;renderOta();assert.equal(node('uploadBtn').disabled,false);
await uploadFirmware({preventDefault(){}});const xhr=getLastXhr();assert.equal(xhr.method,'POST');assert.equal(xhr.url,'/api/v1/firmware');assert.equal(xhr.headers['X-WSPD-Update'],'1');assert.equal(xhr.headers.Authorization,'Basic '+btoa('admin:synthetic-password'));
assert.deepEqual(Array.from(xhr.body.values,entry=>entry[0]),['sha256','size','firmware']);assert.equal(xhr.body.values[1][1],'256');assert.equal(xhr.body.values[2][1],appFile);assert.equal(uploading,true);assert.equal(node('firmwareFile').disabled,true);
xhr.upload.onprogress({lengthComputable:true,loaded:50,total:100});assert.equal(node('progress').value,50);xhr.status=202;xhr.responseText='{"status":"ok","message":"Firmware verified; restarting."}';xhr.onload();assert.equal(uploading,false);assert.equal(rebootPending,true);assert.equal(node('otaPassword').value,'');assert.equal(node('updateConfirm').checked,false);assert.equal(preparedFile,null);
fetch=async()=>{throw new Error('restarting');};await load();assert.match(node('networkNotice').textContent,/restarting/);fetch=async()=>({ok:true,json:async()=>fixture()});await load();assert.equal(rebootPending,false);assert.equal(node('networkNotice').hidden,true);assert.match(node('updateResult').textContent,/Receiver reconnected/);
latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();node('otaPassword').value='synthetic-password';node('updateConfirm').checked=true;renderOta();await uploadFirmware({preventDefault(){}});const failed=getLastXhr();failed.status=401;failed.responseText='{"error":"unauthorized","message":"Update credentials were rejected."}';failed.onload();assert.equal(node('otaPassword').value,'');assert.match(node('updateResult').textContent,/credentials were rejected/);assert.equal(rebootPending,false);
for(const [status,responseText] of [[200,'<html>proxy page</html>'],[200,'{"status":"ok","message":"Firmware verified; restarting."}'],[202,'null']]){await load();latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();node('otaPassword').value='synthetic-password';node('updateConfirm').checked=true;renderOta();await uploadFirmware({preventDefault(){}});const unexpected=getLastXhr();unexpected.status=status;unexpected.responseText=responseText;unexpected.onload();assert.equal(rebootPending,false);assert.equal(node('otaPassword').value,'');assert.match(node('updateResult').textContent,/did not confirm/);assert.equal(node('progressText').textContent,'Update not confirmed');}
node('firmwareFile').files=[{...appFile,name:'boot.txt'}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/application .bin/);
node('firmwareFile').files=[{...appFile,size:3145729}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/exceeds/);
node('firmwareFile').files=[{...appFile,arrayBuffer:async()=>new Uint8Array(256).buffer}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/image header/);
latest.gateway.ota.enabled=false;renderOta();assert.equal(node('uploadBtn').disabled,true);assert.match(node('otaAvailability').textContent,/disabled/);
latest=fixture(0);render();assert.match(node('empty').innerHTML,/No SPDs configured/);
})()
"""

# TextEncoder is intentionally supplied outside the app, just as a browser does.
HARNESS = HARNESS.replace("console,Uint8Array", "console,TextEncoder,Uint8Array")
with tempfile.TemporaryDirectory(prefix="wspd-dashboard-check-") as directory:
    path = Path(directory) / "dashboard.js"
    path.write_text("const SCRIPT=" + repr(script) + ";\nconst TESTS=" + repr(TESTS) + ";\n" + HARNESS)
    subprocess.run([node, str(path)], check=True)
