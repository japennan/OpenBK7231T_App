#include "../obk_config.h"

#include "../new_common.h"
#include "../logging/logging.h"
#include "../httpserver/new_http.h"
#include "../new_pins.h"
#include "../jsmn/jsmn_h.h"
#include "../hal/hal_ota.h"
#include "../hal/hal_wifi.h"
#include "../hal/hal_flashVars.h"
#include "../littlefs/our_lfs.h"
#include "lwip/sockets.h"
#if ENABLE_BT_PROXY
#include "../hal/hal_bt_proxy.h"
#endif

#define DEFAULT_FLASH_LEN 0x200000

#if PLATFORM_RTL8710A || PLATFORM_GD32VW553
#undef DEFAULT_FLASH_LEN
#define DEFAULT_FLASH_LEN 0x400000
#elif PLATFORM_RTL8720D || PLATFORM_REALTEK_NEW
extern uint8_t flash_size_8720;
#undef DEFAULT_FLASH_LEN
#define DEFAULT_FLASH_LEN (flash_size_8720 << 20)
#endif

#include "../new_cfg.h"
// Commands register, execution API and cmd tokenizer
#include "../cmnds/cmd_public.h"
#include "../driver/drv_uartBridge.h"

#ifndef OBK_DISABLE_ALL_DRIVERS
#include "../driver/drv_local.h"
#endif

#define MAX_JSON_VALUE_LENGTH   128


int http_rest_error(http_request_t* request, int code, char* msg);

static int http_rest_get(http_request_t* request);
static int http_rest_post(http_request_t* request);
static int http_rest_app(http_request_t* request);

static int http_rest_post_pins(http_request_t* request);
static int http_rest_get_pins(http_request_t* request);

static int http_rest_get_channelTypes(http_request_t* request);
static int http_rest_post_channelTypes(http_request_t* request);

static int http_rest_get_seriallog(http_request_t* request);

static int http_rest_post_logconfig(http_request_t* request);
static int http_rest_get_logconfig(http_request_t* request);

#if ENABLE_LITTLEFS
static int http_rest_get_lfs_delete(http_request_t* request);
static int http_rest_get_lfs_file(http_request_t* request);
static int http_rest_run_lfs_file(http_request_t* request);
static int http_rest_post_lfs_file(http_request_t* request);
#endif

static int http_rest_post_reboot(http_request_t* request);
int http_rest_post_flash(http_request_t* request, int startaddr, int maxaddr);
static int http_rest_get_flash(http_request_t* request, int startaddr, int len);
static int http_rest_get_flash_advanced(http_request_t* request);
static int http_rest_post_flash_advanced(http_request_t* request);

static int http_rest_get_info(http_request_t* request);
static int http_rest_get_bt_scan(http_request_t* request);

static int http_rest_post_channels(http_request_t* request);
static int http_rest_get_channels(http_request_t* request);

static int http_rest_post_cmd(http_request_t* request);

static int http_rest_get_wl5(http_request_t* request);
static int http_rest_get_raw(http_request_t* request);


void init_rest() {
	HTTP_RegisterCallback("/api/", HTTP_GET, http_rest_get, 1);
	HTTP_RegisterCallback("/api/", HTTP_POST, http_rest_post, 1);
	HTTP_RegisterCallback("/app", HTTP_GET, http_rest_app, 1);
	HTTP_RegisterCallback("/wl5", HTTP_GET, http_rest_get_wl5, 1);   // MiBoxer-style app UI
	HTTP_RegisterCallback("/raw", HTTP_GET, http_rest_get_raw, 1);   // direct channel/raw UI
}

/* Extracts string token value into outBuffer (128 char). Returns true if the operation was successful. */
bool tryGetTokenString(const char* json, jsmntok_t* tok, char* outBuffer) {
	int length;
	if (tok == NULL || tok->type != JSMN_STRING) {
		return false;
	}

	length = tok->end - tok->start;

	//Don't have enough buffer
	if (length > MAX_JSON_VALUE_LENGTH) {
		return false;
	}

	memset(outBuffer, '\0', MAX_JSON_VALUE_LENGTH); //Wipe previous value
	strncpy(outBuffer, json + tok->start, length);
	return true;
}

// GET /api/uartcmd?cmd=<command>
// Sends "<command>\r\n" on the UART (set up by uartInit) and returns the next
// reply line from the device on the other end (e.g. the WL5 PY32) as plain text,
// or "TIMEOUT". Synchronous request/response. Requires "startDriver UARTBridge".
static int http_rest_get_uartcmd(http_request_t* request) {
	char cmd[128];
	char reply[128];

	http_setup(request, httpMimeTypeText);
	if (!http_getArg(request->url, "cmd", cmd, sizeof(cmd))) {
		poststr(request, "ERROR: missing cmd argument, use /api/uartcmd?cmd=...");
		poststr(request, NULL);
		return 0;
	}
	if (UARTBridge_SendCommandAndWait(cmd, reply, sizeof(reply), 1000)) {
		poststr(request, reply);
	} else {
		poststr(request, "TIMEOUT");
	}
	poststr(request, NULL);
	return 0;
}

