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
assert 'src="/ha-guide"' in html and 'title="Complete Home Assistant installation' in html
assert 'href="/ha-guide" download=' in html and 'href="/ha-rest.yaml" download=' in html
assert 'MQTT Discovery' in html
assert re.findall(r'data-page="([^"]+)"', html) == ['overview','firmware','logs','integration']
assert '>HA Integration</button>' in html
assert 'id="tab-logs"' in html and 'id="logsBody"' in html
assert 'href="/api/v1/get?pretty=1" target="_blank"' in html
assert html.count('id="muteBtn"') == 1 and html.count('id="refreshBtn"') == 1
assert html.index('id="muteBtn"') < html.index('id="refreshBtn"') < html.index('<section id="overview"')
assert 'otaUsername' not in html and 'otaPassword' not in html
assert 'Update username' not in html and 'Update password' not in html
node = shutil.which("node")
if not node:
    raise SystemExit("Dashboard checks require Node.js.")

HARNESS = r"""
const assert=require('node:assert/strict'),vm=require('node:vm'),cryptoLib=require('node:crypto');
const nodes=new Map(),timers=new Map();let timerId=0,fetchCalls=0,lastXhr=null;
function node(id){if(!nodes.has(id))nodes.set(id,{id,textContent:'',innerHTML:'',value:id==='statusFilter'?'all':'',hidden:false,disabled:false,checked:false,files:[],dataset:{},listeners:{},attributes:{},className:'',style:{},classList:{values:new Set(),toggle(name,enabled){if(enabled)this.values.add(name);else this.values.delete(name);}},addEventListener(event,handler){this.listeners[event]=handler;},setAttribute(key,value){this.attributes[key]=value;},focus(){this.focused=true;}});return nodes.get(id);}
const tabs=['overview','firmware','logs','integration'].map(page=>{const button=node('tab-'+page);button.dataset.page=page;return button;});
class FormDataMock{constructor(){this.values=[];}append(...args){this.values.push(args);}}
class XhrMock{constructor(){lastXhr=this;this.headers={};this.upload={};}open(method,url){this.method=method;this.url=url;}setRequestHeader(key,value){this.headers[key]=value;}send(body){this.body=body;}}
const context={console,Uint8Array,Uint32Array,DataView,ArrayBuffer,Set,Date,Math,Number,String,Boolean,JSON,Error,AbortController,Promise,FormData:FormDataMock,XMLHttpRequest:XhrMock,location:{origin:'http://192.168.4.1'},navigator:{},btoa:value=>{if(/[^\x00-\xff]/.test(value))throw new Error('Invalid character');return Buffer.from(value,'binary').toString('base64');},setTimeout:callback=>{const id=++timerId;timers.set(id,callback);return id;},clearTimeout:id=>timers.delete(id),fetch:async()=>{fetchCalls++;throw new Error('offline');},document:{getElementById:node,querySelectorAll:()=>tabs},assert,cryptoLib,node,nodes,tabs,getLastXhr:()=>lastXhr,getFetchCalls:()=>fetchCalls};
vm.createContext(context);
vm.runInContext(SCRIPT,context);
vm.runInContext(TESTS,context).then(()=>console.log('Dashboard regression checks passed: rendering, filters, network failures, battery policy, tabs, offline guide, mute, OTA, logs and SHA-256.')).catch(error=>{console.error(error);process.exitCode=1;});
"""

