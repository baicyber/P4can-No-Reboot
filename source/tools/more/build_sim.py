#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_sim.py  —  从 index_html.h 解压出 HTML，并生成一个可本地模拟运行的版本。

用法:
    python build_sim.py

产物 (同目录):
    p4can-embedded.html   index_html.h 中 gzip 载荷的逐字节还原（固件实际下发的页面）
    p4can-sim.html        在上面注入一个假 /api 后端，file:// 直接打开即可交互
"""
import gzip
import hashlib
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
FIRMWARE = os.path.join(REPO, "firmware", "P4can-unlock-more")
SRC = os.path.join(FIRMWARE, "index_html.h")
DASH = os.path.join(FIRMWARE, "dashboard_source.html")
OUTDIR = os.path.join(REPO, "sim")
os.makedirs(OUTDIR, exist_ok=True)


def extract():
    h = open(SRC, encoding="utf-8", errors="replace").read()
    m = re.search(r"INDEX_HTML_GZ\[\]\s*PROGMEM\s*=\s*\{(.*?)\};", h, re.S)
    if not m:
        raise SystemExit("index_html.h 里找不到 INDEX_HTML_GZ 数组")
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    decl = re.search(r"INDEX_HTML_GZ_LEN\s*=\s*(\d+)", h)
    raw = gzip.decompress(data)
    out = os.path.join(OUTDIR, "p4can-embedded.html")
    open(out, "wb").write(raw)
    print("gzip      : %d 字节%s" % (
        len(data), "" if not decl else "  (头文件声明 %s)" % decl.group(1)))
    print("解压后    : %d 字节" % len(raw))
    if os.path.exists(DASH):
        same = open(DASH, "rb").read() == raw
        print("与 dashboard_source.html 逐字节一致: %s" % same)
    print("sha256    : %s" % hashlib.sha256(raw).hexdigest())
    print("写出      : %s" % out)
    return raw


MOCK = r"""
<style>
#simFab{position:fixed;right:10px;top:10px;z-index:9999;font:700 10px/1 -apple-system,system-ui,sans-serif;
  padding:7px 10px;border-radius:999px;background:#111;color:#fff;letter-spacing:.08em;cursor:pointer;
  box-shadow:0 4px 14px rgba(0,0,0,.28);opacity:.82;user-select:none}
#simFab:hover{opacity:1}
#simPanel{position:fixed;right:10px;top:44px;z-index:9999;width:min(340px,calc(100vw - 20px));display:none;
  font:12px/1.5 -apple-system,system-ui,sans-serif;background:#fff;color:#111;border:1px solid #d5d7db;
  border-radius:12px;box-shadow:0 12px 34px rgba(0,0,0,.22);overflow:hidden}
#simPanel.on{display:block}
#simPanel .hd{display:flex;align-items:center;justify-content:space-between;padding:9px 11px;
  background:#f4f5f7;border-bottom:1px solid #e3e5e9;font-weight:750}
#simPanel .hd span{font-size:10px;font-weight:700;color:#6b7280;letter-spacing:.06em}
#simPanel .bd{padding:10px 11px;max-height:52vh;overflow:auto}
#simPanel .row{display:flex;align-items:center;gap:7px;margin-bottom:8px}
#simPanel .row input{margin:0}
#simPanel .row label{font-size:12px}
#simPanel .log{font:10px/1.55 ui-monospace,SFMono-Regular,Menlo,monospace;color:#374151;
  background:#f8f9fb;border:1px solid #eceef2;border-radius:8px;padding:7px 8px;max-height:200px;overflow:auto;
  white-space:pre-wrap;word-break:break-all}
#simPanel .log b{color:#0a7c3f;font-weight:700}
#simPanel button{font:600 11px/1 inherit;padding:6px 9px;border-radius:7px;border:1px solid #d5d7db;
  background:#fff;cursor:pointer;color:#111}