// GET /raw  — direct/raw control page with two tabs: "Valot" (per-channel
// sliders, output mode, raw PWM freq, raw RF map) and "Sync" (PY32 sync-input
// config). The MiBoxer-style perceptual UI lives at /wl5 (http_rest_get_wl5).
// Self-contained HTML; its JS calls /api/uartcmd on this SAME host, so there is
// no cross-origin (CORS) issue. Uses only single quotes / no backslashes so the
// C string literals stay clean. Commands map to the PY32 protocol:
// W/C/B/G/R:<0..31>, ALL:n, ON, OFF, CCT, IR, STATUS?, SYNC:..., SAVE.
static int http_rest_get_raw(http_request_t* request) {
	http_setup(request, httpMimeTypeHTML);
	poststr(request,
		"<!DOCTYPE html><html lang='fi'><head>"
		"<meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1'>"
		"<title>WL5</title><style>"
		"*{box-sizing:border-box}"
		"body{margin:0 auto;max-width:480px;padding:16px;font-family:system-ui,sans-serif;background:#111;color:#eee}"
		"h1{font-size:1.2rem;display:flex;justify-content:space-between;align-items:center}"
		"h2{font-size:.95rem;color:#9c9;margin:16px 0 4px}"
		".tabs{display:flex;gap:8px;margin-bottom:14px}"
		".tab{flex:1;padding:.6rem;border:0;border-radius:8px;background:#222;color:#aaa}"
		".tab.act{background:#4caf50;color:#fff}"
		".pow{font-size:.9rem;padding:.5rem 1rem;border:0;border-radius:8px;color:#fff;background:#555}"
		".pow.on{background:#4caf50}"
		".ch{margin:14px 0}"
		".ch label{display:flex;justify-content:space-between;font-size:.95rem;margin-bottom:4px}"
		"input[type=range]{width:100%;height:28px;accent-color:#4caf50}"
		".presets{display:flex;gap:8px;margin-top:18px;flex-wrap:wrap}"
		".presets button{flex:1;padding:.7rem;border:0;border-radius:8px;background:#333;color:#eee}"
		".row{display:flex;justify-content:space-between;align-items:center;margin:10px 0}"
		".seg{display:flex;gap:6px}"
		".sg{padding:.4rem .7rem;border:0;border-radius:6px;background:#333;color:#ccc}"
		".sg.act{background:#4caf50;color:#fff}"
		"input[type=number]{width:90px;background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:.3rem}"
		"#save{margin-top:16px;width:100%;padding:.8rem;border:0;border-radius:8px;background:#37e;color:#fff}"
		"#stat{font-size:.75rem;color:#888;margin-top:14px;min-height:1em}"
		"</style></head><body>");
	poststr(request,
		"<div class='tabs'><button id='tabL' class='tab act'>Valot</button>"
		"<button id='tabS' class='tab'>Sync</button></div>"
		"<div id='vL'>"
		"<h1>Valot <button id='pow' class='pow'>-</button></h1>"
		"<div class='row'><span>Moodi</span><span class='seg' id='gMode'></span></div>"
		"<button id='bRaw' class='sg' style='width:100%;margin-bottom:10px'>Raw (piilotettu)</button>"
		"<div class='row'><span>Nappi</span><span class='seg' id='gBtn'></span></div>"
		"<div id='chs'></div>"
		"<h2>Napin presetit</h2>"
		"<div style='font-size:.78rem;color:#888;margin-bottom:4px'>"
		"nappi-tila 'Preset': nopea klikkaus (alle 1 s) vaihtaa seuraavaan, tauon "
		"jalkeen painallus sammuttaa</div>"
		"<div class='row'><span>Maara</span><span class='seg' id='gPn'></span></div>"
		"<div id='plist'></div>"
		"<div class='presets'>"
		"<button id='bFull'>Taysi</button>"
		"<button id='bWhite'>Valkea</button>"
		"<button id='bWarm'>Lammin</button>"
		"<button id='bOff'>Pois</button>"
		"</div>"
		"<h2>Raw PWM taajuus</h2>"
		"<select id='fSel' style='width:100%;padding:.55rem;background:#222;color:#eee;border:1px solid #444;border-radius:6px'>"
		"<option value='250'>250 Hz</option><option value='500'>500 Hz</option>"
		"<option value='1000'>1 kHz</option><option value='2000'>2 kHz</option>"
		"<option value='4000'>4 kHz</option><option value='8000'>8 kHz</option>"
		"<option value='16000'>16 kHz</option><option value='31250'>31 kHz</option>"
		"<option value='62500'>62.5 kHz</option><option value='125000'>125 kHz</option>"
		"<option value='250000'>250 kHz</option><option value='500000'>500 kHz</option></select>"
		"<div style='font-size:.78rem;color:#9c9;margin-top:6px' id='freqHz'>-</div>"
		"<h2>Raw RF-mappays (kauko -> kanava)</h2>"
		"<div style='font-size:.78rem;color:#888;margin-bottom:4px'>milight-kauko ajaa naita kanavia raw-moodissa</div>"
		"<div id='rmap'></div>"
		"</div>"
		"<div id='vS' style='display:none'>"
		"<h1>Synkronointi</h1>"
		"<div class='row'><span>Pinni</span><span class='seg' id='gPin'></span></div>"
		"<div class='row'><span>Pull</span><span class='seg' id='gPull'></span></div>"
		"<div class='row'><span>Reuna</span><span class='seg' id='gEdge'></span></div>"
		"<div class='row'><span>Pulssit</span><input type='number' id='pulses' min='0' max='4294967295'></div>"
		"<h2>Skene 0 (lepo)</h2><div id='s0'></div>"
		"<h2>Skene 1 (aktiivinen)</h2><div id='s1'></div>"
		"<button id='save'>Tallenna pysyvasti</button>"
		"</div><div id='stat'></div>");
	poststr(request,
		"<script>"
		"var MAX=31;"
		"var CH=[['W','W Valkoinen'],['C','C Cool'],['B','Sininen'],['G','Vihrea'],['R','Punainen']];"
		"var powOn=true,curTab='L',gPin,gPull,gEdge;"
		"function setStat(t){document.getElementById('stat').textContent=t;}"
		// Serial request queue: OpenBeken serializes concurrent /api/uartcmd
		// with escalating delay (5 in parallel ~0.1/1/3/7/15 s), so never let two
		// overlap. q advances past failures (q=p.catch) so one error can't poison
		// the chain; the caller still gets the real promise p.
		"var q=Promise.resolve();"
		"function api(cmd){var p=q.then(function(){"
		"return fetch('/api/uartcmd?cmd='+encodeURIComponent(cmd),{cache:'no-store'})"
		".then(function(r){return r.text();});});q=p.catch(function(){});return p;}"
		// Default post-send refresh: light (only STATUS reflects a value/power/
		// preset change). Controls that change other state pass their own 'after'.
		"function defAfter(){if(curTab==='L'){statusRefresh();}else{syncRefresh();}}"
		"function send(cmd,after){setStat('-> '+cmd);"
		"api(cmd).then(function(t){setStat(cmd+' -> '+t);(after||defAfter)();})"
		".catch(function(e){setStat('virhe: '+e);});}"
		"function mkSlider(host,k,name,onCh){"
		"var d=document.createElement('div');d.className='ch';"
		"var lab=document.createElement('label');var sp=document.createElement('span');"
		"sp.textContent='0';lab.textContent=name+' ';lab.appendChild(sp);"
		"var r=document.createElement('input');r.type='range';r.min='0';r.max=MAX;r.value='0';"
		"d.appendChild(lab);d.appendChild(r);host.appendChild(d);"
		"r.addEventListener('input',function(){sp.textContent=r.value;});"
		"r.addEventListener('change',onCh);return r;}");
	poststr(request,
		"var chs=document.getElementById('chs');"
		"CH.forEach(function(c){var k=c[0];var r=mkSlider(chs,k,c[1],function(){send(k+':'+r.value);});r.id='r'+k;});"
		"function togglePow(){send(powOn?'OFF':'ON');}"
		// Full refresh (tab switch to Valot / initial load). Every read goes
		// through api() so the queue serializes them automatically — no manual
		// chaining, and no overlap with other requests.
		"function statusRefresh(){return api('STATUS?').then(applyStatus).catch(function(){});}"
		"function refresh(){statusRefresh();modeRefresh();freqRefresh();btnRefresh();rawmapRefresh();presetRefresh();}"
		"function applyStatus(t){t.split(' ').forEach(function(tok){"
		"var p=tok.split('=');if(p.length!==2)return;var k=p[0],val=p[1];"
		"if(k==='ON'){powOn=(val==='1');var b=document.getElementById('pow');"
		"b.textContent=powOn?'ON':'OFF';b.className=powOn?'pow on':'pow';return;}"
		"var r=document.getElementById('r'+k);if(r){r.value=val;"
		"r.previousSibling.lastChild.textContent=val;}});}"
		"document.getElementById('pow').addEventListener('click',togglePow);"
		"document.getElementById('bFull').addEventListener('click',function(){send('ALL:31');});"
		"document.getElementById('bWhite').addEventListener('click',function(){send('CCT');});"
		"document.getElementById('bWarm').addEventListener('click',function(){send('IR');});"
		"document.getElementById('bOff').addEventListener('click',function(){send('OFF');});"
		"var gMode=document.getElementById('gMode');"
		// Mode change shifts MAX/channel count/freq — do a full refresh.
		"seg(gMode,[['single','Single'],['dualwhite','DualW'],['rgb','RGB'],['rgbw','RGBW'],['rgbcct','RGB+CCT']],function(v){return 'OUTPUT:'+v;},refresh);"
		"document.getElementById('bRaw').addEventListener('click',function(){send('OUTPUT:raw',refresh);});"
		"var gBtn=document.getElementById('gBtn');"
		"seg(gBtn,[['reset','Reset'],['mode','Moodi'],['onoff','On/off'],['preset','Preset']],function(v){return 'BTN:FUNC:'+v;},btnRefresh);"
		"function btnRefresh(){return api('BTN?')"
		".then(function(t){t.split(' ').forEach(function(tok){"
		"var p=tok.split('=');if(p[0]==='FUNC'){segAct(gBtn,p[1]);}});}).catch(function(){});}"
		"function setMax(mx){if(mx>0){MAX=mx;CH.forEach(function(c){var r=document.getElementById('r'+c[0]);if(r){r.max=MAX;}});}}"
		"function modeRefresh(){return api('OUTPUT?')"
		".then(function(t){var p=t.split(' ');var m=p[1];"
		"segAct(gMode,m);document.getElementById('bRaw').className=(m==='raw')?'sg act':'sg';"
		"if(p[2]){setMax(parseInt(p[2].split('=')[1],10));}}).catch(function(){});}"
		"var fSel=document.getElementById('fSel');"
		"function freqToPP(f){var total=Math.round(16000000/f);var pre=0;"
		"while((total/(pre+1))>65536){pre++;}var per=Math.round(total/(pre+1))-1;"
		"if(per<1){per=1;}if(per>65535){per=65535;}return[per,pre];}"
		"fSel.addEventListener('change',function(){var pp=freqToPP(parseInt(fSel.value,10));"
		"send('FREQ:'+pp[0]+':'+pp[1],function(){freqRefresh();modeRefresh();});});"
		"function freqRefresh(){return api('FREQ?')"
		".then(function(t){var v=t.split(' ')[1];if(!v){return;}"
		"var a=v.split(':');var per=parseInt(a[0],10),pre=parseInt(a[1],10);"
		"var hz=Math.round(16000000/((per+1)*(pre+1)));var steps=Math.min(per+1,256);"
		"document.getElementById('freqHz').textContent=hz+' Hz, '+steps+' askelta';"
		"var o=fSel.options;for(var i=0;i<o.length;i++){if(parseInt(o[i].value,10)===hz){fSel.selectedIndex=i;break;}}"
		"}).catch(function(){});}");
	poststr(request,
		"var RM=[['warm','Lammin'],['cool','Cool'],['red','Punainen'],['green','Vihrea'],['blue','Sininen']];"
		"var rmap=document.getElementById('rmap');"
		"RM.forEach(function(o){var role=o[0];"
		"var row=document.createElement('div');row.className='row';"
		"var sp=document.createElement('span');sp.textContent=o[1];"
		"var sel=document.createElement('select');sel.id='rm_'+role;"
		"sel.style.cssText='background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:.35rem';"
		"[['off','-'],['W','W'],['C','C'],['B','B'],['G','G'],['R','R']].forEach(function(c){"
		"var op=document.createElement('option');op.value=c[0];op.textContent=c[1];sel.appendChild(op);});"
		"sel.addEventListener('change',function(){send('RAWMAP:'+role+':'+sel.value,rawmapRefresh);});"
		"row.appendChild(sp);row.appendChild(sel);rmap.appendChild(row);});"
		"function rawmapRefresh(){return api('RAWMAP?')"
		".then(function(t){t.split(' ').forEach(function(tok){"
		"var p=tok.split('=');if(p.length!==2)return;var s=document.getElementById('rm_'+p[0]);"
		"if(s){s.value=p[1];}});}).catch(function(){});}");
	// Button presets: how many are in the click cycle, plus one row per preset
	// (its channel values, "show it now" = BTN:PSEL, "store the current light"
	// = BTN:PSAVE). The scenes come from PRESET?, not BTN? — see the WL5 repo.
	poststr(request,
		"var gPn=document.getElementById('gPn');"
		"seg(gPn,[['1','1'],['2','2'],['3','3'],['4','4'],['5','5']],function(v){return 'BTN:PRESETS:'+v;},presetRefresh);"
		"var plist=document.getElementById('plist');"
		"for(var pi=1;pi<=5;pi++){(function(n){"
		"var row=document.createElement('div');row.className='row';"
		"var sp=document.createElement('span');sp.id='pv'+n;sp.style.fontSize='.88rem';sp.textContent='P'+n;"
		"var g=document.createElement('span');g.className='seg';"
		"var b1=document.createElement('button');b1.className='sg';b1.textContent='Nayta';"
		"b1.addEventListener('click',function(){send('BTN:PSEL:'+n,function(){statusRefresh();presetRefresh();});});"
		"var b2=document.createElement('button');b2.className='sg';b2.textContent='Tallenna';"
		"b2.addEventListener('click',function(){send('BTN:PSAVE:'+n,presetRefresh);});"
		"g.appendChild(b1);g.appendChild(b2);row.appendChild(sp);row.appendChild(g);plist.appendChild(row);"
		"})(pi);}"
		"function presetRefresh(){return api('PRESET?').then(function(t){"
		"var cnt=5,cur=0;"
		"t.split(' ').forEach(function(tok){var p=tok.split('=');if(p.length!==2)return;"
		"if(p[0]==='COUNT'){cnt=parseInt(p[1],10);segAct(gPn,p[1]);}"
		"else if(p[0]==='CUR'){cur=parseInt(p[1],10);}"
		"else if(p[0].charAt(0)==='P'){var n=parseInt(p[0].slice(1),10);"
		"var e=document.getElementById('pv'+n);if(e){e.textContent='P'+n+'  '+p[1];}}});"
		"for(var i=1;i<=5;i++){var e=document.getElementById('pv'+i);if(!e)continue;"
		"e.style.color=(i===cur)?'#4caf50':((i<=cnt)?'#eee':'#666');}"
		"}).catch(function(){});}");
	poststr(request,
		"function seg(host,opts,fn,after){host.innerHTML='';opts.forEach(function(o){"
		"var b=document.createElement('button');b.className='sg';b.textContent=o[1];b.dataset.v=o[0];"
		"b.addEventListener('click',function(){send(fn(o[0]),after);});host.appendChild(b);});}"
		"function segAct(host,v){Array.prototype.forEach.call(host.children,function(b){"
		"b.className=(b.dataset.v===String(v))?'sg act':'sg';});}"
		"function sendScene(host,idx){var a=[];Array.prototype.forEach.call(host.querySelectorAll('input'),"
		"function(r){a.push(r.value);});send('SYNC:'+idx+':'+a.join(','));}"
		"function buildScene(host,idx){CH.forEach(function(c){"
		"mkSlider(host,c[0],c[1],function(){sendScene(host,idx);});});}"
		"function fillScene(host,csv){var a=csv.split(',');var rows=host.children;"
		"for(var i=0;i<rows.length&&i<a.length;i++){var r=rows[i].querySelector('input');"
		"var s=rows[i].querySelector('span');if(r){r.value=a[i];}if(s){s.textContent=a[i];}}}");
	poststr(request,
		"gPin=document.getElementById('gPin');gPull=document.getElementById('gPull');gEdge=document.getElementById('gEdge');"
		"seg(gPin,[['off','Off'],['14','PA14'],['12','PA12']],function(v){return 'SYNC:PIN:'+v;});"
		"seg(gPull,[['none','None'],['up','Up'],['down','Down']],function(v){return 'SYNC:PULL:'+v;});"
		"seg(gEdge,[['rising','Nouseva'],['falling','Laskeva'],['both','Molemmat']],function(v){return 'SYNC:EDGE:'+v;});"
		"buildScene(document.getElementById('s0'),0);buildScene(document.getElementById('s1'),1);"
		"document.getElementById('pulses').addEventListener('change',function(){send('SYNC:PULSES:'+this.value);});"
		"document.getElementById('save').addEventListener('click',function(){send('SAVE');});"
		"function syncRefresh(){return api('SYNC?').then(applySync).catch(function(){});}"
		"function applySync(t){var PU={N:'none',U:'up',D:'down'},ED={R:'rising',F:'falling',B:'both'};"
		"t.split(' ').forEach(function(tok){var p=tok.split('=');if(p.length!==2)return;var k=p[0],v=p[1];"
		"if(k==='pin'){segAct(gPin,v==='off'?'off':v.replace('PA',''));}"
		"else if(k==='pull'){segAct(gPull,PU[v]);}"
		"else if(k==='edge'){segAct(gEdge,ED[v]);}"
		"else if(k==='pulses'){document.getElementById('pulses').value=v;}"
		"else if(k==='s0'){fillScene(document.getElementById('s0'),v);}"
		"else if(k==='s1'){fillScene(document.getElementById('s1'),v);}});}"
		"function showTab(x){curTab=x;var L=(x==='L');"
		"document.getElementById('vL').style.display=L?'':'none';"
		"document.getElementById('vS').style.display=L?'none':'';"
		"document.getElementById('tabL').className=L?'tab act':'tab';"
		"document.getElementById('tabS').className=L?'tab':'tab act';"
		"if(L){refresh();}else{syncRefresh();}}"
		"document.getElementById('tabL').addEventListener('click',function(){showTab('L');});"
		"document.getElementById('tabS').addEventListener('click',function(){showTab('S');});"
		"refresh();"
		"</script></body></html>");
	poststr(request, NULL);
	return 0;
}