TESTS = r"""
(async()=>{
function fixture(total=128){return {gateway:{firmware_id:'v0.2.4-tconnpro',ip:'192.168.4.1',uptime_seconds:100,lora_freq_mhz:868,lora_sf:9,total_valid_packets:50,local_spd_pin:7,stale_after_seconds:120,alarm_active:false,alarm_muted:false,alarm_kind:'none',ota:{enabled:false,busy:false,slot_size:3145728}},summary:{},spds:Array.from({length:total},(_,id)=>({spd_id:id,friendly_name:id===1?'<img src=x onerror="alert(1)">':`Beacon ${id}`,local:id===0,source:id===0?'local':'lora',status:['OK','FAIL','STALE','UNKNOWN'][id%4],stale:id%4===2,status_code:id%4===0?1:null,temperature_c:id===1?-127:22,hardware_version:2,firmware_version:1,battery_voltage_v:id===0?null:3.31,battery_low_alarm:id%9===0&&id!==0,rssi_dbm:-93.5,snr_db:8.2,packet_count:12,packet_loss_window_total:20,packet_loss_window_missed:1,packet_loss_percent:5,total_skipped:1,age_seconds:18,last_reported_status:'OK',nonce_hex:'<nonce>',raw_hex:'<packet>'}))};}
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
const originalSnapshot=latest;
latest=null;assert.equal(hasBatteryHardware(),false);
latest=fixture(3);latest.spds.forEach(x=>{x.hardware_version=x.local?2:null;x.battery_voltage_v=3.6;x.status='UNKNOWN';x.stale=false;});renderRows();assert.equal(hasBatteryHardware(),false);assert.equal(node('batteryHeader').hidden,true);assert(!node('body').innerHTML.includes('data-label="Battery"'));assert.match(node('body').innerHTML,/colspan="6"/);
latest.spds.forEach(x=>{if(!x.local)x.hardware_version=1;});renderRows();assert.equal(hasBatteryHardware(),false);assert(node('spdTable').classList.values.has('without-battery'));assert.match(node('body').innerHTML,/RSSI -94 dBm/);assert.match(node('body').innerHTML,/SNR 8.2 dB/);
latest=fixture(128);latest.spds.forEach(x=>{x.hardware_version=x.local?2:1;});latest.spds[127].hardware_version=2;latest.spds[127].status='STALE';latest.spds[127].stale=true;latest.spds[127].battery_voltage_v=null;renderRows();assert.equal(hasBatteryHardware(),true);assert.equal(node('batteryHeader').hidden,false);assert.match(node('body').innerHTML,/data-label="Battery"/);assert.match(node('body').innerHTML,/colspan="7"/);
node('search').value='BEACON 3';renderRows();assert(!visibleSpds().some(x=>x.spd_id===127));assert.equal(node('batteryHeader').hidden,false);assert.match(node('body').innerHTML,/colspan="7"/);
node('search').value='';node('statusFilter').value='fail';renderRows();assert(!visibleSpds().some(x=>x.spd_id===127));assert.equal(node('batteryHeader').hidden,false);
node('search').value='no such beacon';renderRows();assert.equal(visibleSpds().length,0);assert.equal(node('batteryHeader').hidden,false);
node('search').value='';node('statusFilter').value='all';latest.spds[127].hardware_version='2';renderRows();assert.equal(node('batteryHeader').hidden,true);latest.spds[127].hardware_version=2;renderRows();assert.equal(node('batteryHeader').hidden,false);
latest=originalSnapshot;renderRows();

assert.match(node('footerVersion').textContent,/v0.2.4-tconnpro/);
lastSuccess=new Date(2026,9,13,23,4,3);renderConnection();assert.equal(node('lastRefresh').textContent,'Last refreshed 2026-10-13 23:04:03');assert.equal(bootTime24(3542000),'00:59:02');assert.match(rowHtml(latest.spds[1]).split('<tr class="detailrow"')[0],/SNR 8.2 dB/);assert.equal(node('muteBtn').textContent,'MUTE');latest.gateway.alarm_muted=true;render();assert.equal(node('muteBtn').textContent,'UNMUTE');latest.gateway.alarm_muted=false;
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
selectPage('firmware');assert.equal(node('overview').hidden,true);assert.equal(node('firmware').hidden,false);assert.equal(tabs[1].attributes['aria-selected'],'true');assert.equal(tabs[1].tabIndex,0);
tabs[1].listeners.keydown({key:'Home',preventDefault(){}});assert.equal(node('overview').hidden,false);assert.equal(tabs[0].focused,true);tabs[0].listeners.keydown({key:'ArrowRight',preventDefault(){}});assert.equal(tabs[1].attributes['aria-selected'],'true');tabs[1].listeners.keydown({key:'End',preventDefault(){}});assert.equal(tabs[3].attributes['aria-selected'],'true');tabs[3].listeners.keydown({key:'ArrowRight',preventDefault(){}});assert.equal(tabs[0].attributes['aria-selected'],'true');node('haGuideFrame').contentDocument={body:{scrollHeight:1800},documentElement:{scrollHeight:1801,classList:{add(value){this.value=value;}}}};selectPage('integration');resizeHaGuide();assert.equal(node('haGuideFrame').style.height,'1801px');assert.equal(node('haGuideFrame').contentDocument.documentElement.classList.value,'embedded');selectPage('overview');
const snapshot=node('body').innerHTML;fetch=async()=>{throw new Error('lost connection');};await load();assert.equal(connected,false);assert.equal(node('body').innerHTML,snapshot);assert.match(node('networkNotice').textContent,/last received readings/);assert.equal(node('muteBtn').disabled,true);
connected=true;fetch=async()=>({ok:false,status:500});await toggleMute();assert.equal(muting,false);assert.equal(node('actionError').hidden,false);assert.match(node('actionError').textContent,/Could not change/);
fetch=async()=>({ok:true,json:async()=>fixture()});await load();assert.equal(connected,true);assert.equal(node('networkNotice').hidden,true);
const encoder=new TextEncoder();for(const input of ['', 'abc', 'The quick brown fox jumps over the lazy dog']){const bytes=encoder.encode(input);assert.equal(sha256Fallback(bytes),cryptoLib.createHash('sha256').update(bytes).digest('hex'));}
for(const size of [1,55,56,63,64,65,1000,3145728]){const bytes=new Uint8Array(size);for(let i=0;i<size;i++)bytes[i]=(i*17+13)&255;assert.equal(sha256Fallback(bytes),cryptoLib.createHash('sha256').update(bytes).digest('hex'));}
const appBytes=new Uint8Array(256);appBytes[0]=0xe9;const appFile={name:'tconnectpro_868.ino.bin',size:appBytes.length,arrayBuffer:async()=>appBytes.buffer};
latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();assert(preparedFile);assert.equal(preparedFile.hash,cryptoLib.createHash('sha256').update(appBytes).digest('hex'));assert.equal(node('uploadBtn').disabled,true);
node('otaToken').value='invalid-token';node('updateConfirm').checked=true;renderOta();await uploadFirmware({preventDefault(){}});assert.equal(getLastXhr(),null);assert.equal(node('otaToken').value,'');assert.match(node('updateResult').textContent,/32–128 letters/);node('otaToken').value='SyntheticTestTokenForFirmwareUpdates';node('updateConfirm').checked=true;renderOta();assert.equal(node('uploadBtn').disabled,false);
await uploadFirmware({preventDefault(){}});const xhr=getLastXhr();assert.equal(xhr.method,'POST');assert.equal(xhr.url,'/api/v1/firmware');assert.equal(xhr.headers['X-WSPD-Update'],'1');assert.equal(xhr.headers.Authorization,'Bearer SyntheticTestTokenForFirmwareUpdates');
assert.deepEqual(Array.from(xhr.body.values,entry=>entry[0]),['sha256','size','firmware']);assert.equal(xhr.body.values[1][1],'256');assert.equal(xhr.body.values[2][1],appFile);assert.equal(uploading,true);assert.equal(node('firmwareFile').disabled,true);
xhr.upload.onprogress({lengthComputable:true,loaded:50,total:100});assert.equal(node('progress').value,50);xhr.status=202;xhr.responseText='{"status":"ok","message":"Firmware verified; restarting."}';xhr.onload();assert.equal(uploading,false);assert.equal(rebootPending,true);assert.equal(node('otaToken').value,'');assert.equal(node('updateConfirm').checked,false);assert.equal(preparedFile,null);
fetch=async()=>{throw new Error('restarting');};await load();assert.match(node('networkNotice').textContent,/restarting/);fetch=async()=>({ok:true,json:async()=>fixture()});await load();assert.equal(rebootPending,false);assert.equal(node('networkNotice').hidden,true);assert.match(node('updateResult').textContent,/Receiver reconnected/);
latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();node('otaToken').value='SyntheticTestTokenForFirmwareUpdates';node('updateConfirm').checked=true;renderOta();await uploadFirmware({preventDefault(){}});const failed=getLastXhr();failed.status=401;failed.responseText='{"error":"unauthorized","message":"The update token was rejected."}';failed.onload();assert.equal(node('otaToken').value,'');assert.match(node('updateResult').textContent,/token was rejected/);assert.equal(rebootPending,false);
for(const [status,responseText] of [[200,'<html>proxy page</html>'],[200,'{"status":"ok","message":"Firmware verified; restarting."}'],[202,'null']]){await load();latest.gateway.ota.enabled=true;node('firmwareFile').files=[appFile];await prepareFile();node('otaToken').value='SyntheticTestTokenForFirmwareUpdates';node('updateConfirm').checked=true;renderOta();await uploadFirmware({preventDefault(){}});const unexpected=getLastXhr();unexpected.status=status;unexpected.responseText=responseText;unexpected.onload();assert.equal(rebootPending,false);assert.equal(node('otaToken').value,'');assert.match(node('updateResult').textContent,/did not confirm/);assert.equal(node('progressText').textContent,'Update not confirmed');}
node('firmwareFile').files=[{...appFile,name:'boot.txt'}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/application .bin/);
node('firmwareFile').files=[{...appFile,size:3145729}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/exceeds/);
node('firmwareFile').files=[{...appFile,arrayBuffer:async()=>new Uint8Array(256).buffer}];await prepareFile();assert.equal(preparedFile,null);assert.match(node('updateResult').textContent,/image header/);
latest.gateway.ota.enabled=false;renderOta();assert.equal(node('uploadBtn').disabled,true);assert.match(node('otaAvailability').textContent,/disabled/);
activePage='overview';
const logEntry=(seq,synced=false)=>({seq,boot_ms:seq*1000,unix_ms:synced?new Date(2026,9,13,17,0,0).getTime()+seq*1000:null,message:seq===150?'<img src=x onerror="alert(1)">':`SPD 1 packet ${seq}`});
const logPage=(start,count,extra={})=>({boot_id:'boot-a',capacity:4096,bytes:2097152,psram:true,dropped:0,clock_synced:false,oldest_seq:1,next_seq:start+count,entries:Array.from({length:count},(_,i)=>logEntry(start+i,!!extra.clock_synced)),has_more:false,...extra});
let logUrls=[],logResponses=[];
const installLogs=responses=>{logResponses=responses.slice();logUrls=[];fetch=async url=>{logUrls.push(url);assert.match(url,/^\/api\/v1\/logs\?/);if(!logResponses.length)throw new Error('Unexpected log fetch');const value=logResponses.shift();return {ok:true,json:async()=>value};};};
installLogs([logPage(101,100)]);await loadLogs('latest');assert.equal(logUrls[0],'/api/v1/logs?tail=1&limit=100');assert.equal(logEntries.length,100);assert.equal(logEntries[0].seq,101);assert.equal(logAfter,200);assert.match(node('logsBody').innerHTML,/Before clock sync/);assert(!node('logsBody').innerHTML.includes('<img src=x'));assert(node('logsBody').innerHTML.includes('&lt;img src=x'));
assert.match(node('logsMeta').textContent,/PSRAM.*2048 KiB.*4096/);assert.equal(node('logsOlderBtn').disabled,false);
installLogs([logPage(201,5,{clock_synced:true}),logPage(106,100,{clock_synced:true})]);await loadLogs();assert.deepEqual(logUrls,['/api/v1/logs?after=200&limit=100','/api/v1/logs?tail=1&limit=100']);assert.equal(logEntries[0].seq,106);assert.equal(logEntries[99].seq,205);assert(!node('logsBody').innerHTML.includes('Before clock sync'));assert.match(node('logsBody').innerHTML,/2026-10-13<br>17:/);assert.match(node('logsMeta').textContent,/clock synchronized/);
installLogs([logPage(206,100,{clock_synced:true,has_more:true}),logPage(306,5,{clock_synced:true})]);await loadLogs();assert.equal(logUrls.length,2);assert.equal(logAfter,310);assert.equal(logEntries.length,205);assert.equal(logEntries[logEntries.length-1].seq,310);
installLogs([logPage(6,100,{clock_synced:true})]);await loadLogs('older');assert.equal(logUrls[0],'/api/v1/logs?before=106&limit=100');assert.equal(logFollowLive,false);assert.equal(logEntries[0].seq,6);assert.match(node('logsNotice').textContent,/paused/);const olderCalls=logUrls.length;await loadLogs();assert.equal(logUrls.length,olderCalls);
installLogs([logPage(6,0,{clock_synced:true,oldest_seq:110,dropped:109})]);await loadLogs('older');assert.equal(logEntries[0].seq,110);assert.match(node('logsNotice').textContent,/109 older events were overwritten/);assert.equal(node('logsOlderBtn').disabled,true);
installLogs([logPage(211,100,{clock_synced:true,oldest_seq:110,dropped:109})]);await loadLogs('latest');assert.equal(logFollowLive,true);assert.equal(logEntries.length,100);assert.equal(logEntries[0].seq,211);
installLogs([logPage(1,0,{boot_id:'boot-b',next_seq:5}),logPage(1,4,{boot_id:'boot-b'})]);await loadLogs();assert.deepEqual(logUrls,['/api/v1/logs?after=310&limit=100','/api/v1/logs?tail=1&limit=100']);assert.equal(logBootId,'boot-b');assert.equal(logEntries.length,4);assert.equal(logAfter,4);assert.match(node('logsNotice').textContent,/receiver restarted/);
const logSnapshot=node('logsBody').innerHTML;fetch=async()=>{throw new Error('offline');};await loadLogs();assert.equal(node('logsBody').innerHTML,logSnapshot);assert.equal(node('logsError').hidden,false);
installLogs(Array.from({length:5},(_,page)=>logPage(5+page*100,100,{boot_id:'boot-b',has_more:true})));await loadLogs();assert.equal(logUrls.length,5);assert.equal(logAfter,504);assert.equal(logEntries.length,504);
installLogs([logPage(505,10,{boot_id:'boot-b'})]);await loadLogs();assert.equal(logUrls[0],'/api/v1/logs?after=504&limit=100');assert.equal(logAfter,514);assert.equal(logEntries.length,514);
logEntries=Array.from({length:1000},(_,i)=>logEntry(1001+i));logAfter=2000;installLogs([logPage(2001,100,{boot_id:'boot-b'})]);await loadLogs();assert.equal(logEntries.length,1000);assert.equal(logEntries[0].seq,1101);assert.match(node('logsCount').textContent,/window limited to 1000/);
installLogs([logPage(1001,100,{boot_id:'boot-b'})]);await loadLogs('older');assert.equal(logEntries.length,1000);assert.equal(logEntries[0].seq,1001);assert.equal(logEntries[999].seq,2000);assert.equal(logFollowLive,false);
const lastLogSnapshot=node('logsBody').innerHTML;installLogs([{boot_id:'boot-b',oldest_seq:1,entries:[{seq:1,message:null}]}]);await loadLogs('latest');assert.equal(node('logsBody').innerHTML,lastLogSnapshot);assert.equal(node('logsError').hidden,false);

latest=fixture(0);render();assert.match(node('empty').innerHTML,/No SPDs configured/);
})()
"""

# TextEncoder is intentionally supplied outside the app, just as a browser does.
HARNESS = HARNESS.replace("console,Uint8Array", "console,TextEncoder,Uint8Array")
with tempfile.TemporaryDirectory(prefix="wspd-dashboard-check-") as directory:
    path = Path(directory) / "dashboard.js"
    path.write_text("const SCRIPT=" + repr(script) + ";\nconst TESTS=" + repr(TESTS) + ";\n" + HARNESS)
    subprocess.run([node, str(path)], check=True)