#simPanel button:hover{background:#f2f3f5}
#simPanel .hint{font-size:10.5px;color:#6b7280;margin:0 0 8px}
</style>
<div id="simFab">SIM</div>
<div id="simPanel">
  <div class="hd">Local simulation<span id="simExit">hide</span></div>
  <div class="bd">
    <div class="row"><input type="checkbox" id="simWired" checked><label for="simWired">非凡后端在线（<code>/api/feifan/status</code> 返回 200，与固件同契约）</label></div>
    <div class="row"><input type="checkbox" id="simModule"><label for="simModule">模拟车上装了 TSL 模块（默认不装，与无模块实机一致）</label></div>
    <p class="hint" id="simHint"></p>
    <p class="hint" id="simAct">最近动作：—</p>
    <div class="log" id="simLog">waiting for requests…</div>
    <div class="row" style="margin:8px 0 0"><button id="simClear">清空 localStorage</button><button id="simReload">重新加载</button></div>
  </div>
</div>
<script>
(function(){
  var LS='p4canFeifan', SIM={log:[],wired:true,cfg:{},
    lighting:{apForcedHighBeam:false,highBeamStrobe:false,rearFogStrobe:false,
      dynamicBrakeMode:0,running:false,apActive:true,stalkSupported:true,
      strobeRunning:false,apRunning:false,fogRunning:false,fogEdge:0,
      owner:0,ownerName:'NONE',txOk:0,txFail:0}};
  /* 中继 / 抓包：预览页也要有真读数，否则面板会把 undefined 当状态印出来。 */
  SIM.uplink={enabled:false,haveCfg:false,ssid:'',connected:false,napt:false,ip:'',rssi:0,attempts:0,upSec:0,stations:1,error:''};
  SIM.cap={on:false,cap:16384,total:0,rows:[]};
  var OTA={attempt:0,ok:false,error:'',stage:'idle',received:0,total:0,inProgress:false};
  try{var a=localStorage.getItem('simOtaAttempt');OTA.attempt=a?Number(a)||0:0;}catch(e){}
  try{SIM.cfg=JSON.parse(localStorage.getItem(LS)||'{}')||{};}catch(e){SIM.cfg={};}
  try{var w=localStorage.getItem('simFeifanWired');SIM.wired=(w===null?true:w==='1');}catch(e){}
  /* 用户实机没有外挂 TSL 模块，模拟默认也按「未安装」跑；勾上才验证模块分支。 */
  try{var hm=localStorage.getItem('simFeifanModule');SIM.hasModule=(hm==='1');}catch(e){SIM.hasModule=false;}

  function resp(obj,status){
    return new Response(JSON.stringify(obj==null?{}:obj),{
      status:status||200,statusText:status===404?'Not Found':'OK',
      headers:{'Content-Type':'application/json','Cache-Control':'no-store'}});
  }
  function qs(q,k){var m=new RegExp('[?&]'+k+'=([^&]*)').exec('?'+q);return m?decodeURIComponent(m[1]):null;}

  var NAG={enabled:false,method:0,mode:0,torque:22,burstMs:180,pauseMs:900,hoRatePct:80,
    targetId:'0x370',steeringId:'0x2B9',apStateId:'0x399',ignoreApState:false,pauseAtZeroSpeed:true,
    torqueRightScrollEnabled:false,torqueRightScrollIntervalSeconds:30,torqueRightScrollPattern:1,
    tsl9Sequence:0,tsl9Window:0,tsl9InputMode:0,tsl9LegacyRoute:0,tsl9LegacyRouteSelectable:false,
    tsl9RightPeriodicEnabled:false,tsl9RightPeriodicIntervalSeconds:30};

  var FEATURES={lab:true,s3xy:true,doorCancel:false,bannedSupported:false,banned:false,
    tlsscRestore:false,s3xySupported:true,apDriveProfile:false,regen:'stock',ok:true};

  function snapshot(q){
    var fast={
      nag:{torque:0.42,apActive:true,handsOnState:2,stoppedGate:false,stopCarrierActive:false,
           canAState:1,canTaskHeartbeatTimeoutCountA:0,canTaskHeartbeatTimeoutCountB:0,canTaskHeartbeatTimeoutCountBoth:0},
      blink:{enabled:true,displayState:'AUTOSTEER',dasState:1,displayStateValid:true,dasStateValid:true},
      summon:{canState:1,canStateName:'RUNNING'},
      cantraffic:{},
      r79:{}
    };
    for(var i=1;i<=3;i++){
      fast.cantraffic['can'+i+'State']=1;
      fast.cantraffic['can'+i+'Ready']=true;
      fast.cantraffic['can'+i+'TrafficSeen']=true;
      fast.cantraffic['can'+i+'TrafficOnline']=true;
      fast.cantraffic['can'+i+'TrafficAgeMs']=[96,180,340][i-1];
    }
    var out={fast:fast};
    if(qs(q,'slow')){
      out.slow={blink:{enabled:true},summon:{tlssc:true},s3xy:{bluetoothEnabled:true,pairedCount:2,connectedCount:2},
                r79:{},lab3f8:{alcMode:0}};
    }
    return out;
  }

  function route(path,q,method,init){
    /* ---- 非凡功能 ---- */
    /* 与固件 feifan_native.h / feifan_api.h 同一套契约：
       applied 只列页面上真实可用的 P4 原生项；模块项在无模块时回 stored/module-absent。
       q1x = SUPER_3CAN 移植项，固件自己发帧。 */
    /* 页面上只剩设置页没有的 7 项，全部是 SUPER_3CAN 移植来的原生帧。
       eapNr 是单独一个常驻开关（TLSSC 免重启），不参与 7 项分组计数。 */
    var FF_NATIVE=['q1a','q1b','q1c','q1d','q1e','q1f','q1g','eapNr'];
    var FF_NATIVE_MAP={};for(var ni=0;ni<FF_NATIVE.length;ni++)FF_NATIVE_MAP[FF_NATIVE[ni]]=1;
    /* 会话级原生项不进权威回显，与固件 feifanNativeConfigJson() 一致 */
    var FF_SESSION={};
    /* 走 TSL 模块配置块/键值的条目，与固件 tsl_map.h 同一份清单 */
    /* 页面上已移除所有模块位域项，模拟后端同样不再有需模块集合 */
    var FF_MOD={};
    SIM.module={enabled:SIM.hasModule,state:SIM.hasModule?'ready':'off',ready:SIM.hasModule,
      cfgValid:SIM.hasModule,cfgLen:SIM.hasModule?72:0,name:SIM.hasModule?'TSL-9':'',
      error:'',writesOk:0,writesFail:0,actionsSent:0,framesIn:0};
    if(path==='/api/feifan/status'){
      if(!SIM.wired)return resp({error:'not_found'},404);
      /* 模拟机默认三路 CAN 都在线，和真机上正常行驶时的状态一致。 */
      var co=SIM.canOnline||[1,1,1];
      var mc={},nc={};
      for(var mk in FF_MOD){ if(SIM.cfg[mk]!=null) mc[mk]=String(SIM.cfg[mk]); }
      /* 原生权威真值：会话级 3 项不回显，避免留下重启后失效的假状态 */
      for(var nk in FF_NATIVE_MAP){ if(!FF_SESSION[nk]) nc[nk]=(SIM.cfg[nk]!=null)?String(SIM.cfg[nk]):'0'; }
      return resp({ok:true,fw:'v3.29.3-sim',uptimeSec:1234,freeHeap:120000,profile:3,
        profileName:'Model Y',topologyName:'BODY + CHASSIS + PARTY',config:SIM.cfg,
        canOnline:co,canRx:SIM.canRx||[1834,922,417],canTxOk:[0,0,0],canTxFail:[0,0,0],
        applied:FF_NATIVE,nativeConfig:nc,moduleAbsent:!SIM.hasModule,
        s3Config:{q1a:!!SIM.cfg.q1a,q1b:!!SIM.cfg.q1b,q1c:false,q1d:!!SIM.cfg.q1d,
                  q1e:!!SIM.cfg.q1e,q1f:!!SIM.cfg.q1f,q1g:!!SIM.cfg.q1g,
                  gearState:4,speedKph:0,txOk:12,txFail:0,
                  strobeArm:false,stalkByte:0x03,stalkRx:128},
        serviceConfig:{txOk:SIM.cfg.svcSent?1:0,txFail:0,lastMs:SIM.cfg.svcSent?1234:0,lastErr:0,
                       confirmed:!!SIM.cfg.svcSent,confirmMs:SIM.cfg.svcSent?1500:0,rx284:0,rx339:0},
        /* 爆闪 / 强制远光：BODY 0x249 方波，和固件同一套字段 */
        strobe:{auth:!!SIM.cfg.q1a,armed:!!SIM.cfg.q1a,hold:!!SIM.cfg.q1b,tpl:true,synth:false,
                rx:413,ckBad:0,ctr:6,dir:0,byte1:0x06,pulls:7,frames:24,
                txOk:SIM.cfg.q1a?24:0,txFail:0},
        /* TLSSC 免重启：开关落 NVS，缓存帧按固件同一套语义回显 */
        eapNr:{enabled:!!SIM.cfg.eapNr,
               prereq:!!(SIM.cfg.banned&&SIM.cfg.tlRestore),
               banned:!!SIM.cfg.banned,restore:!!SIM.cfg.tlRestore,
               cacheValid:true,cacheLen:8,cacheHex:'1B00000000000000',
               stockRx:64,stockMs:1200,replayOk:SIM.cfg.eapNr?37:0,replayFail:0,replaySkip:0,
               burst:SIM.cfg.eapNr?37:0,lastMs:SIM.cfg.eapNr?1234:0,lastErr:0},
        module:SIM.module,moduleConfig:mc,
        writesOk:0,writesFail:0,actionsBlocked:(SIM.blocked||0),
        pending:{action:!SIM.hasModule,gauge:true,moduleBlob:!SIM.hasModule}});
    }
    if(path==='/api/feifan/gauge'){
      if(!SIM.wired)return resp({error:'not_found'},404);
      return resp({ok:false,pending:true,reason:'gauge-decode-pending'},503);
    }
    if(path==='/api/feifan/blob'){
      if(!SIM.wired)return resp({error:'not_found'},404);
      var nm=qs(q,'name');
      if(nm==='backup')return resp({ok:true,v:1,fw:'v3.29.3-sim',name:'backup',config:SIM.cfg});
      return resp({ok:true,name:nm||'quick',len:0,hex:'',pending:true,reason:'no-module-reply'});
    }
    if(path==='/api/feifan/module'){
      var mb={}; try{mb=JSON.parse(init.body||'{}')||{};}catch(e){}
      /* 与固件一致：GET 只查询，只有 POST 且显式带 on 才切换链路 */
      if(method==='POST'&&(mb.on!=null||qs(q,'on')!=null)){
        var on=mb.on!=null?mb.on:qs(q,'on');
        SIM.hasModule=(on===true||on===1||on==='1'||on==='true');
        try{localStorage.setItem('simFeifanModule',SIM.hasModule?'1':'0');}catch(e){}
        SIM.module.enabled=SIM.hasModule;
        SIM.module.ready=SIM.hasModule;
        SIM.module.state=SIM.hasModule?'ready':'off';
        SIM.module.cfgValid=SIM.hasModule;
        SIM.module.name=SIM.hasModule?'TSL-9':'';
      }
      return resp({ok:true,enabled:SIM.hasModule,state:SIM.module.state,ready:SIM.hasModule,
        cfgValid:SIM.hasModule,name:SIM.module.name,error:''});
    }
    if(path==='/api/feifan/config'){
      var b={},applied=false,via='',note='';
      try{b=JSON.parse(init.body||'{}')||{};}catch(e){}
      if(b.id!=null){
        SIM.cfg[b.id]=b.value;
        localStorage.setItem(LS,JSON.stringify(SIM.cfg));
      }
      if(FF_NATIVE_MAP[b.id]){applied=true;via='p4-native';note='native-updated';}
      else if(FF_MOD[b.id]&&SIM.hasModule&&SIM.module.ready){applied=true;via='module';note='module-block-written';}
      else if(FF_MOD[b.id]){note='module-absent';}
      else{note='stored-only';}
      return resp({ok:true,id:b.id,value:b.value,effect:applied?'applied':'stored',
        pending:!applied,note:note,via:via,
        sessionOnly:!!FF_SESSION[b.id],moduleAbsent:(!SIM.hasModule&&!!FF_MOD[b.id])});
    }
    if(path==='/api/feifan/action'){
      var act={};
      try{act=JSON.parse(init.body||'{}');}catch(e){}
      SIM.last={id:act.id,code:act.code};
      if(act.id==='q1c'){
        log('   ↳ q1c = service mode 0x339 data[5]=0x80 ×1  → sent (p4-native)');
        SIM.cfg.svcSent=true;
        return resp({ok:true,pending:false,reason:'service-frame-sent',id:'q1c',code:'',sentOnce:true,blockedTotal:SIM.blocked||0},200);
      }
      var nn=Number(act.code);
      var sent=!isNaN(nn)&&SIM.hasModule&&SIM.module.ready;
      if(!sent)SIM.blocked=(SIM.blocked||0)+1;
      log('   ↳ '+act.id+' = '+(act.code!=null?act.code:act.label||'—')+
          (isNaN(nn)?'':'  (0x'+nn.toString(16).toUpperCase()+')')+
          (sent?'  → sent via module':'  → 501 '+(SIM.hasModule?'module-offline':'module-absent')));
      return resp({ok:sent,pending:!sent,
        reason:sent?(FF_MOD[act.id]?'key-sent':'action-sent'):(SIM.hasModule?'module-not-ready':'module-absent'),
        module:SIM.module.state,id:act.id,code:act.code,blockedTotal:SIM.blocked},
        sent?200:501);
    }

    /* ---- 启动与配置 ---- */
    if(path==='/api/profile/status')return resp({setupMode:false,nvsError:false,migrationNotice:false,
      profile:3,topology:2,turn:1,topologyName:'Body + Chassis + Party',canB:'CHASSIS',
      nagSupported:true,nagTorqueSupported:true,nagTsl9Supported:true,
      advancedEapSupported:true,euUnlockSupported:true,pedalMapSupported:true});
    if(path==='/api/profile/select')return resp({ok:true});
    if(path==='/api/features/status')return resp(FEATURES);
    if(path.indexOf('/api/features/')===0){
      if(q){var kv=q.split('&');for(var i=0;i<kv.length;i++){var p=kv[i].split('=');
        if(p[0]==='enabled')FEATURES.lab=p[1]==='1';}}
      return resp(FEATURES);
    }

    /* ---- 遥测 ---- */
    if(path==='/api/snapshot')return resp(snapshot(q));
    if(path==='/api/nag/config')return resp(NAG);
    if(path==='/api/nag/mode'){NAG.mode=Number(qs(q,'m')||0);return resp(NAG);}
    if(path.indexOf('/api/nag/')===0)return resp(NAG);
    if(path==='/api/wifi/status')return resp({mode:'AP',ssid:'P4-CAN',rssi:-48,ip:'192.168.4.1',
      clients:1,sta:false,ok:true});
    if(path==='/api/ota/status')return resp({ok:OTA.ok,inProgress:OTA.inProgress,error:OTA.error,
      stage:OTA.stage,updateError:0,attempt:OTA.attempt,receivedBytes:OTA.received,totalBytes:OTA.total});
    if(path==='/api/system/stats')return resp({fwVersion:'3.29.3',build:'P4-CAN Universal',
      uptimeMs:86400000,heap:180000,psram:24000000,resetReason:'poweron',busOffA:0,busOffB:0,busOffC:0});
    if(path==='/api/s3xy/stats')return resp({bluetoothEnabled:true,pairedCount:2,connectedCount:2,devices:[]});
    if(path==='/api/s3xy/scan')return resp({devices:[]});
    if(path==='/api/r79/stats')return resp({hw3Supported:false,enabled:false,bit19:false,bit47:false});
    if(path==='/api/ulc/stats')return resp({enabled:false});
    if(path==='/api/summon/stats')return resp({tlssc:true,canState:1});
    if(path==='/api/country/stats')return resp({mode:0});
    if(path==='/api/blinkA/stats')return resp({enabled:true,displayState:'AUTOSTEER'});
    if(path==='/api/drivermonitoring/config')return resp({enabled:false});
    if(path==='/api/isa-suppression/config')return resp({enabled:false});
    if(path==='/api/continuous-ap/config')return resp({enabled:false});
    if(path==='/api/vision-control/stats')return resp({enabled:false});
    if(path==='/api/lab/auto-lane-change/stats')return resp({enabled:false,alcRaw:0,alcValid:false});
    if(path==='/api/lab/summon-heartbeat/stats')return resp({enabled:false});
    if(path==='/api/lab/driver-window/stats')return resp({enabled:false});
    if(path==='/api/pedalmap/stats')return resp({mode:'stock'});
    if(path==='/api/researchcapture/stats')return resp({running:false});

    /* ---- 灯光功能（作者 v3.35.6 整页）---- */
    if(path==='/api/lighting/stats')return resp(Object.assign({ok:true},SIM.lighting));
    if(path==='/api/lighting/update'){
      if(method==='POST'){
        if(q.indexOf('apForcedHighBeam=')>=0)SIM.lighting.apForcedHighBeam=q.indexOf('apForcedHighBeam=1')>=0;
        if(q.indexOf('highBeamStrobe=')>=0)SIM.lighting.highBeamStrobe=q.indexOf('highBeamStrobe=1')>=0;
        if(q.indexOf('rearFogStrobe=')>=0)SIM.lighting.rearFogStrobe=q.indexOf('rearFogStrobe=1')>=0;
        var dm=/dynamicBrakeMode=(\d)/.exec(q);if(dm)SIM.lighting.dynamicBrakeMode=Number(dm[1]);
        return resp(Object.assign({ok:true},SIM.lighting));
      }
      return resp(Object.assign({ok:true},SIM.lighting));
    }

    /* ---- 上游 Wi-Fi 中继（AP+STA+NAPT）---- */
    if(path==='/api/wifi/uplink'){
      if(method==='POST'){
        if(q.indexOf('off')>=0){SIM.uplink.enabled=false;SIM.uplink.connected=false;SIM.uplink.napt=false;}
        else{SIM.uplink.enabled=true;SIM.uplink.haveCfg=true;SIM.uplink.connected=true;SIM.uplink.napt=true;
             SIM.uplink.ssid=SIM.uplink.ssid||'Home-5G';SIM.uplink.ip='192.168.1.87';SIM.uplink.rssi=-47;SIM.uplink.upSec=12;}
        return resp({ok:true,uplink:SIM.uplink});
      }
      return resp({ok:true,uplink:SIM.uplink});
    }

    /* ---- 独立抓包：给几行假帧，方便核对排版 ---- */
    if(path==='/api/capture/status')return resp({ok:true,on:SIM.cap.on,cap:SIM.cap.cap,total:SIM.cap.total,
      seq:SIM.cap.total,busMask:7,idFilter:0,psram:true,startMs:0,uptimeMs:123456});
    if(path==='/api/capture/frames'){
      if(!SIM.cap.rows.length){
        var ids=[0x249,0x3F5,0x129,0x2B9,0x399,0x239];
        for(var fi=0;fi<12;fi++){
          SIM.cap.rows.push({ms:1200+fi*42,bus:'A',id:ids[fi%ids.length],dlc:8,
            hex:'01 06 00 00 00 00 00 0'+(fi%8)});
        }
        if(SIM.cap.total<12)SIM.cap.total=12;
      }
      return resp({ok:true,rows:SIM.cap.rows});
    }
    if(path==='/api/capture/start'){SIM.cap.on=true;SIM.cap.rows=[];SIM.cap.total=0;return resp({ok:true})}
    if(path==='/api/capture/stop'){SIM.cap.on=false;return resp({ok:true})}
    if(path==='/api/capture/clear'){SIM.cap.on=false;SIM.cap.rows=[];SIM.cap.total=0;return resp({ok:true})}

    /* ---- 其他 POST：一律成功 ---- */
    if(method==='POST')return resp({ok:true});
    return resp({ok:true});
  }

  var realFetch=window.fetch.bind(window);
  window.fetch=function(input,init){
    init=init||{};
    var url=(typeof input==='string')?input:(input&&input.url)||'';
    var hash=url.indexOf('#');if(hash>=0)url=url.slice(0,hash);
    var qi=url.indexOf('?');
    var path=qi<0?url:url.slice(0,qi);
    var q=qi<0?'':url.slice(qi+1);
    var method=(init.method||(input&&input.method)||'GET').toUpperCase();
    if(path.indexOf('/api/')!==0){
      return realFetch(input,init);
    }
    var r=route(path,q,method,init);
    log(method+' '+path+(q?'?'+q:'')+'  →  '+r.status);
    return Promise.resolve(r);
  };

  /* OTA uses XMLHttpRequest (for upload progress), so the fetch shim above
     never sees it. Patch the prototype instead: accept POST /update, replay a
     believable progress curve, then stop short of the real reboot handshake —
     waitForP4CanReconnect() would call location.replace('/?resume=...') which
     is meaningless under file://. We show the reboot screen ourselves. */
  (function(){
    var P=window.XMLHttpRequest&&window.XMLHttpRequest.prototype;
    if(!P||P.__p4otaPatched)return;
    P.__p4otaPatched=true;
    var oOpen=P.open,oSend=P.send;
    P.open=function(m,u){
      this.__p4ota=(String(u).indexOf('/update')===0);
      return oOpen.apply(this,arguments);
    };
    P.send=function(body){
      if(!this.__p4ota)return oSend.apply(this,arguments);
      var xhr=this,size=1.2*1024*1024;
      try{if(body&&body.get){var f=body.get('update');if(f&&f.size)size=f.size;}}catch(e){}
      OTA.attempt++;OTA.inProgress=true;OTA.ok=false;OTA.error='';
      OTA.stage='write';OTA.received=0;OTA.total=size;
      try{localStorage.setItem('simOtaAttempt',String(OTA.attempt));}catch(e){}
      log('POST /update  ·  multipart  ·  '+Math.round(size/1024)+' KB');
      var pct=0;
      var timer=setInterval(function(){
        pct=Math.min(100,pct+5);
        OTA.received=Math.round(size*pct/100);
        try{
          xhr.upload.dispatchEvent(new ProgressEvent('progress',
            {lengthComputable:true,loaded:OTA.received,total:size}));
        }catch(e){}
        if(pct>=100){
          clearInterval(timer);
          OTA.inProgress=false;OTA.ok=true;OTA.stage='complete';OTA.received=size;
          log('   ↳ flash ok · '+size+' bytes · restart simulated');
          simOtaDone(xhr,size);
        }
      },110);
    };
    function simOtaDone(xhr,size){
      var bar=document.getElementById('otaBar'),msg=document.getElementById('otaMsg');
      if(bar)bar.style.width='100%';
      var payload=JSON.stringify({ok:true,inProgress:false,error:'',stage:'complete',
        updateError:0,attempt:OTA.attempt,receivedBytes:size,totalBytes:size});
      try{Object.defineProperty(xhr,'readyState',{value:4,configurable:true});}catch(e){}
      try{Object.defineProperty(xhr,'status',{value:200,configurable:true});}catch(e){}
      try{Object.defineProperty(xhr,'responseText',{value:payload,configurable:true});}catch(e){}
      try{Object.defineProperty(xhr,'response',{value:payload,configurable:true});}catch(e){}
      if(msg)msg.textContent='Flash successful · reboot simulated · reloading in 3 s';
      var rs=document.getElementById('profileRebootScreen');
      if(rs){
        rs.classList.add('show');
        var t=document.getElementById('profileRebootTitle');
        if(t)t.textContent='OTA Complete · Simulated';
        var s=document.getElementById('profileRebootSub');
        if(s)s.textContent='镜像已被模拟引导程序接受，共 '+(size/1048576).toFixed(2)+' MB。\n'
          +'真实设备此时会重启，Wi-Fi 会中断约 10 秒，然后自动回到首页。';
      }
      setTimeout(function(){location.href=location.pathname;},3000);
    }
  })();

  function log(line){
    SIM.log.unshift(line);
    if(SIM.log.length>60)SIM.log.pop();
    var el=document.getElementById('simLog');
    if(el){
      el.innerHTML=SIM.log.slice(0,14).map(function(s){
        return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/\s+→\s+(\d{3})/,'  <b>→ $1</b>');
      }).join('\n');
    }
    var a=document.getElementById('simAct');
    if(a&&line.indexOf('↳')>=0)a.textContent='最近动作：'+line.replace(/^\s*↳\s*/,'').trim();
  }

  document.addEventListener('DOMContentLoaded',function(){
    var fab=document.getElementById('simFab'),panel=document.getElementById('simPanel');
    var wired=document.getElementById('simWired'),hint=document.getElementById('simHint');
    var mod=document.getElementById('simModule');
    function sync(){
      wired.checked=SIM.wired;
      if(mod)mod.checked=SIM.hasModule;
      hint.textContent=(SIM.wired
        ? '非凡页顶部显示 SYNC：开关会以 POST /api/feifan/config 回传（仅内存 + localStorage）。'
        : '非凡页顶部显示 LOCAL：等同你当前未接后端的固件，开关只存本机。')+
        (SIM.hasModule?' 已模拟 TSL 模块在线：模块项走配置块。':
          ' 未装 TSL 模块：模块项显示「需模块」并锁定，原生项直接发帧。');
    }
    fab.onclick=function(){panel.classList.toggle('on');sync();};
    document.getElementById('simExit').onclick=function(){panel.classList.remove('on');};
    wired.onchange=function(){
      SIM.wired=wired.checked;
      try{localStorage.setItem('simFeifanWired',SIM.wired?'1':'0');}catch(e){}
      sync();location.reload();
    };
    if(mod)mod.onchange=function(){
      SIM.hasModule=mod.checked;
      try{localStorage.setItem('simFeifanModule',SIM.hasModule?'1':'0');}catch(e){}
      sync();location.reload();
    };
    document.getElementById('simClear').onclick=function(){
      try{localStorage.removeItem(LS);localStorage.removeItem('p4canLang');
          localStorage.removeItem('simOtaAttempt');localStorage.removeItem('simFeifanModule');}catch(e){}
      SIM.cfg={};location.reload();
    };
    document.getElementById('simReload').onclick=function(){location.reload();};
    sync();
  });
})();
</script>
"""


def build_sim(raw):
    s = raw.decode("utf-8")
    if "</head>" not in s:
        raise SystemExit("HTML 里找不到 </head>")
    s = s.replace("</head>", MOCK + "</head>", 1)
    out = os.path.join(OUTDIR, "p4can-sim.html")
    open(out, "w", encoding="utf-8").write(s)
    print("写出      : %s  (%d 字节)" % (out, len(s.encode("utf-8"))))


if __name__ == "__main__":
    build_sim(extract())