// GET /wl5  — MiBoxer-style perceptual control page. A hue ring (canvas) with a
// centre power button, plus Saturation / Kelvin / Brightness sliders, adapting to
// the PY32 output mode (single/dualwhite/rgb/rgbw/rgbcct). Maps to the perceptual
// protocol: HUE:0-359, SAT:0-100, KELVIN:0-100 (0=warm,100=cool), BRI:0-100,
// ON/OFF; reads OUTPUT?/BULB?/STATUS? on load. Self-contained, single quotes only,
// no backslashes. The raw/channel UI lives at /raw.
static int http_rest_get_wl5(http_request_t* request) {
	http_setup(request, httpMimeTypeHTML);
	poststr(request,
		"<!DOCTYPE html><html lang='fi'><head>"
		"<meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
		"<title>WL5</title><style>"
		"*{box-sizing:border-box}"
		"body{margin:0 auto;max-width:480px;padding:14px;font-family:system-ui,sans-serif;background:#fff;color:#222}"
		"h1{font-size:1.05rem;display:flex;justify-content:space-between;align-items:center;margin:.2rem 0 .6rem}"
		".lab{font-size:.92rem;color:#555;margin:16px 0 5px}"
		".wrap{position:relative;width:300px;height:300px;margin:4px auto}"
		"canvas{display:block;touch-action:none}"
		"#pw{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);width:43%;height:43%;"
		"border-radius:50%;border:0;background:#fff;box-shadow:0 1px 7px rgba(0,0,0,.28);"
		"display:flex;align-items:center;justify-content:center;cursor:pointer}"
		"#th{position:absolute;width:28px;height:28px;border-radius:50%;background:#fff;border:2px solid #e0e0e0;"
		"box-shadow:0 1px 5px rgba(0,0,0,.45);transform:translate(-50%,-50%);pointer-events:none}"
		"input[type=range]{width:100%;height:26px;-webkit-appearance:none;appearance:none;border-radius:13px;outline:0;margin:0}"
		"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:28px;height:28px;border-radius:50%;"
		"background:#fff;border:1px solid #bbb;box-shadow:0 1px 4px rgba(0,0,0,.4)}"
		"input[type=range]::-moz-range-thumb{width:26px;height:26px;border-radius:50%;background:#fff;border:1px solid #bbb}"
		".wbtn{width:100%;padding:.85rem;border:0;border-radius:10px;background:#2196f3;color:#fff;font-size:1rem;margin-top:16px;cursor:pointer}"
		".foot{margin-top:20px;text-align:center}.foot a{color:#999;font-size:.85rem;text-decoration:none}"
		"#note{display:none;background:#fff3cd;color:#7a5b00;padding:.7rem;border-radius:8px;font-size:.9rem;margin:8px 0}"
		"</style></head><body>");
	poststr(request,
		"<h1><span id='hl'>WL5</span>"
		"<button id='pwTxt' style='font-size:.8rem;padding:.35rem .8rem;border:0;border-radius:7px;background:#eee;color:#333'>-</button>"
		"</h1>"
		"<div id='note'>Laite on raw-tilassa - perceptuaalinen ohjaus ei toimi. "
		"<a href='/raw'>Avaa raw-sivu</a></div>"
		"<div class='wrap' id='wrap'>"
		"<canvas id='wh' width='300' height='300'></canvas>"
		"<button id='pw' aria-label='power'>"
		"<svg id='pwi' viewBox='0 0 24 24' width='40' height='40' fill='none' stroke='#e53935' stroke-width='2.4' stroke-linecap='round'>"
		"<line x1='12' y1='3' x2='12' y2='12'/><path d='M6.4 7.1 a8 8 0 1 0 11.2 0'/></svg>"
		"</button>"
		"<div id='th'></div>"
		"</div>"
		"<div id='satRow'><div class='lab' id='sl'>Saturation</div>"
		"<input type='range' id='eSat' min='0' max='100' value='100'></div>"
		"<div id='kelRow'><div class='lab' id='kll'>Kelvin</div>"
		"<input type='range' id='eKel' min='0' max='100' value='50'></div>"
		"<div id='briRow'><div class='lab' id='bl'>Brightness</div>"
		"<input type='range' id='eBri' min='0' max='100' value='100'></div>"
		"<button class='wbtn' id='wbtn'>White Light</button>"
		"<div class='lab'>Presetit</div>"
		"<div id='pres' style='display:flex;gap:8px;flex-wrap:wrap'></div>"
		"<label style='display:flex;align-items:center;gap:8px;margin-top:10px;font-size:.82rem;color:#666'>"
		"<input type='checkbox' id='pmode' style='width:18px;height:18px'>"
		"Tallenna nykyinen valo seuraavaksi painettuun presettiin</label>"
		"<div class='foot'><a href='/raw'>Raw / lisaasetukset</a></div>");
	poststr(request,
		"<script>"
		"var SZ=300,C=150,RO=144,RI=88;"
		"var hue=0,bri=100,on=true,mode='rgbcct';"
		// Serial request queue (OBK serializes concurrent /api/uartcmd; never overlap).
		"var q=Promise.resolve();"
		"function api(cmd){var p=q.then(function(){"
		"return fetch('/api/uartcmd?cmd='+encodeURIComponent(cmd),{cache:'no-store'})"
		".then(function(r){return r.text();});});q=p.catch(function(){});return p;}"
		// Leading+trailing throttle per key so dragging doesn't flood the queue.
		"var tmr={},lastc={};"
		"function tsend(k,cmd){lastc[k]=cmd;if(tmr[k]){return;}api(cmd);"
		"tmr[k]=setTimeout(function(){tmr[k]=null;if(lastc[k]!==cmd){api(lastc[k]);}},120);}"
		"function $(i){return document.getElementById(i);}"
		"var cv=$('wh'),ctx=cv.getContext('2d');"
		"function drawRing(){ctx.clearRect(0,0,SZ,SZ);for(var a=0;a<360;a++){"
		"var a0=(a-91.5)*Math.PI/180,a1=(a-88)*Math.PI/180;"
		"ctx.beginPath();ctx.moveTo(C,C);ctx.arc(C,C,RO,a0,a1);ctx.closePath();"
		"ctx.fillStyle='hsl('+a+',100%,50%)';ctx.fill();}"
		"ctx.beginPath();ctx.arc(C,C,RI,0,6.2832);ctx.fillStyle='#fff';ctx.fill();}"
		"function hueCol(){return 'hsl('+hue+',100%,50%)';}"
		"function moveThumb(){var rr=(RO+RI)/2,ang=(hue-90)*Math.PI/180;"
		"$('th').style.left=(C+rr*Math.cos(ang))+'px';$('th').style.top=(C+rr*Math.sin(ang))+'px';}"
		"function paintSat(){$('eSat').style.background='linear-gradient(90deg,#fff,'+hueCol()+')';}"
		"function paintBri(){$('eBri').style.background='linear-gradient(90deg,#000,#fff)';}"
		"function paintKel(){$('eKel').style.background='linear-gradient(90deg,#ffb16e,#fff,#bcd9ff)';}"
		"function hueLab(){$('hl').textContent='RGB:'+hue;}"
		"function satLab(){$('sl').textContent='Saturation:'+$('eSat').value;}"
		"function briLab(){$('bl').textContent='Brightness:'+$('eBri').value;}"
		"function kelLab(){var k=Math.round((2700+(+$('eKel').value)*38)/100)*100;$('kll').textContent='Kelvin:'+k+'K';}"
		"function paintPow(){$('pwi').setAttribute('stroke',on?'#e53935':'#c9c9c9');"
		"$('pwTxt').textContent=on?'ON':'OFF';$('pwTxt').style.background=on?'#d7f0d7':'#eee';}");
	poststr(request,
		"function rel(e){var r=cv.getBoundingClientRect();var t=(e.touches&&e.touches[0])||e;"
		"return{x:t.clientX-r.left-C,y:t.clientY-r.top-C};}"
		"var drag=false;"
		"function rdown(e){var p=rel(e);var d=Math.sqrt(p.x*p.x+p.y*p.y);"
		"if(d<RI||d>RO+12){return;}drag=true;rmove(e);}"
		"function rmove(e){if(!drag){return;}var p=rel(e);"
		"var ang=Math.atan2(p.y,p.x)*180/Math.PI;hue=Math.round(ang+90+360)%360;"
		"hueLab();moveThumb();paintSat();tsend('h','HUE:'+hue);"
		"if(e.cancelable){e.preventDefault();}}"
		"function rup(){drag=false;}"
		"cv.addEventListener('mousedown',rdown);window.addEventListener('mousemove',rmove);window.addEventListener('mouseup',rup);"
		"cv.addEventListener('touchstart',rdown,{passive:false});"
		"cv.addEventListener('touchmove',rmove,{passive:false});cv.addEventListener('touchend',rup);"
		"$('pw').addEventListener('click',function(){on=!on;paintPow();api(on?'ON':'OFF');});"
		"$('pwTxt').addEventListener('click',function(){on=!on;paintPow();api(on?'ON':'OFF');});"
		"$('eSat').addEventListener('input',function(){satLab();tsend('s','SAT:'+$('eSat').value);});"
		"$('eKel').addEventListener('input',function(){kelLab();tsend('k','KELVIN:'+$('eKel').value);});"
		"$('eBri').addEventListener('input',function(){bri=+$('eBri').value;briLab();tsend('b','BRI:'+$('eBri').value);});"
		"$('wbtn').addEventListener('click',function(){api('KELVIN:'+$('eKel').value);});");
	poststr(request,
		"function show(id,v){$(id).style.display=v?'':'none';}"
		"function applyMode(m){mode=m;"
		"var raw=(m==='raw');show('note',raw);"
		"var col=(m==='rgb'||m==='rgbw'||m==='rgbcct');"
		"var kel=(m==='dualwhite'||m==='rgbcct');"
		"var wl=(m==='rgbw'||m==='rgbcct');"
		"show('wrap',col&&!raw);show('satRow',col&&!raw);show('kelRow',kel&&!raw);"
		"show('briRow',!raw);show('wbtn',wl&&!raw);}"
		"function parseBulb(t){t.split(' ').forEach(function(tok){var p=tok.split('=');"
		"if(p.length!==2){return;}var k=p[0],v=+p[1];"
		"if(k==='bri'){bri=v;$('eBri').value=v;briLab();}"
		"else if(k==='hue'){hue=v;hueLab();moveThumb();paintSat();}"
		"else if(k==='sat'){$('eSat').value=v;satLab();}"
		"else if(k==='kelvin'){$('eKel').value=v;kelLab();}});}"
		// Presets are raw channel scenes, so they work in every output mode —
		// including raw, where the rest of this page is hidden. Tapping one is
		// BTN:PSEL (which also switches the light on); with the checkbox ticked it
		// is BTN:PSAVE instead, storing the light as it looks right now.
		"function presLoad(){return api('PRESET?').then(function(t){"
		"var cnt=5,cur=0;"
		"t.split(' ').forEach(function(tok){var p=tok.split('=');if(p.length!==2){return;}"
		"if(p[0]==='COUNT'){cnt=parseInt(p[1],10);}else if(p[0]==='CUR'){cur=parseInt(p[1],10);}});"
		"var h=$('pres');h.innerHTML='';"
		"for(var i=1;i<=cnt;i++){(function(n){"
		"var b=document.createElement('button');b.textContent=n;"
		"b.style.cssText='flex:1;min-width:52px;padding:.75rem;border:0;border-radius:10px;"
		"font-size:1rem;cursor:pointer;'+((n===cur)?'background:#2196f3;color:#fff':'background:#eee;color:#333');"
		"b.addEventListener('click',function(){"
		"if($('pmode').checked){$('pmode').checked=false;api('BTN:PSAVE:'+n).then(presLoad);}"
		"else{api('BTN:PSEL:'+n).then(function(){on=true;paintPow();presLoad();});}});"
		"h.appendChild(b);})(i);}"
		"}).catch(function(){});}"
		"function load(){"
		"api('OUTPUT?').then(function(t){applyMode((t.split(' ')[1]||'rgbcct').trim());});"
		"api('BULB?').then(parseBulb);"
		"api('STATUS?').then(function(t){t.split(' ').forEach(function(tok){"
		"if(tok.indexOf('ON=')===0){on=tok.slice(3)==='1';paintPow();}});});"
		"presLoad();}"
		"drawRing();moveThumb();paintSat();paintKel();paintBri();paintPow();"
		"satLab();kelLab();briLab();hueLab();load();"
		"</script></body></html>");
	poststr(request, NULL);
	return 0;
}

static int http_rest_get(http_request_t* request) {
	ADDLOG_DEBUG(LOG_FEATURE_API, "GET of %s", request->url);

	if (!strncmp(request->url, "api/uartcmd", 11)) {
		return http_rest_get_uartcmd(request);
	}

	if (!strcmp(request->url, "api/channels")) {
		return http_rest_get_channels(request);
	}

	if (!strcmp(request->url, "api/pins")) {
		return http_rest_get_pins(request);
	}
	if (!strcmp(request->url, "api/channelTypes")) {
		return http_rest_get_channelTypes(request);
	}
	if (!strcmp(request->url, "api/logconfig")) {
		return http_rest_get_logconfig(request);
	}

	if (!strncmp(request->url, "api/seriallog", 13)) {
		return http_rest_get_seriallog(request);
	}

#if ENABLE_LITTLEFS
	if (!strcmp(request->url, "api/fsblock")) {
		uint32_t newsize = CFG_GetLFS_Size();
		uint32_t newstart = (LFS_BLOCKS_END - newsize);

		newsize = (newsize / LFS_BLOCK_SIZE) * LFS_BLOCK_SIZE;

		// double check again that we're within bounds - don't want
		// boot overwrite or anything nasty....
		if (newstart < LFS_BLOCKS_START_MIN) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}
		if ((newstart + newsize > LFS_BLOCKS_END) ||
			(newstart + newsize < LFS_BLOCKS_START_MIN)) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}

		return http_rest_get_flash(request, newstart, newsize);
	}
#endif

#if ENABLE_LITTLEFS
	if (!strncmp(request->url, "api/lfs/", 8)) {
		return http_rest_get_lfs_file(request);
	}
	if (!strncmp(request->url, "api/run/", 8)) {
		return http_rest_run_lfs_file(request);
	}
	if (!strncmp(request->url, "api/del/", 8)) {
		return http_rest_get_lfs_delete(request);
	}
#endif

	if (!strcmp(request->url, "api/info")) {
		return http_rest_get_info(request);
	}
#if ENABLE_BT_PROXY
	if (!strcmp(request->url, "api/bt_scan")) {
		return http_rest_get_bt_scan(request);
	}
#endif

	if (!strncmp(request->url, "api/flash/", 10)) {
		return http_rest_get_flash_advanced(request);
	}

	http_setup(request, httpMimeTypeHTML);
	http_html_start(request, "GET REST API");
	poststr(request, "GET of ");
	poststr(request, request->url);
	http_html_end(request);
	poststr(request, NULL);
	return 0;
}

// POST /api/py32ota  — body is a raw PY32 application image (for 0x08002000).
// Buffers it, then drives the bootloader OTA over UART (FW:UPDATE + transfer).
static int http_rest_post_py32ota(http_request_t* request) {
	char msg[80];
	int total = request->contentLength;
	int got, n;
	uint8_t* img;

	if (total <= 0 || total > 20 * 1024) {
		http_setup(request, httpMimeTypeText);
		hprintf255(request, "ERR: bad Content-Length %d (max 20480)", total);
		poststr(request, NULL);
		return 0;
	}
	img = (uint8_t*)os_malloc(total);
	if (!img) {
		http_setup(request, httpMimeTypeText);
		poststr(request, "ERR: out of memory");
		poststr(request, NULL);
		return 0;
	}
	/* read the whole image body first (bodystart holds the first chunk) */
	got = 0;
	n = request->bodylen;
	if (n > total) n = total;
	if (n > 0) { memcpy(img, request->bodystart, n); got = n; }
	while (got < total) {
		int r = recv(request->fd, (char*)img + got, total - got, 0);
		if (r <= 0) break;
		got += r;
	}
	http_setup(request, httpMimeTypeText);
	if (got != total) {
		os_free(img);
		hprintf255(request, "ERR: received %d of %d bytes", got, total);
		poststr(request, NULL);
		return 0;
	}
	UARTBridge_PushFirmware(img, (uint32_t)total, msg, sizeof(msg));
	os_free(img);
	poststr(request, msg);
	poststr(request, NULL);
	return 0;
}

static int http_rest_post(http_request_t* request) {
	char tmp[20];
	ADDLOG_DEBUG(LOG_FEATURE_API, "POST to %s", request->url);

	if (!strcmp(request->url, "api/py32ota")) {
		return http_rest_post_py32ota(request);
	}

	if (!strcmp(request->url, "api/channels")) {
		return http_rest_post_channels(request);
	}
	
	if (!strcmp(request->url, "api/pins")) {
		return http_rest_post_pins(request);
	}
	if (!strcmp(request->url, "api/channelTypes")) {
		return http_rest_post_channelTypes(request);
	}
	if (!strcmp(request->url, "api/logconfig")) {
		return http_rest_post_logconfig(request);
	}

	if (!strcmp(request->url, "api/reboot")) {
		return http_rest_post_reboot(request);
	}
	if (!strcmp(request->url, "api/ota")) {
		OTA_IncrementProgress(1);
#if ENABLE_BT_PROXY
		HAL_BTProxy_StopScan();
#endif
		int r = 0;
#if PLATFORM_BEKEN
		r = http_rest_post_flash(request, START_ADR_OF_BK_PARTITION_OTA, LFS_BLOCKS_END);
#elif PLATFORM_W600
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_W800
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_BL602 || PLATFORM_BL_NEW
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_LN882H || PLATFORM_LN8825
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_ESPIDF || PLATFORM_ESP8266
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_REALTEK
		r = http_rest_post_flash(request, 0, -1);
#elif PLATFORM_ECR6600 || PLATFORM_TR6260
		r = http_rest_post_flash(request, -1, -1);
#elif PLATFORM_XRADIO && !PLATFORM_XR809
		r = http_rest_post_flash(request, 0, -1);
#elif PLATFORM_TXW81X
		r = http_rest_post_flash(request, 0, -1);
#elif PLATFORM_RDA5981
		r = http_rest_post_flash(request, 0, -1);
#elif PLATFORM_GD32VW553
		r = http_rest_post_flash(request, 0, -1);
#else
		// TODO
		ADDLOG_ERROR(LOG_FEATURE_API, "No OTA");
#endif
		OTA_ResetProgress();
		return r;
	}
	if (!strncmp(request->url, "api/flash/", 10)) {
		return http_rest_post_flash_advanced(request);
	}

	if (!strcmp(request->url, "api/cmnd")) {
		return http_rest_post_cmd(request);
	}


#if ENABLE_LITTLEFS
	if (!strcmp(request->url, "api/fsblock")) {
		if (lfs_present()) {
			release_lfs();
		}
		uint32_t newsize = CFG_GetLFS_Size();
		uint32_t newstart = (LFS_BLOCKS_END - newsize);

		newsize = (newsize / LFS_BLOCK_SIZE) * LFS_BLOCK_SIZE;

		// double check again that we're within bounds - don't want
		// boot overwrite or anything nasty....
		if (newstart < LFS_BLOCKS_START_MIN) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}
		if ((newstart + newsize > LFS_BLOCKS_END) ||
			(newstart + newsize < LFS_BLOCKS_START_MIN)) {
			return http_rest_error(request, -20, "LFS Size mismatch");
		}

		// we are writing the lfs block
		int res = http_rest_post_flash(request, newstart, LFS_BLOCKS_END);
		// initialise the filesystem, it should be there now.
		// don't create if it does not mount
		init_lfs(0);
		return res;
	}
	if (!strncmp(request->url, "api/lfs/", 8)) {
		return http_rest_post_lfs_file(request);
	}
#endif

	http_setup(request, httpMimeTypeHTML);
	http_html_start(request, "POST REST API");
	poststr(request, "POST to ");
	poststr(request, request->url);
	poststr(request, "<br/>Content Length:");
	sprintf(tmp, "%d", request->contentLength);
	poststr(request, tmp);
	poststr(request, "<br/>Content:[");
	poststr(request, request->bodystart);
	poststr(request, "]<br/>");
	http_html_end(request);
	poststr(request, NULL);
	return 0;
}

static int http_rest_app(http_request_t* request) {
	const char* webhost = CFG_GetWebappRoot();
//	const char* ourip = HAL_GetMyIPString(); //CFG_GetOurIP();
	http_setup(request, httpMimeTypeHTML);
//	if (webhost && ourip) {
// we don't need to rely on any function here for our IP.
// If this code is used, someone is accessing the webif, so we
// know our ip (and port) inside the browser (JS "location").
// Knowing/using the port from location.host is very usefull e.g. in simulator ;-) 
	if (webhost) {
		poststr(request, htmlDoctype);

		poststr(request, "<head><title>");
		poststr(request, CFG_GetDeviceName());
		poststr(request, "</title>");

		poststr(request, htmlShortcutIcon);
		poststr(request, htmlHeadMeta);
		hprintf255(request, "<script>var root='%s',device='http://'+location.host;</script>", webhost);
		hprintf255(request, "<script src='%s/startup.js'></script>", webhost);
		poststr(request, "</head><body></body></html>");
	}
	else {
		http_html_start(request, "Not available");
		poststr(request, htmlFooterReturnToMainPage);
		poststr(request, "no APP available<br/>");
		http_html_end(request);
	}
	poststr(request, NULL);
	return 0;
}

#if ENABLE_LITTLEFS

int EndsWith(const char* str, const char* suffix)
{
	if (!str || !suffix)
		return 0;
	size_t lenstr = strlen(str);
	size_t lensuffix = strlen(suffix);
	if (lensuffix > lenstr)
		return 0;
	return strncmp(str + lenstr - lensuffix, suffix, lensuffix) == 0;
}
char *my_memmem(const char *haystack, int haystack_len, const char *needle, int needle_len) {
	if (needle_len == 0 || haystack_len < needle_len)
		return NULL;

	for (int i = 0; i <= haystack_len - needle_len; i++) {
		if (memcmp(haystack + i, needle, needle_len) == 0)
			return (char *)(haystack + i);
	}
	return NULL;
}
typedef struct berryBuilder_s {

	char berry_buffer[4096];
	int berry_len;
} berryBuilder_t;

void BB_Start(berryBuilder_t *b)
{
	b->berry_buffer[0] = 0;
	b->berry_len = 0;
}
void BB_AddCode(berryBuilder_t *b, const char *start, const char *end) {
	int len;
	if (end) {
		len = end - start;
	}
	else {
		len = strlen(start);
	}
	memcpy(&b->berry_buffer[b->berry_len], start, len);
	b->berry_len += len;
}
void BB_AddText(berryBuilder_t *b, const char *fname, const char *start, const char *end) {
	BB_AddCode(b, " echo(\"",0);
#if 0
	BB_AddCode(b, start, end);
#else
	const char *p = start;
	const char *limit = end ? end : (start + strlen(start));
	while (p < limit) {
		char c = *p++;
		switch (c) {
		case '\\': BB_AddCode(b, "\\\\", 0); break;
		case '\"': BB_AddCode(b, "\\\"", 0); break;
		case '\n': BB_AddCode(b, "\\n", 0); break;
		case '\r': BB_AddCode(b, "\\r", 0); break;
		case '\t': BB_AddCode(b, "\\t", 0); break;
		default:
			BB_AddCode(b, &c, &c + 1);
			break;
		}
	}

#endif
	BB_AddCode(b, "\")", 0);
}
void eval_berry_snippet(const char *s);
void Berry_SaveRequest(http_request_t *r);
void BB_Run(berryBuilder_t *b)
{
	b->berry_buffer[b->berry_len] = 0;
	eval_berry_snippet(b->berry_buffer);
}
int http_runBerryFile(http_request_t *request, const char *fname) {
	Berry_SaveRequest(request);
	berryBuilder_t bb;
	BB_Start(&bb);
	char *data = (char*)LFS_ReadFile(fname);
	if (data == 0)
		return 0;
	http_setup(request, httpMimeTypeHTML);
	char *p = data;
	while (*p) {
		char *btag = strstr(p, "<?b");
		if (!btag) {
			break;
		}
		BB_AddText(&bb, fname, p, btag);
		char *etag = strstr(btag, "?>");

		BB_AddCode(&bb, btag + 3, etag);

		p = etag + 2;
	}
	const char *s = p;
	while (*p)
		p++;
	BB_AddText(&bb, fname, s, p);
	free(data);
	BB_Run(&bb);
	return 1;
}
static int http_rest_run_lfs_file(http_request_t* request) {
	char* fpath;
	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}
#if ENABLE_OBK_BERRY
	const char* base = request->url + strlen("api/lfs/");
	const char* q = strchr(base, '?');
	size_t len = q ? (size_t)(q - base) : strlen(base);
	fpath = os_malloc(len + 1);
	memcpy(fpath, base, len);
	fpath[len] = '\0';
	int ran = http_runBerryFile(request, fpath);
	if (ran==0) 
#endif
	{
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}
	free(fpath);
	return 0;
}

static int http_rest_get_lfs_file(http_request_t* request) {
	char* fpath;
	char* buff;
	int len;
	int lfsres;
	int total = 0;
	lfs_file_t* file;
	char *args;
	bool isGzip;

	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/lfs/") + 1);

	buff = os_malloc(1024);
	file = os_malloc(sizeof(lfs_file_t));
	memset(file, 0, sizeof(lfs_file_t));

	strcpy(fpath, request->url + strlen("api/lfs/"));

	// strip HTTP args with ?
	args = strchr(fpath, '?');
	if (args) {
		*args = 0;
	}

	isGzip = EndsWith(fpath, "gz");

	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS read of %s", fpath);
	lfsres = lfs_file_open(&lfs, file, fpath, LFS_O_RDONLY);

	if (lfsres == -21) {
		lfs_dir_t* dir;
		ADDLOG_DEBUG(LOG_FEATURE_API, "%s is a folder", fpath);
		dir = os_malloc(sizeof(lfs_dir_t));
		memset(dir, 0, sizeof(*dir));
		// if the thing is a folder.
		lfsres = lfs_dir_open(&lfs, dir, fpath);

		if (lfsres >= 0) {
			// this is needed during iteration...?
			struct lfs_info info;
			int count = 0;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "opened folder %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"dir\":\"%s\",\"content\":[", fpath);
			do {
				// Read an entry in the directory
				//
				// Fills out the info structure, based on the specified file or directory.
				// Returns a positive value on success, 0 at the end of directory,
				// or a negative error code on failure.
				lfsres = lfs_dir_read(&lfs, dir, &info);
				if (lfsres > 0) {
					if (count) poststr(request, ",");
					hprintf255(request, "{\"name\":\"%s\",\"type\":%d,\"size\":%d}",
						info.name, info.type, info.size);
				}
				else {
					if (lfsres < 0) {
						if (count) poststr(request, ",");
						hprintf255(request, "{\"error\":%d}", lfsres);
					}
				}
				count++;
			} while (lfsres > 0);

			hprintf255(request, "]}");

			lfs_dir_close(&lfs, dir);
			if (dir) os_free(dir);
			dir = NULL;
		}
		else {
			if (dir) os_free(dir);
			dir = NULL;
			request->responseCode = HTTP_RESPONSE_NOT_FOUND;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
		}
	}
	else {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS open [%s] gives %d", fpath, lfsres);
		if (lfsres >= 0) {
			char* ext = fpath;
			const char *mimetype = httpMimeTypeBinary;

			if (isGzip) {
				// find original extension (e.g., .js from .js.gz)
				char* dot = strrchr(fpath, '.');
				if (dot) {
					*dot = '\0'; // temporarily strip .gz
					if (EndsWith(fpath, ".js")) {
						mimetype = httpMimeTypeJavascript;
					}
					else if (EndsWith(fpath, ".html")) {
						mimetype = httpMimeTypeHTML;
					}
					else if (EndsWith(fpath, ".css")) {
						mimetype = httpMimeTypeCSS;
					}
					else if (EndsWith(fpath, ".json")) {
						mimetype = httpMimeTypeJson;
					}
					else if (EndsWith(fpath, ".ico")) {
						mimetype = "image/x-icon";
					}
					*dot = '.'; // restore .gz
				}
			}
			else {
				if (EndsWith(fpath, ".js") || EndsWith(fpath, ".vue")) {
					mimetype = httpMimeTypeJavascript;
				}
				else if (EndsWith(fpath, ".json")) {
					mimetype = httpMimeTypeJson;
				}
				else if (EndsWith(fpath, ".html")) {
					mimetype = httpMimeTypeHTML;
				}
				else if (EndsWith(fpath, ".css")) {
					mimetype = httpMimeTypeCSS;
				}
				else if (EndsWith(fpath, ".ico")) {
					mimetype = "image/x-icon";
				}
			}

			if (isGzip) {
				http_setup_gz(request, mimetype);
			}
			else {
				http_setup(request, mimetype);
			}
			//#if ENABLE_OBK_BERRY
			//			http_runBerryFile(request, fpath);
			//#else
			do {
				len = lfs_file_read(&lfs, file, buff, 1024);
				total += len;
				if (len) {
					//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes read", len);
					postany(request, buff, len);
				}
			} while (len > 0);
			//#endif
			lfs_file_close(&lfs, file);
			ADDLOG_DEBUG(LOG_FEATURE_API, "%d total bytes read", total);
		}
		else {
			request->responseCode = HTTP_RESPONSE_NOT_FOUND;
			http_setup(request, httpMimeTypeJson);
			ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s lfs result %d", fpath, lfsres);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
		}
	}
	poststr(request, NULL);
	if (fpath) os_free(fpath);
	if (file) os_free(file);
	if (buff) os_free(buff);
	return 0;
}
bool HTTP_checkLFSOverride(http_request_t* request, const char *ext) {
	char tmp[64];
	//sprintf_s(tmp, sizeof(tmp), "override/%s", request->url);
	//sprintf_s(tmp, sizeof(tmp), "%s%s", request->url, ext);
	strcpy_safe(tmp, request->url, sizeof(tmp));
	strcat_safe(tmp, ext, sizeof(tmp));
	char *fix = strchr(tmp, '?');
	if (fix) {
		*fix = 0;
	}
	lfs_file_t* file;
	file = os_malloc(sizeof(lfs_file_t));
	memset(file,0, sizeof(lfs_file_t));
	int lfsres = lfs_file_open(&lfs, file, tmp, LFS_O_RDONLY);
	if (lfsres == 0) {
		lfs_file_close(&lfs, file);
		free(file);
		strcpy_safe(tmp, "api/lfs/", sizeof(tmp));
		strcat_safe(tmp, request->url, sizeof(tmp));
		strcat_safe(tmp, ext, sizeof(tmp));
		char *oldURL = request->url;
		request->url = tmp;
		http_rest_get_lfs_file(request);
		request->url = oldURL;
		// "api/lfs/", 8)) {
		// "api/run/", 8)) {
		return 1;
	}
	free(file);
	return 0;
}
static int http_rest_get_lfs_delete(http_request_t* request) {
	char* fpath;
	int lfsres;

	// don't start LFS just because we're trying to read a file -
	// it won't exist anyway
	if (!lfs_present()) {
		request->responseCode = HTTP_RESPONSE_NOT_FOUND;
		http_setup(request, httpMimeTypeText);
		poststr(request, "Not found");
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/del/") + 1);

	strcpy(fpath, request->url + strlen("api/del/"));

	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s", fpath);
	lfsres = lfs_remove(&lfs, fpath);

	if (lfsres == LFS_ERR_OK) {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s OK", fpath);

		poststr(request, "OK");
	}
	else {
		ADDLOG_DEBUG(LOG_FEATURE_API, "LFS delete of %s error %i", fpath, lfsres);
		poststr(request, "Error");
	}
	poststr(request, NULL);
	if (fpath) os_free(fpath);
	return 0;
}

static int http_rest_post_lfs_file(http_request_t* request) {
	int len;
	int lfsres;
	int total = 0;
	int loops = 0;

	// allocated variables
	lfs_file_t* file;
	char* fpath;
	char* folder;

	// create if it does not exist
	init_lfs(1);

	if (!lfs_present()) {
		request->responseCode = 400;
		http_setup(request, httpMimeTypeText);
		poststr(request, "LittleFS is not available");
		poststr(request, NULL);
		return 0;
	}

	fpath = os_malloc(strlen(request->url) - strlen("api/lfs/") + 1);
	file = os_malloc(sizeof(lfs_file_t));
	memset(file, 0, sizeof(lfs_file_t));

	strcpy(fpath, request->url + strlen("api/lfs/"));
	ADDLOG_DEBUG(LOG_FEATURE_API, "LFS write of %s len %d", fpath, request->contentLength);

	folder = strchr(fpath, '/');
	if (folder) {
		int folderlen = folder - fpath;
		folder = os_malloc(folderlen + 1);
		strncpy(folder, fpath, folderlen);
		folder[folderlen] = 0;
		ADDLOG_DEBUG(LOG_FEATURE_API, "file is in folder %s try to create", folder);
		lfsres = lfs_mkdir(&lfs, folder);
		if (lfsres < 0) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "mkdir error %d", lfsres);
		}
	}

	//ADDLOG_DEBUG(LOG_FEATURE_API, "LFS write of %s len %d", fpath, request->contentLength);

	lfsres = lfs_file_open(&lfs, file, fpath, LFS_O_RDWR | LFS_O_CREAT);
	if (lfsres >= 0) {
		//ADDLOG_DEBUG(LOG_FEATURE_API, "opened %s");
		int towrite = request->bodylen;
		char* writebuf = request->bodystart;
		int writelen = request->bodylen;
		if (request->contentLength >= 0) {
			towrite = request->contentLength;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "bodylen %d, contentlen %d", request->bodylen, request->contentLength);

		if (writelen < 0) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "ABORTED: %d bytes to write", writelen);
			lfs_file_close(&lfs, file);
			request->responseCode = HTTP_RESPONSE_SERVER_ERROR;
			http_setup(request, httpMimeTypeJson);
			hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, -20);
			goto exit;
		}

		do {
			loops++;
#if ENABLE_LFS_SPI
			if (loops > 50) {
				loops = 0;
				rtos_delay_milliseconds(1);
			}
#else
			if (loops > 10) {
				loops = 0;
				rtos_delay_milliseconds(10);
			}
#endif
			//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes to write", writelen);
			len = lfs_file_write(&lfs, file, writebuf, writelen);
			if (len < 0) {
				ADDLOG_ERROR(LOG_FEATURE_API, "Failed to write to %s with error %i", fpath,len);
				break;
			}
			total += len;
			if (len > 0) {
				//ADDLOG_DEBUG(LOG_FEATURE_API, "%d bytes written", len);
			}
			towrite -= len;
			if (towrite > 0) {
				writebuf = request->received;
				writelen = recv(request->fd, writebuf, request->receivedLenmax, 0);
				if (writelen < 0) {
					ADDLOG_DEBUG(LOG_FEATURE_API, "recv returned %d - end of data - remaining %d", writelen, towrite);
				}
			}
		} while ((towrite > 0) && (writelen >= 0));

		// no more data
		lfs_file_truncate(&lfs, file, total);

		//ADDLOG_DEBUG(LOG_FEATURE_API, "closing %s", fpath);
		lfs_file_close(&lfs, file);
		ADDLOG_DEBUG(LOG_FEATURE_API, "%d total bytes written", total);
		http_setup(request, httpMimeTypeJson);
		hprintf255(request, "{\"fname\":\"%s\",\"size\":%d}", fpath, total);
	}
	else {
		request->responseCode = HTTP_RESPONSE_SERVER_ERROR;
		http_setup(request, httpMimeTypeJson);
		ADDLOG_DEBUG(LOG_FEATURE_API, "failed to open %s err %d", fpath, lfsres);
		hprintf255(request, "{\"fname\":\"%s\",\"error\":%d}", fpath, lfsres);
	}
exit:
	poststr(request, NULL);
	if (folder) os_free(folder);
	if (file) os_free(file);
	if (fpath) os_free(fpath);
	return 0;
}

// static int http_favicon(http_request_t* request) {
// 	request->url = "api/lfs/favicon.ico";
// 	return http_rest_get_lfs_file(request);
// }

#else
// static int http_favicon(http_request_t* request) {
// 	request->responseCode = HTTP_RESPONSE_NOT_FOUND;
// 	http_setup(request, httpMimeTypeHTML);
// 	poststr(request, NULL);
// 	return 0;
// }
#endif



static int http_rest_get_seriallog(http_request_t* request) {
	if (request->url[strlen(request->url) - 1] == '1') {
		direct_serial_log = 1;
	}
	else {
		direct_serial_log = 0;
	}
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "Direct serial logging set to %d", direct_serial_log);
	poststr(request, NULL);
	return 0;
}



static int http_rest_get_pins(http_request_t* request) {
	int i;
	int maxNonZero;
	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"rolenames\":[");
	for (i = 0; i < IOR_Total_Options; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "\"%s\"", htmlPinRoleNames[i]);
	}
	poststr(request, "],\"roles\":[");

	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", g_cfg.pins.roles[i]);
	}
	// TODO: maybe we should cull futher channels that are not used?
	// I support many channels because I plan to use 16x relays module with I2C MCP23017 driver

	// find max non-zero ch
	//maxNonZero = -1;
	//for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
	//	if (g_cfg.pins.channels[i] != 0) {
	//		maxNonZero = i;
	//	}
	//}

	poststr(request, "],\"channels\":[");
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", g_cfg.pins.channels[i]);
	}
	// find max non-zero ch2
	maxNonZero = -1;	
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (g_cfg.pins.channels2[i] != 0) {
			maxNonZero = i;
		}
	}
	if (maxNonZero != -1) {
		poststr(request, "],\"channels2\":[");
		for (i = 0; i <= maxNonZero; i++) {
			if (i) {
				hprintf255(request, ",");
			}
			hprintf255(request, "%d", g_cfg.pins.channels2[i]);
		}
	}
	poststr(request, "],\"states\":[");
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		if (i) {
			hprintf255(request, ",");
		}
		hprintf255(request, "%d", CHANNEL_Get(g_cfg.pins.channels[i]));
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}


static int http_rest_get_channelTypes(http_request_t* request) {
	int i;

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{\"typenames\":[");
	for (i = 0; i < ChType_Max; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", g_channelTypeNames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", g_channelTypeNames[i]);
		}
	}
	poststr(request, "],\"types\":[");

	for (i = 0; i < CHANNEL_MAX; i++) {
		if (i) {
			hprintf255(request, ",%d", g_cfg.pins.channelTypes[i]);
		}
		else {
			hprintf255(request, "%d", g_cfg.pins.channelTypes[i]);
		}
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}



////////////////////////////
// log config
static int http_rest_get_logconfig(http_request_t* request) {
	int i;
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"level\":%d,", g_loglevel);
	hprintf255(request, "\"features\":%d,", logfeatures);
	poststr(request, "\"levelnames\":[");
	for (i = 0; i < LOG_MAX; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", loglevelnames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", loglevelnames[i]);
		}
	}
	poststr(request, "],\"featurenames\":[");
	for (i = 0; i < LOG_FEATURE_MAX; i++) {
		if (i) {
			hprintf255(request, ",\"%s\"", logfeaturenames[i]);
		}
		else {
			hprintf255(request, "\"%s\"", logfeaturenames[i]);
		}
	}
	poststr(request, "]}");
	poststr(request, NULL);
	return 0;
}

static int http_rest_post_logconfig(http_request_t* request) {
	int i;
	int r;
	char tmp[64];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	http_setup(request, httpMimeTypeText);
	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * 128);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		poststr(request, NULL);
		os_free(p);
		os_free(t);
		return 0;
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		poststr(request, NULL);
		os_free(p);
		os_free(t);
		return 0;
	}

	//sprintf(tmp,"parsed JSON: %s\n", json_str);
	//poststr(request, tmp);
	//poststr(request, NULL);

		/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (jsoneq(json_str, &t[i], "level") == 0) {
			if (t[i + 1].type != JSMN_PRIMITIVE) {
				continue; /* We expect groups to be an array of strings */
			}
			g_loglevel = atoi(json_str + t[i + 1].start);
			i += t[i + 1].size + 1;
		}
		else if (jsoneq(json_str, &t[i], "features") == 0) {
			if (t[i + 1].type != JSMN_PRIMITIVE) {
				continue; /* We expect groups to be an array of strings */
			}
			logfeatures = atoi(json_str + t[i + 1].start);;
			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
			snprintf(tmp, sizeof(tmp), "Unexpected key: %.*s\n", t[i].end - t[i].start,
				json_str + t[i].start);
			poststr(request, tmp);
		}
	}

	poststr(request, NULL);
	os_free(p);
	os_free(t);
	return 0;
}

/////////////////////////////////////////////////


static int http_rest_get_info(http_request_t* request) {
	char macstr[3 * 6 + 1];
	long int* pAllGenericFlags = (long int*)&g_cfg.genericFlags;

	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"uptime_s\":%d,", g_secondsElapsed);
	hprintf255(request, "\"build\":\"%s\",", g_build_str);
	hprintf255(request, "\"ip\":\"%s\",", HAL_GetMyIPString());
	hprintf255(request, "\"mac\":\"%s\",", HAL_GetMACStr(macstr));
	hprintf255(request, "\"flags\":\"%ld\",", *pAllGenericFlags);
	hprintf255(request, "\"mqtthost\":\"%s:%d\",", CFG_GetMQTTHost(), CFG_GetMQTTPort());
	hprintf255(request, "\"mqtttopic\":\"%s\",", CFG_GetMQTTClientId());
	hprintf255(request, "\"chipset\":\"%s\",", PLATFORM_MCU_NAME);
	hprintf255(request, "\"webapp\":\"%s\",", CFG_GetWebappRoot());
	hprintf255(request, "\"shortName\":\"%s\",", CFG_GetShortDeviceName());
	poststr(request, "\"startcmd\":\"");
	// This can be longer than 255
	poststr_escapedForJSON(request, CFG_GetShortStartupCommand());
	poststr(request, "\",");
#ifndef OBK_DISABLE_ALL_DRIVERS
	hprintf255(request, "\"supportsSSDP\":%d,", DRV_IsRunning("SSDP") ? 1 : 0);
#else
	hprintf255(request, "\"supportsSSDP\":0,");
#endif

	hprintf255(request, "\"supportsClientDeviceDB\":true}");

	poststr(request, NULL);
	return 0;
}

#if ENABLE_BT_PROXY
static int http_rest_get_bt_scan(http_request_t* request) {
	int init_done = 0;
	int scan_active = 0;
	int total_packets = 0;
	int dropped_packets = 0;

	HAL_BTProxy_GetScanStats(&init_done, &scan_active, &total_packets, &dropped_packets);
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"init\":%d,\"scan\":%d,\"total\":%d,\"dropped\":%d}",
		init_done, scan_active, total_packets, dropped_packets);
	poststr(request, NULL);
	return 0;
}
#endif

static int http_rest_post_pins(http_request_t* request) {
	int i;
	int r;
	char tmp[64];
	int iChanged = 0;
	char tokenStrValue[MAX_JSON_VALUE_LENGTH + 1];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * TOKEN_COUNT);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (tryGetTokenString(json_str, &t[i], tokenStrValue) != true) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "Parsing failed");
			continue;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "parsed %s", tokenStrValue);

		if (strcmp(tokenStrValue, "roles") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int roleval, pr;
				jsmntok_t* g = &t[i + j + 2];
				roleval = atoi(json_str + g->start);
				pr = PIN_GetPinRoleForPinIndex(j);
				if (pr != roleval) {
					PIN_SetPinRoleForPinIndex(j, roleval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "channels") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int chanval, pr;
				jsmntok_t* g = &t[i + j + 2];
				chanval = atoi(json_str + g->start);
				pr = PIN_GetPinChannelForPinIndex(j);
				if (pr != chanval) {
					PIN_SetPinChannelForPinIndex(j, chanval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "deviceFlag") == 0) {
			int flag;
			jsmntok_t* flagTok = &t[i + 1];
			if (flagTok == NULL || flagTok->type != JSMN_PRIMITIVE) {
				continue;
			}

			flag = atoi(json_str + flagTok->start);
			ADDLOG_DEBUG(LOG_FEATURE_API, "received deviceFlag %d", flag);

			if (flag >= 0 && flag <= 10) {
				CFG_SetFlag(flag, true);
				iChanged++;
			}

			i += t[i + 1].size + 1;
		}
		else if (strcmp(tokenStrValue, "deviceCommand") == 0) {
			if (tryGetTokenString(json_str, &t[i + 1], tokenStrValue) == true) {
				ADDLOG_DEBUG(LOG_FEATURE_API, "received deviceCommand %s", tokenStrValue);
				CFG_SetShortStartupCommand_AndExecuteNow(tokenStrValue);
				iChanged++;
			}

			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
		}
	}
	if (iChanged) {
		CFG_Save_SetupTimer();
		ADDLOG_DEBUG(LOG_FEATURE_API, "Changed %d - saved to flash", iChanged);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
}

static int http_rest_post_channelTypes(http_request_t* request) {
	int i;
	int r;
	char tmp[64];
	int iChanged = 0;
	char tokenStrValue[MAX_JSON_VALUE_LENGTH + 1];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * TOKEN_COUNT);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_OBJECT) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Object expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		if (tryGetTokenString(json_str, &t[i], tokenStrValue) != true) {
			ADDLOG_DEBUG(LOG_FEATURE_API, "Parsing failed");
			continue;
		}
		//ADDLOG_DEBUG(LOG_FEATURE_API, "parsed %s", tokenStrValue);

		if (strcmp(tokenStrValue, "types") == 0) {
			int j;
			if (t[i + 1].type != JSMN_ARRAY) {
				continue; /* We expect groups to be an array of strings */
			}
			for (j = 0; j < t[i + 1].size; j++) {
				int typeval, pr;
				jsmntok_t* g = &t[i + j + 2];
				typeval = atoi(json_str + g->start);
				pr = CHANNEL_GetType(j);
				if (pr != typeval) {
					CHANNEL_SetType(j, typeval);
					iChanged++;
				}
			}
			i += t[i + 1].size + 1;
		}
		else {
			ADDLOG_ERROR(LOG_FEATURE_API, "Unexpected key: %.*s", t[i].end - t[i].start,
				json_str + t[i].start);
		}
	}
	if (iChanged) {
		CFG_Save_SetupTimer();
		ADDLOG_DEBUG(LOG_FEATURE_API, "Changed %d - saved to flash", iChanged);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
}

int http_rest_error(http_request_t* request, int code, char* msg) {
	request->responseCode = code;
	http_setup(request, httpMimeTypeJson);
	if (code != 200) {
		hprintf255(request, "{\"error\":%d, \"msg\":\"%s\"}", code, msg);
	}
	else {
		hprintf255(request, "{\"success\":%d, \"msg\":\"%s\"}", code, msg);
	}
	poststr(request, NULL);
	return 0;
}

static int http_rest_post_reboot(http_request_t* request) {
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"reboot\":%d}", 3);
	ADDLOG_DEBUG(LOG_FEATURE_API, "Rebooting in 3 seconds...");
	RESET_ScheduleModuleReset(3);
	poststr(request, NULL);
	return 0;
}

static int http_rest_get_flash_advanced(http_request_t* request) {
	char* params = request->url + 10;
	int startaddr = 0;
	int len = 0;
	int sres;
	sres = sscanf(params, "%x-%x", &startaddr, &len);
	if (sres == 2) {
		return http_rest_get_flash(request, startaddr, len);
	}
	return http_rest_error(request, -1, "invalid url");
}

static int http_rest_post_flash_advanced(http_request_t* request) {
	char* params = request->url + 10;
	int startaddr = 0;
	int sres;
	sres = sscanf(params, "%x", &startaddr);
	if (sres == 1 && startaddr >= START_ADR_OF_BK_PARTITION_OTA) {
		// allow up to end of flash
		return http_rest_post_flash(request, startaddr, 0x200000);
	}
	return http_rest_error(request, -1, "invalid url");
}

static int http_rest_get_flash(http_request_t* request, int startaddr, int len) {
	char* buffer;
	int res;

	if (startaddr < 0 || (startaddr + len > DEFAULT_FLASH_LEN)) {
		return http_rest_error(request, -1, "requested flash read out of range");
	}

	int bufferSize = 1024;
	buffer = os_malloc(bufferSize);
	memset(buffer, 0, bufferSize);

	http_setup(request, httpMimeTypeBinary);
	while (len) {
		int readlen = len;
		if (readlen > 1024) {
			readlen = 1024;
		}
		res = HAL_FlashRead(buffer, readlen, startaddr);
		startaddr += readlen;
		len -= readlen;
		postany(request, buffer, readlen);
	}
	poststr(request, NULL);
	os_free(buffer);
	return 0;
}

static int http_rest_get_channels(http_request_t* request) {
	int i;
	int addcomma = 0;

	http_setup(request, httpMimeTypeJson);
	poststr(request, "{");

	// TODO: maybe we should cull futher channels that are not used?
	// I support many channels because I plan to use 16x relays module with I2C MCP23017 driver
	for (i = 0; i < PLATFORM_GPIO_MAX; i++) {
		// "i" is a pin index
		// Get channel index and role
		int ch = PIN_GetPinChannelForPinIndex(i);
		int role = PIN_GetPinRoleForPinIndex(i);
		if (role) {
			if (addcomma) {
				hprintf255(request, ",");
			}
			hprintf255(request, "\"%d\":%d", ch, CHANNEL_Get(ch));
			addcomma = 1;
		}
	}
	poststr(request, "}");
	poststr(request, NULL);
	return 0;
}

// currently crashes the MCU - maybe stack overflow?
static int http_rest_post_channels(http_request_t* request) {
	int i;
	int r;
	char tmp[64];

	//https://github.com/zserge/jsmn/blob/master/example/simple.c
	//jsmn_parser p;
	jsmn_parser* p = os_malloc(sizeof(jsmn_parser));
	//jsmntok_t t[128]; /* We expect no more than 128 tokens */
#define TOKEN_COUNT 128
	jsmntok_t* t = os_malloc(sizeof(jsmntok_t) * TOKEN_COUNT);
	char* json_str = request->bodystart;
	int json_len = strlen(json_str);

	memset(p, 0, sizeof(jsmn_parser));
	memset(t, 0, sizeof(jsmntok_t) * 128);

	jsmn_init(p);
	r = jsmn_parse(p, json_str, json_len, t, TOKEN_COUNT);
	if (r < 0) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Failed to parse JSON: %d", r);
		sprintf(tmp, "Failed to parse JSON: %d\n", r);
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Assume the top-level element is an object */
	if (r < 1 || t[0].type != JSMN_ARRAY) {
		ADDLOG_ERROR(LOG_FEATURE_API, "Array expected", r);
		sprintf(tmp, "Object expected\n");
		os_free(p);
		os_free(t);
		return http_rest_error(request, 400, tmp);
	}

	/* Loop over all keys of the root object */
	for (i = 1; i < r; i++) {
		int chanval;
		jsmntok_t* g = &t[i];
		chanval = atoi(json_str + g->start);
		CHANNEL_Set(i - 1, chanval, 0);
		ADDLOG_DEBUG(LOG_FEATURE_API, "Set of chan %d to %d", i,
			chanval);
	}

	os_free(p);
	os_free(t);
	return http_rest_error(request, 200, "OK");
	return 0;
}



static int http_rest_post_cmd(http_request_t* request) {
	commandResult_t res;
	int code;
	const char *reply;
	const char *type;
	const char* cmd = request->bodystart;
	res = CMD_ExecuteCommand(cmd, COMMAND_FLAG_SOURCE_CONSOLE);
	reply = CMD_GetResultString(res);
	if (1) {
		addLogAdv(LOG_INFO, LOG_FEATURE_CMD, "[WebApp Cmd '%s' Result] %s", cmd, reply);
	}
	if (res != CMD_RES_OK) {
		type = "error";
		if (res == CMD_RES_UNKNOWN_COMMAND) {
			code = 501;
		}
		else {
			code = 400;
		}
	}
	else {
		type = "success";
		code = 200;
	}

	request->responseCode = code;
	http_setup(request, httpMimeTypeJson);
	hprintf255(request, "{\"%s\":%d, \"msg\":\"%s\", \"res\":", type, code, reply);
#if ENABLE_TASMOTA_JSON
	JSON_ProcessCommandReply(cmd, skipToNextWord(cmd), request, (jsonCb_t)hprintf255, COMMAND_FLAG_SOURCE_HTTP);
#endif
	hprintf255(request, "}", code, reply);
	poststr(request, NULL);
	return 0;
}

