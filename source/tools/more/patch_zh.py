#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""补齐看板中文词条（主要是「实验 / LAB」页）。

`dashboard_source.html` 里的 P4CAN-ZH 块是一张 en -> zh 精确匹配表，
上游只覆盖了首页 / 设备 / 设置，实验页整页仍是英文。这个脚本把缺的词条
合并进 DICT，幂等：已有词条一律保留（先插入者胜），只补新的。

    python tools/more/patch_zh.py

注意：只写 en -> zh 单向映射。切回英文靠页面自己的 textMemo 还原原文，
所以这里不需要反向词条 —— 加了反而会和 `2 次` / `闪 2 次` 这类撞车。
"""
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DASH = REPO / 'firmware' / 'P4can-unlock-more' / 'dashboard_source.html'

START = '<!--P4CAN-ZH-START-->'
END = '<!--P4CAN-ZH-END-->'
DYN_START = '<!--P4CAN-ZH-DYN-START-->'
DYN_END = '<!--P4CAN-ZH-DYN-END-->'

# 运行时状态兜底
# ----------------
# 实验页的「驾驶状态」不是静态文本：renderR79Summary() 每 500 ms 用英文重新
# 写一遍 #r79TxBig / #r79TxSub。中文包的 MutationObserver 能翻，但下一次渲染
# 又覆盖回英文，视觉上会闪。这里只盯这几个已知节点，按 ' · ' 分段查表。
DYN_JS = r'''<!--P4CAN-ZH-DYN-START-->
<script>
(function(){
  var IDS=['r79TxBig','r79TxSub','labGearBig','homeR79State','homeR79Reason'];
  var RULES=[
    [/^Waiting$/,'等待中'],[/^Active$/,'激活'],[/^Park standby$/,'驻车待机'],
    [/^Ready$/,'就绪'],[/^AP Wait$/,'AP 等待'],[/^AP engaged$/,'AP 已启用'],
    [/^Summon$/,'召唤'],[/^CAN OFFLINE$/,'CAN 离线'],[/^DEFAULT$/,'默认'],
    [/^WAIT TEMPLATE$/,'等待模板'],[/^BUSY$/,'处理中'],[/^BLOCKED$/,'已拦截'],
    [/^SUSPENDED$/,'已挂起'],[/^READY$/,'就绪'],[/^ACTIVE$/,'激活']
  ];
  function zh(s){
    if(!s)return s;
    var parts=String(s).split(' · '),out=[],i,k,p,n;
    for(i=0;i<parts.length;i++){
      p=parts[i].trim(); n=null;
      for(k=0;k<RULES.length;k++){
        if(RULES[k][0].test(p)){n=p.replace(RULES[k][0],RULES[k][1]);break;}
      }
      out.push(n===null?parts[i]:n);
    }
    return out.join(' · ');
  }
  function isZh(){try{return localStorage.getItem('p4canLang')!=='en';}catch(e){return true;}}
  function tick(){
    if(!isZh())return;
    for(var i=0;i<IDS.length;i++){
      var el=document.getElementById(IDS[i]); if(!el)continue;
      var t=el.textContent; if(!t)continue;
      var n=zh(t);
      if(n!==t)el.textContent=n;
    }
  }
  setInterval(tick,600);
  tick();
})();
</script>
<!--P4CAN-ZH-DYN-END-->'''


# ---------------------------------------------------------------- 词表
# 约定：
#   散文、控件标题、状态标签  -> 翻译
#   帧 ID、位域表达式、纯数值  -> 保留原文（DICT 里不出现，页面就不动它）
#   `N s` / `N ms` 由页面正则自动处理，不要在这里重复
ZH_EXTRA = {
    # ---- 更多功能：SUPER_3CAN 移植项与精简后的分组文案
    # ---- 灯光功能：作者 v3.35.6 整页移植（替换原爆闪/强制远光）
    'Lighting': '灯光功能',
    'Set AP high beam and dynamic brake light.': '设置自动驾驶远光灯和动态刹车灯。',
    'Manual high beam in Autopilot': '自动驾驶时手动开远光',
    'Push the stalk forward once in AP to turn on, again to turn off.':
        '自动驾驶中向前推一次拨杆开启，再推一次关闭。',
    'High beam strobe': '远光灯频闪',
    'Stalk gesture only. Pull three times quickly to start, once more to stop. 70 ms on / off.':
        '仅授权拨杆手势。快速拉动拨杆三次启动，再拉一次停止。亮、灭各 70 毫秒。',
    'Rear fog strobe': '后雾灯频闪',
    'Hold the right scroll wheel and scroll the left wheel down to flash the rear fog 20 times.':
        '按住右滚轮并向下滚动左滚轮，触发后雾灯闪烁 20 次。',
    'Dynamic brake light': '动态刹车灯',
    'Flash the brake light to warn following cars.': '踩刹车时让刹车灯闪烁提醒后车。',
    'Off': '关闭',
    'Light press': '轻踩',
    'Hard press': '深踩',
    'Loading…': '加载中…',
    'Saved': '已保存',
    'Update failed': '保存失败',
    'strobe on': '爆闪运行',
    'AP beam': 'AP 远光',
    'fog ': '后雾灯 ',
    'stalkless vehicle': '此车为无杆方案',
    # ---- 设置页：抓包面板 / Wi-Fi 中继
    'Packet Capture': '抓包',
    'Record CAN frames and export CSV': '记录 CAN 帧并导出 CSV',
    'Records every frame this controller receives. Read-only — nothing is transmitted. The ring keeps the newest 16k frames (2k on internal RAM).':
        '记录本控制器收到的每一帧。只读，绝不发帧。环缓冲保留最新 16384 帧（无 PSRAM 时 2048 帧）。',
    'BUSES': '总线',
    'ONLY ID · HEX, BLANK = ALL': '只看该 ID · 十六进制，留空 = 全部',
    'Start capture': '开始抓包',
    'Stop': '停止',
    'Clear': '清空',
    'LATEST FRAMES': '最新帧',
    'Download CSV': '下载 CSV',
    'WI-FI RELAY · UPSTREAM': 'WI-FI 中继 · 上游',
    'HOME WI-FI (SSID)': '家里 Wi-Fi（SSID）',
    'PASSWORD · LEAVE BLANK TO KEEP CURRENT': '密码 · 留空表示沿用已保存的',
    'Connect as Relay': '作为中继连接',
    'Stop Relay': '停止中继',
    'Relay off · clients can only reach 192.168.4.1': '中继关闭 · 客户端只能访问 192.168.4.1',
    'Relay keeps the P4 hotspot and joins your home Wi-Fi at the same time, so phones on the P4 hotspot keep internet. The P4 stays at 192.168.4.1 and hands out the upstream router\'s DNS. Saved in NVS and restored on boot.':
        '中继让 P4 热点和家里 Wi-Fi 同时在线：手机连 P4 热点照样有网。P4 仍是 192.168.4.1，DNS 由上游路由器下发。配置存 NVS，重启后自动恢复。',
    'Not armed': '未启动',
    'Off-road mode': '越野模式',
    '0x2B6 = 60 00 00': '0x2B6 = 60 00 00',
    'ESP off · drift': '关闭 ESP · 漂移',
    '0x2B6 = 46 00 00': '0x2B6 = 46 00 00',
    'Power boost': '动力狂暴',
    '0x334 byte0 = FF · max power + regen': '0x334 byte0 = FF · 功率拉满 + 强回收',
    'Scroll-wheel shift': '滚轮换挡',
    '0x229 · P/R/D at 8 km/h or below': '0x229 · 8 km/h 以下 P/R/D',
    'SERVICE mode': 'SERVICE 模式',
    '0x339 ×3 · one-shot': '0x339 ×3 · 一次性',
    'Service mode': '维护模式',
    'Service': '维护',
    '0x339 · one frame per tap, never replayed': '0x339 · 每次点击只发一帧，不自动重放',
    'Enter service mode': '进入维护模式',
    'Not sent': '未发送',
    'Send SERVICE': '发送 SERVICE',
    'Session only': '仅本次会话',
    'Off after every reboot · never written to flash': '每次重启后自动关闭 · 不写入 flash',
    'Everything listed here is wired to this P4 firmware and transmitted over the three CAN buses. Switch-only items live on this controller; the rest follow a real stock frame and are only sent while the switch is on.':
        '这里列出的每一项都已接入本 P4 固件并经三路 CAN 下发。纯开关项只存在本机控制器；其余项跟随真实原车帧，仅在本开关打开时才发送。',
    '6 groups': '6 组',
    'ADAS core · scroll-wheel shift · power boost': 'ADAS 核心 · 滚轮换挡 · 动力狂暴',
    'Strobe · forced high beam': '爆闪 · 强制远光',
    'Strobe and forced high beam, driven by the stock 0x249 stalk frames.':
        '爆闪与强制远光，走原车 0x249 灯光杆帧（BODY 路）。',
    'Seat memory names': '座椅记忆命名',
    'Nag suppression · AP settings · EAP · Summon': '防唠叨 · AP 设置 · EAP · 召唤',
    'Off-road · ESP · SERVICE · body state': '越野 · ESP · SERVICE · 车身状态',
    'Theme · display · local options': '主题 · 显示 · 本地选项',
    'Nag Killer, turn-signal lane change, scroll-wheel shift and power boost.':
        '唠叨杀手、打灯变道、滚轮换挡与动力狂暴。',
    'Strobe and forced high beam, driven by the stock 0x249 stalk frames.':
        '爆闪与强制远光，跟随原车 0x249 灯光杆帧。',
    'Seat memory names stored on this controller.': '座椅记忆名称，仅保存在本机控制器。',
    'Nag suppression, AP settings, EAP and Summon.': '防唠叨、AP 设置、EAP 与召唤。',
    'Off-road, ESP and SERVICE are one-shot overlays on top of real stock frames.':
        '越野、ESP 与 SERVICE 都是在真实原车帧上做的一次性覆写。',
    'Off-road and ESP rewrite the next stock frame; service mode sends one frame per tap.':
        '越野与 ESP 覆写下一条原车帧；维护模式每次点击只发一帧。',
    'Live telemetry and local shortcuts.': '实时遥测与本机快捷项。',
    'Theme, display and local options.': '主题、显示与本地选项。',
    'Shift & power': '换挡与动力',
    'Gauges & shortcuts': '仪表与快捷项',
    'Gauges': '仪表',
    'Headlight': '大灯',
    'Lights': '灯光',
    'AP & EAP': 'AP 与 EAP',
    '1 entries': '1 条记录',
    'Already implemented in P4-CAN, mapped instead of rebuilt': '本固件已实现，直接映射而非重写',
    'Off-road': '越野',
    'Seat & memory': '座椅与记忆',
    'Drivetrain': '动力总成',
    'Body & drivetrain': '车身与动力',
    'Body state': '车身状态',
    'General': '通用',
    'App settings': '应用设置',
    'Drive assist': '驾驶辅助',

    # ---- 更多功能：落地通道图例（静态 HTML，走 DICT 翻译；
    #      JS 动态生成的「P4 原生 / 需模块」标签自带双语，不进词表）
    'P4 native': 'P4 原生',
    'Sent by this firmware over the three CAN buses, no external module needed':
        '由本固件经三路 CAN 直接发帧，不需要任何外挂模块',
    'Needs module': '需模块',
    'Requires a TSL module installed in the car; disabled when absent':
        '需要车上安装 TSL 模块；未安装时该项禁用',
    'Evidence grade': '证据等级',
    'A read from decompiled frames · B inferred from UI naming · C local only':
        'A 读自反编译帧定义 · B 由界面命名推断 · C 纯本地行为',
    '5 groups': '5 组',
    'Off-road · ESP · SERVICE': '越野 · ESP · SERVICE',
    'Off-road · ESP · Service mode': '越野 · ESP · 维护模式',
    '3 groups': '3 组',
    'Scroll-wheel shift · power boost': '滚轮换挡 · 动力狂暴',
    'Scroll-wheel shift and power boost — both rewritten from the stock frames.':
        '滚轮换挡与动力狂暴，都是直接改写原车帧。',
    'Only what the Settings page does not already have. Every entry here rewrites or answers a real stock frame, and is transmitted over the three CAN buses only while its switch is on.':
        '这里只列设置页没有的项。每条都改写或应答真实原车帧，仅在其开关打开时经三路 CAN 下发。',
    'A — read from the decrypted frame tables; every entry left on this page is grade A':
        'A — 读自反编译的帧表；本页保留的每一项都是 A 级',
    'Stock-frame rewrites only · 7 entries in 3 groups': '只做原车帧改写 · 3 组 7 项',
    'RAM only': '仅内存',
    'Off again after every reboot, never written to flash':
        '重启后自动关闭，从不写入 Flash',

    # ---- 实验页主页
    'Research tools. Read-only unless marked experimental.': '研究工具。除标注实验性外均为只读。',
    'Research Tools': '研究工具',
    'Driving State · live': '驾驶状态 · 实时',
    'Waiting': '等待中',
    'R79 TX · Reason —': 'R79 发送 · 原因 —',
    'ALC STATE': 'ALC 状态',
    'Lane': '车道',
    'LEFT': '左',
    'RIGHT': '右',
    'R79 AP Control': 'R79 AP 控制',
    'Block or delay R79 during AP.': 'AP 期间拦截或延迟 R79。',
    'R79 Status': 'R79 状态',
    'View R79 send status.': '查看 R79 发送状态。',
    'Summon Heartbeat Override': '召唤心跳覆盖',
    'Test Summon heartbeat signals.': '测试召唤心跳信号。',
    'Auto Lane Change Enable': '自动变道使能',
    'Test lane-change enable signals.': '测试变道使能信号。',
    'CAN Research Capture': 'CAN 研究抓包',
    'Record and download CAN signals.': '录制并下载 CAN 信号。',
    'Driver Monitoring Capture': '驾驶员监测抓包',
    'Record driver-monitoring signals.': '录制驾驶员监测信号。',
    'EXP': '实验性',

    # ---- R79 AP 控制面板
    'Set R79 behavior during AP and manual driving independently.': '分别设置 AP 与手动驾驶时的 R79 行为。',
    'Allow R79 During Manual Driving': '手动驾驶时允许 R79',
    'OFF keeps the default AP behavior': '关闭则保持默认 AP 行为',
    'Separate setting · default OFF': '独立设置 · 默认关闭',
    'AP Policy': 'AP 策略',
    'Block during AP': 'AP 期间拦截',
    'Delay after AP': 'AP 后延迟',
    'AP DELAY (S)': 'AP 延迟 (秒)',
    'Gate state': '门控状态',
    'Remaining': '剩余',
    'ON bypasses only manual D/R suppression. Existing AP policy and transport safety remain active.':
        '开启仅绕过手动挡 D/R 抑制，AP 策略与传输安全仍然生效。',
    'Loading saved settings…': '正在载入已保存设置…',

    # ---- 召唤心跳面板
    '0x3F8 UI_driverAssistControl · UI_summonHeartbeat bits 2–3 · raw values 0–3.':
        '0x3F8 UI_driverAssistControl · UI_summonHeartbeat bit 2–3 · 原始值 0–3。',
    'Continuous Override': '持续覆盖',
    'Applies the selected raw value to every valid stock frame': '对每一帧有效的原厂帧应用所选原始值',
    'Raw Value': '原始值',
    'Two-bit field · 0, 1, 2 or 3': '两位字段 · 0、1、2 或 3',
    'RUNTIME STATE': '运行状态',
    'GATE': '门控',
    'Stock raw / age': '原厂原始值 / 时效',
    'Selected / last applied': '所选 / 上次应用',
    'Applied / TX fail / blocked': '已应用 / 发送失败 / 拦截',
    'Session-only experimental override. It is OFF after every reboot and CAN full-recovery. Other 0x3F8 fields are preserved and existing transport safety checks remain active.':
        '仅本次会话的实验性覆盖，每次重启与 CAN 完全恢复后都会关闭。0x3F8 的其它字段保持原样，现有传输安全检查仍然生效。',
    'Loading session state…': '正在载入会话状态…',

    # ---- 自动变道使能面板
    'Experimental 0x293 UI_autoLaneChangeEnable injection. Independent from the production CAN B 0x3F8 compositor.':
        '实验性 0x293 UI_autoLaneChangeEnable 注入，与正式的 CAN B 0x3F8 合成器相互独立。',
    '0x293 UI_chassisControl · UI_autoLaneChangeEnable bits 24–25 → raw 1 · AP active':
        '0x293 UI_chassisControl · UI_autoLaneChangeEnable bit 24–25 → 原始值 1 · AP 激活',
    '0x293 TX Target': '0x293 发送目标',
    '0x293 TX A OK / FAIL': '0x293 发送 A 成功 / 失败',
    '0x293 TX B OK / FAIL': '0x293 发送 B 成功 / 失败',
    'Independent from Confirm-Free 0x3F8 target · same-bus stock template only':
        '与免确认 0x3F8 目标相互独立 · 仅用同总线原厂模板',
    'Requests / completed / blocked': '请求 / 完成 / 拦截',
    'Gate-blocked A / B': '门控拦截 A / B',
    'Fast attempts / OK / FAIL': '快速尝试 / 成功 / 失败',
    'MUX1 RX / TX OK / FAIL': 'MUX1 接收 / 发送 成功 / 失败',

    # ---- CAN 研究抓包面板
    'BODY + CHASSIS + PARTY research recorder with automatic multi-segment re-arm.':
        'BODY + CHASSIS + PARTY 三路研究录制器，支持自动多段续录。',
    'SNAPSHOT: latest state sets · RAW: actual BODY + CHASSIS + PARTY RX frames. Recorder never transmits CAN.':
        '快照：最新状态集合 · 原始：真实的 BODY + CHASSIS + PARTY 接收帧。录制器从不发送 CAN。',
    'CAPTURE STATE': '录制状态',
    'CAPTURE MODE': '录制模式',
    'Snapshot plan': '快照计划',
    'PRE WINDOW': '前置窗口',
    'POST WINDOW': '后置窗口',
    'PRE BUFFER': '前置缓冲',
    'REMAINING POST': '剩余后置',
    'RAW PRE ring / archive': '原始前置 环形 / 归档',
    'RAW PRE actual coverage': '原始前置 实际覆盖',
    'RAW TRANSITION': '原始切换',
    'PRE ready / slots': '前置就绪 / 槽位',
    'SAMPLES / CAPACITY': '样本 / 容量',
    'SEGMENTS / MAX': '分段 / 上限',
    'Pre 2 s · trigger · POST 5 s': '前置 2 秒 · 触发 · 后置 5 秒',
    'PRE 2 s · trigger · POST 5 s': '前置 2 秒 · 触发 · 后置 5 秒',
    'PRE 0 · trigger 0 rows · POST 0 / 4': '前置 0 · 触发 0 行 · 后置 0 / 4',
    'OBSERVED IDs': '观测到的 ID',
    'Last segment snapshots': '上次分段快照',
    'CURRENT / LAST': '当前 / 上次',
    'Download Research CSV': '下载研究数据 CSV',
    'Reset CAN Research Capture': '重置 CAN 研究抓包',
    'AUTO last transition': '自动 上次切换',
    'AUTO qualified / segments / merged': '自动 合格 / 分段 / 合并',
    'AUTO LEFT OPEN / BLOCKED': '自动左窗 开 / 拦截',
    'Quiet arm / fire / guard': '静默 待命 / 触发 / 保护',
    'Periodic': '周期',
    'Periodic OK / FAIL': '周期 成功 / 失败',
    'STATE': '状态',
    'SNAPSHOT': '快照',
    'Memory': '内存',
    'Storage': '存储',
    'Transport': '传输',

    # ---- 驾驶员监测抓包面板
    'Driver Monitoring bit43': '驾驶员监测 bit43',
    'Download Driver Monitoring CSV': '下载驾驶员监测 CSV',
    'Reset Driver Monitoring Capture': '重置驾驶员监测抓包',
    'DRIVER INTERACTION': '驾驶员交互',
    'FRONT · NORMAL': '前 · 正常',
    'FRONT · NO TORQUE': '前 · 无扭矩',
    'TORQUE + LOOK AWAY': '扭矩 + 分心',
    'SCREEN GLANCE': '看屏幕',
    'SIDE LOOK': '侧视',
    'LOOK DOWN': '低头',
    'Last interaction transition': '上次交互切换',

    # ---- 驾驶员车窗测试面板
    'Driver Window Test': '驾驶员车窗测试',
    'Model YL only · PARK required · clones fresh CAN B 0x3C2 MUX0 and sets byte 6 bits [3:2] to Auto Down value 2.':
        '仅 Model YL · 需挂 P 挡 · 克隆最新的 CAN B 0x3C2 MUX0，并把 byte6 的 bit[3:2] 设为 Auto Down 值 2。',
    'Test window opening in Park.': '在 P 挡测试车窗开启。',
    'OPEN DRIVER WINDOW · AUTO DOWN': '打开驾驶员车窗 · 自动下降',
    'Fixed R79 Policy': '固定 R79 策略',
    'Bit 18 policy': 'Bit 18 策略',
    'Mode 1 / Mode 2 · bit19=0 · bit47 follows HW3 policy · manual D/R suppression':
        '模式 1 / 2 · bit19=0 · bit47 跟随 HW3 策略 · 手动挡 D/R 抑制',
    'Transport and scheduler are fixed in v3.7. Bit 18 is configured from Settings.':
        '传输与调度在 v3.7 已固定。Bit 18 在「设置」中配置。',
    'LABEL A': '标签 A',
    'LABEL B': '标签 B',
    'LABEL C': '标签 C',
    'LABEL D': '标签 D',
    'Save Labels': '保存标签',

    # ---- 各面板通用状态行
    'READY': '就绪',
    'ACTIVE': '激活',
    'AVAILABILITY': '可用性',
    'ACTION STATE': '动作状态',
    'TEST': '测试',
    'Capture': '抓包',
    'Captured data': '已抓数据',
    'Runtime / template': '运行 / 模板',
    'Requests / ignored / dropped': '请求 / 忽略 / 丢弃',
    'TX OK / FAIL': '发送 成功 / 失败',
    'BOTH': '双路',
    'CAN A stock / age / RX': 'CAN A 原厂 / 时效 / 接收',
    'CAN B stock / age / RX': 'CAN B 原厂 / 时效 / 接收',
    'STOCK A / B': '原厂 A / B',
    'Last TX raw': '上次发送原始值',
    'Last forced TX': '上次强制发送',
    'Stock 0x3C2 age': '原厂 0x3C2 时效',
    'OFF · 0': '关 · 0',
    'WARMUP': '预热',
    'Read-only production status for the selected R79 transmitter.': '所选 R79 发送器的只读正式状态。',

    # ---- 更多功能页：开源地址
    'Open source:': '开源项目地址：',

    # ---- Nag Killer 主面板（panelNag）
    'NAG Method': 'NAG 方式',
    'Torque · existing': '扭矩 · 原车',
    'Torque · Party CAN A': '扭矩 · PARTY CAN A',
    'Ignore AP State': '忽略 AP 状态',
    'Torque A/B/C/H only · default OFF · other safety gates remain active': '仅扭矩 A/B/C/H 模式 · 默认关闭 · 其余安全门控保持生效',
    'Auto Speed Scroll': '自动右滚',
    'Torque mode · periodic during AP and on new warnings': '扭矩模式 · AP 期间与新警告出现时周期性右滚',
    'INTERVAL (S)': '间隔（秒）',
    'Waveform': '波形',
    'Choose the generated right-scroll sequence': '选择生成的右滚序列',
    'Pause While Stopped': '停车暂停',
    'Torque mode · pause at confirmed 0 km/h': '扭矩模式 · 确认 0 km/h 时暂停',
    'Right Speed': '右滚速度',
    'Periodic Right Speed': '周期右滚速度',
    'Left Volume': '左滚音量',
    'TSL9 Input Assist': 'TSL9 输入辅助',
    'TSL9 Sequence': 'TSL9 序列',
    'V8.2 Original · 4 → 1': 'V8.2 原版 · 4 → 1',
    'Extended · 2 / 3 / 4 → 1': '扩展版 · 2 / 3 / 4 → 1',
    'Legacy TSL9 Route': 'TSL9 旧路由',
    'Choose warning states': '选择警告状态',
    'Entire AP session': '整个 AP 会话',
    'Keep Hands-On downgrade active': '保持 Hands-On 降级生效',
    'Keep warning assist · add a regular AP-active injection': '保留警告辅助 · 叠加周期性 AP 激活注入',
    'One route only · Chassis state observation remains active': '仅选一条路由 · 底盘状态观测保持开启',
    'Choose steering-torque output or a TSL9 Hands-On signal override.': '选择方向盘扭矩输出，或 TSL9 Hands-On 信号覆盖。',
    'TSL9 · Extended': 'TSL9 · 扩展版',
    'TSL9 · V8.2': 'TSL9 · V8.2 原版',
    'Mode B · Burst / Pause': '模式 B · 突发 / 暂停',
    'Mode C · Random Walk': '模式 C · 随机游走',
    'Unavailable': '不可用',

    # ---- Nag 模式卡片（NAG_MODE_UI_COPY + nagModeHInfo，动态渲染）
    'CLASSIC · STEADY': '经典 · 恒定',
    'Mode A': '模式 A',
    'Classic steering torque table injection while AP is active.': 'AP 激活期间注入经典方向盘扭矩表。',
    'Torque table': '扭矩表',
    'AP only': '仅 AP',
    'BURST / PAUSE': '突发 / 暂停',
    'Mode B': '模式 B',
    'Alternates injection bursts with pause windows while AP is active.': 'AP 激活期间交替进行突发注入与暂停窗口。',
    'Burst cadence': '突发节奏',
    'RANDOM WALK': '随机游走',
    'Mode C': '模式 C',
    'Continuous randomized torque-table selection while AP is active.': 'AP 激活期间持续随机选择扭矩表。',
    'Continuous': '持续',
    'Randomized': '随机',
    'HUMAN INTERACTION': '人工交互',
    'Stock-opposite carrier and Human Interaction events.': '反向载波与人工交互事件。',
    'Per-RX carrier': '逐帧载波',
    'Strict speed gate': '严格车速门控',
    'Human interaction': '人工交互',
    'OPPOSITE CARRIER': '反向载波',
    'Per-RX stock-opposite carrier during WAIT and REFRACTORY, plus Human Interaction primary events biased 80% negative / 20% positive.': '等待期与不应期逐帧反向载波，外加偏向 80% 负向 / 20% 正向的人工交互主事件。',
    '1.80–2.60 Nm peak': '1.80–2.60 Nm 峰值',
    '0.10–0.60 Nm carrier': '0.10–0.60 Nm 载波',
    'TIERED HO=1 / 2': '分级 HO=1 / 2',
    'Per-RX random': '逐帧随机',
    'MODE A': '模式 A',
    'MODE B': '模式 B',
    'MODE C': '模式 C',
    'MODE H': '模式 H',
    'Body CAN A · 0x39B': '车身 CAN A · 0x39B',
    'Chassis CAN B · 0x399': '底盘 CAN B · 0x399',
    'Party CAN A · 0x399': 'PARTY CAN A · 0x399',

    # ---- Nag Mode H 调校页（panelNagModeH）
    'Mode H profile tuning and live engine monitor.': 'Mode H 参数调校与实时引擎监视。',
    'Mode H tuning and live engine monitor.': 'Mode H 调校与实时引擎监视。',
    'Current profile runtime': '当前配置运行状态',
    'LIVE ENGINE': '实时引擎',
    'ENGINE OUTPUT': '引擎输出',
    'PHASE / EVENT': '阶段 / 事件',
    'PRIMARY PEAK MIN (Nm)': 'PRIMARY 峰值下限（Nm）',
    'PRIMARY PEAK MAX (Nm)': 'PRIMARY 峰值上限（Nm）',
    'WAIT MIN (s)': '等待下限（秒）',
    'WAIT MAX (s)': '等待上限（秒）',
    'REFRACTORY MIN (s)': '不应期下限（秒）',
    'REFRACTORY MAX (s)': '不应期上限（秒）',
    'CARRIER MIN (Nm)': '载波下限（Nm）',
    'CARRIER MAX (Nm)': '载波上限（Nm）',
    'Hands-On Policy': 'Hands-On 策略',
    'full 2-bit HO overwrite · generated value is capped at HO=2': '完整 2 位 HO 覆写 · 生成值最高 HO=2',
    'ALWAYS HO=1': '始终 HO=1',
    'THRESHOLD HO=1': '阈值 HO=1',
    'HO=1 THRESHOLD (Nm)': 'HO=1 阈值（Nm）',
    'HO=2 THRESHOLD (Nm)': 'HO=2 阈值（Nm）',
    'HO=3 is never generated. In THRESHOLD HO=1 mode, HO=2 threshold is stored but not used. In TIERED mode, torque below the HO=1 threshold keeps the accepted stock HO field.': '不会生成 HO=3。阈值 HO=1 模式下 HO=2 阈值仅保存不使用；分级模式下，低于 HO=1 阈值的扭矩保留原厂 HO 字段。',
    'Visual Warning Rescue': '视觉警告救援',
    'DAS visual warning starts a delayed direct PRIMARY peak': 'DAS 视觉警告触发一次延迟的直接 PRIMARY 峰',
    'RESCUE DELAY (s)': '救援延迟（秒）',
    'Stop Behavior': '停止行为',
    'STOPPED only · event engine stays paused. STOCK CARRIER preserves stock torque + stock HO and advances only counter/checksum.': '仅停车状态 · 事件引擎保持暂停。原厂载波保留原厂扭矩与 HO，只推进计数器与校验和。',
    'HARD PAUSE · 0-TX': '硬暂停 · 零发送',
    'STOCK CARRIER': '原厂载波',
    'A/B test for stationary Hands-On warnings. Speed stale and move-confirming states remain hard-paused.': '用于静止 Hands-On 警告的 A/B 测试。车速过期与确认移动状态仍硬暂停。',
    'Apply Mode H Tuning': '应用 Mode H 调校',
    'Reset Current Profile': '重置当前配置',
    'Mode H update failed': 'Mode H 更新失败',
    'Mode H reset failed': 'Mode H 重置失败',
    'Stop behavior update failed': '停止行为更新失败',
    'STOCK CARRIER selected · stopped event generation is paused, while accepted stock torque/HO is echoed with a fresh counter/checksum.': '已选原厂载波 · 停车事件生成暂停，已接受的原厂扭矩/HO 以新计数器与校验和回显。',
    'RAW RX · PRE 5 s · POST 2 s · auto re-arm': '原始接收 · 前 5 秒 · 后 2 秒 · 自动续录',
    'STOCK · STOP CARRIER': '原厂 · 停车载波',

    # ---- Nag/抓包引擎动态状态词
    'ARCHIVE FULL': '归档已满',
    'CAPACITY FULL': '容量已满',
    'SEGMENTS FULL': '分段已满',
    'RECORDING': '录制中',
    'AUTO ARMED': '自动待命',
    'AUTO WARMUP': '自动预热',
    'ARMED': '已待命',
    'FRESH': '新鲜',
    'PRESENT': '存在',
    'OPEN': '开启',
    'OWNED': '已占用',
    'CARRIER': '载波',
    'EVENT': '事件',
    'FULL': '已满',
    'INVALID ROUTE': '路由无效',
    'NOT ALLOCATED': '未分配',

    # ---- ULC（panelUlc）
    'Apply Before AP Starts': 'AP 开始前应用',
    'Confirm-Free Lane Change': '免确认变道',
    'Configure confirmation, off-highway flags and the blind-spot setting. Controls share one CAN B output.': '配置免确认、非高速标志与盲点设置。各控件共用一条 CAN B 输出。',
    'Gate-blocked': '被门控拦截',
    'INJECTION GATE': '注入门控',
    'OFF · AP ACTIVE ONLY · current/default behavior': '关闭 · 仅 AP 激活时 · 当前/默认行为',
    'STOCK / FORCED BIT1': '原厂 / 强制 BIT1',
    'TX CAN B OK / FAIL': 'CAN B 发送 成功 / 失败',
    'Total TX OK / FAIL': '累计发送 成功 / 失败',
    'CAN B · CHASSIS · bit1 / Hz / age / RX': 'CAN B · 底盘 · bit1 / Hz / 时效 / 接收',
    'CAN B · CHASSIS · raw': 'CAN B · 底盘 · 原始值',
    'Auto Lane Change Enable · EXPERIMENTAL': '自动变道使能 · 实验性',
    'AP GATE': 'AP 门控',

    # ---- 国家 / 抓包窗口 / 设备 / 配对 / 连续 AP / R79 / 诊断 / OTA
    'All vehicle profiles with a valid CAN topology. Overrides follow fresh stock frames on the same bus while R79 is ACTIVE. Vehicle behavior remains unverified.': '适用于 CAN 拓扑有效的所有车型配置。R79 激活时覆盖跟随同总线的新鲜原厂帧。车辆实际行为未经确认。',
    '10 s': '10 秒',
    '15 s': '15 秒',
    '2 s': '2 秒',
    '5 s': '5 秒',
    'Configure this button without exposing low-level BLE data.': '配置此按键，不暴露底层 BLE 数据。',
    'Offline': '离线',
    'S3XY Button': 'S3XY 按键',
    'Add S3XY Button': '添加 S3XY 按键',
    'Choose a method': '选择一种方式',
    'Check controller health, Bluetooth and CAN communication. Recovery actions are listed below.': '检查控制器健康、蓝牙与 CAN 通信。恢复操作在下方列出。',
    'Heap current / minimum': '堆内存 当前 / 最低',
    'Largest heap block': '最大连续堆块',
    'Task stack HWM · RX workers': '任务栈高水位 · RX 工作线程',
    'Task stack HWM · Sup / Web / BLE': '任务栈高水位 · 监督 / Web / BLE',
    'BLE log capacity': 'BLE 日志容量',
    'Uptime': '运行时长',
    'SYSTEM': '系统',
    'SETTINGS': '参数',
    'NAG KILLER': '提醒抑制',
    'Last forced TX / age': '上次强制发送 / 时效',
    'PERIODIC INTERVAL (S)': '周期间隔（秒）',

    # ---- Nag 监视卡 + 高级参数（panelNag，嵌套 section 曾漏扫）
    'Live monitor': '实时监视',
    'NAG Continuity Monitor': 'NAG 连续性监视',
    'Current AP-session TX continuity and skip/block reasons. AP state uses validity, not an age timeout.': '本次 AP 会话的发送连续性与跳过/拦截原因。AP 状态按有效性判定，不按超时。',
    'AP state / validity': 'AP 状态 / 有效性',
    'Last TX / max gap': '上次发送 / 最大间隔',
    'Session TX OK': '本次会话发送成功',
    'TSL9 RX / modified / TX OK / FAIL': 'TSL9 接收 / 改写 / 发送成功 / 失败',
    'Last skip': '上次跳过',
    'Vehicle speed / gate': '车速 / 门控',
    'Mode H phase / motion': 'Mode H 阶段 / 运动',
    'Mode H event': 'Mode H 事件',
    'Mode H output': 'Mode H 输出',
    'Skip stopped / speed stale': '停车跳过 / 车速过期',
    'Mode': '模式',
    'Select a mode': '选择一个模式',
    'The selected mode behavior will appear here.': '所选模式的行为说明会显示在这里。',
    'Mode H Tuning': 'Mode H 调校',
    'Peak, wait, refractory and HO policy': '峰值、等待、不应期与 HO 策略',
    'Advanced Parameters': '高级参数',
    'ADVANCED PARAMETERS': '高级参数',
    'Timing, CAN IDs and torque values': '时序、CAN ID 与扭矩值',
    'NAG MODE': 'NAG 模式',
    'AP STATE ID': 'AP 状态 ID',
    'STEERING ID': '方向盘 ID',
    'TARGET ID': '目标 ID',
    'TORQUE TABLE · B2 / B3 / Nm': '扭矩表 · B2 / B3 / Nm',
    'HANDSON RATE %': 'HANDSON 占比 %',
    'BURST MS': '突发（毫秒）',
    'PAUSE MS': '暂停（毫秒）',
    'BEHAVIOR': '行为',
    'Add Torque': '叠加扭矩',
    'Choose injection behavior': '选择注入行为',
    'Reset Mode A': '重置模式 A',
    'Last TX block': '上次发送拦截',
    'TX block TWAI / send error': '发送拦截 TWAI / 发送错误',
    'TX block fresh / epoch / mutex': '发送拦截 新鲜 / 纪元 / 互斥',

    # ---- ULC（补漏）
    'ULC Blind Spot': 'ULC 盲点',
    'ULC Off-Highway': 'ULC 非高速',
    'Stock ULC Blind Spot': '原厂 ULC 盲点',
    'Select the ULC blind-spot configuration during AP': '选择 AP 期间的 ULC 盲点配置',
    'Set the off-highway ALC flag while AP is active': 'AP 激活期间设置非高速 ALC 标志',
    'Override the off-highway ULC flag during AP': 'AP 期间覆盖非高速 ULC 标志',
    'Off-Highway TX OK / FAIL': '非高速发送 成功 / 失败',
    'COMPOSITOR GATE': '合成器门控',
    'AGGRESSIVE': '激进',
    'STANDARD': '标准',
    'MAD MAX': '疯狂模式',

    # ---- S3XY 设备页（panelDevice）
    'S3XY Button': 'S3XY 按键',
    'DEVICE': '设备',
    'CONNECTION': '连接',
    'INPUT': '输入',
    'PROTOCOL': '协议',
    'RECONNECT': '重连',
    'BUTTON ACTIONS': '按键动作',
    'Connect': '连接',
    'Connect manually': '手动连接',
    'Forget Device': '忘记设备',
    'Rename': '重命名',
    'Retry': '重试',
    'Discover': '发现设备',
    'BLE connect': 'BLE 连接',
    'BLE address type': 'BLE 地址类型',
    'BLE state, button input counters and errors': 'BLE 状态、按键输入计数与错误',
    'Reconnect this button after boot or link loss.': '启动或断链后重连此按键。',
    'Global Auto Connect can be changed in Settings → Wireless.': '全局自动连接可在 设置 → 无线 中修改。',
    'Low-level BLE and input diagnostics. These values are not required for normal use.': '底层 BLE 与输入诊断。日常使用无需关注这些数值。',
    'Remove the saved registry entry and BLE bond.': '删除已保存的注册表项与 BLE 绑定。',
    'Current Bluetooth state': '当前蓝牙状态',
    'Auto Connect': '自动连接',
    'Notify subscription': '通知订阅',
    'Notify / unparsed': '通知 / 未解析',
    'Last notify': '上次通知',
    'Last path': '上次路径',
    'Raw state': '原始状态',
    'RSSI': 'RSSI',
    'Failures': '失败',
    'Error': '错误',
    'Secure': '安全连接',
    'Identity recovery': '身份恢复',
    'Identity probes / matches': '身份探测 / 匹配',
    'Identity rebinds / mismatches': '身份重绑 / 不匹配',
    'Saved 3D49 ID': '已存 3D49 ID',
    '3D49 NVS verified': '3D49 NVS 已校验',
    'Attempt → READY': '尝试 → 就绪',
    'B6 ACK': 'B6 应答',
    'Reconnects': '重连次数',
    'Single': '单击',
    'Double': '双击',
    'Long': '长按',
    'Single press': '单击',
    'Double press': '双击',
    'Long press': '长按',
    'None': '无',

    # ---- S3XY 配对向导（panelOta 内嵌）
    'Configure Button': '配置按键',
    'Connecting': '连接中',
    "Couldn't Connect": '无法连接',
    'CONNECTION ERROR': '连接错误',
    'Done': '完成',
    'Try Again': '重试',
    'OFFLINE': '离线',
    'SINGLE': '单击',
    'DOUBLE': '双击',
    'LONG': '长按',
    'S3XY Button Connected': 'S3XY 按键已连接',
    'S3XY Button Found': '找到 S3XY 按键',
    'Secure · Auto connect': '安全 · 自动连接',
    'A pairing-mode button was found. P4-CAN will start pairing automatically.': '发现处于配对模式的按键，P4-CAN 将自动开始配对。',
    'Keep the S3XY Button pressed while P4-CAN completes the secure connection. Release it after Connected appears.': '在 P4-CAN 完成安全连接期间按住 S3XY 按键，出现「已连接」后再松开。',
    'Press and HOLD the S3XY Button. Keep holding it until Connected appears. P4-CAN will search for the button and prepare the secure Bluetooth connection.': '按住 S3XY 按键不放，直到出现「已连接」。P4-CAN 会搜索按键并准备安全蓝牙连接。',
    'The button could not be prepared. No incomplete registry entry is kept after a failed pair.': '按键未能完成准备。配对失败后不会保留不完整的注册表项。',

    # ---- 诊断页（panelDiag）
    'BLE CSV': 'BLE 日志 CSV',
    'System Stats JSON': '系统统计 JSON',
    'Reset Stats': '重置统计',
    'Hard Reinitialize': '硬重置三路 CAN',
    'Hard reinitializations / failures': '硬重置次数 / 失败',
    'Last recovery reason': '上次恢复原因',
    'Recovery starts / failures': '恢复启动 / 失败',
    'Arduino-ESP32 core': 'Arduino-ESP32 核心',
    'BLE stack': 'BLE 协议栈',
    'BLE stack initialized': 'BLE 协议栈已初始化',
    'Bond persist still missing': '绑定持久化仍缺失',
    'Bond repair armed': '绑定修复已布防',
    'Bond repair attempts / READY': '绑定修复尝试 / 就绪',
    'Boot bonds / current': '启动绑定 / 当前',
    'Boot Capture CSV': '启动抓包 CSV',
    'Clear BLE Log': '清除 BLE 日志',
    'Bus errors / arbitration lost / TX failed': '总线错误 / 仲裁丢失 / 发送失败',
    'BUS OFF events': 'BUS OFF 次数',
    'Controller state / status age': '控制器状态 / 状态时效',
    'CAN mode / bitrate': 'CAN 模式 / 波特率',
    'CAN 1 · BODY': 'CAN 1 · 车身',
    'CAN 2 · CHASSIS': 'CAN 2 · 底盘',
    'CAN 3 · PARTY': 'CAN 3 · PARTY',
    'Hosted host / C6 version': 'HOSTED 主控 / C6 版本',
    'Network state': '网络状态',
    'Queue RX / TX': '队列 接收 / 发送',
    'RX missed / overrun': '接收丢失 / 溢出',
    'Receive calls / latest result': '接收调用 / 最近结果',
    'Received frames / last frame': '已收帧 / 最后一帧',
    'TEC / REC': 'TEC / REC',
    'TWAI controller / TX / RX': 'TWAI 控制器 / 发送 / 接收',
    'TX queued / rejected': '发送排队 / 拒绝',
    'S3XY / BLE': 'S3XY / BLE',
    'S3XY ULC TX OK / FAIL': 'S3XY ULC 发送 成功 / 失败',
    'S3XY ULC accepted / blocked': 'S3XY ULC 接受 / 拦截',
    'Last ULC result': '最近 ULC 结果',
    'Last auto trace': '最近自动轨迹',
    'Auto exact hits / misses': '自动精确命中 / 未命中',
    'Auto link failures': '自动连接失败',
    'RECOVERY · ALL THREE BUSES': '恢复 · 全部三路总线',
    'WI-FI / ESP-HOSTED': 'WI-FI / ESP-HOSTED',
    'Initialization': '初始化',

    # ---- 驾驶员监测抓包页散项（panelLabDriverMonitor）
    'Applied': '已下发',
    'Blocked': '已拦截',
    'Stored': '已保存',
    'Choose Setting': '选择设置项',
    'Select an option to apply it.': '选择一个选项即可应用。',
    'Endpoint layer': '端点层',
    'Endpoints': '端点',
    'Firmware status': '固件状态',
    'Next': '下一步',
    '1 beep': '1 声鸣笛',
    '2 beeps': '2 声鸣笛',
    '3 beeps': '3 声鸣笛',
    '2 flashes': '闪 2 次',
    '3 flashes': '闪 3 次',
    '5 flashes': '闪 5 次',
    '7 flashes': '闪 7 次',
    '2 default': '2 默认',
    '10 shifts': '10 次拨动',
    '3 shifts': '3 次拨动',
    '6 shifts': '6 次拨动',
    '7 entries': '7 条记录',
    '8 entries': '8 条记录',
    '9 entries': '9 条记录',
    '10 entries': '10 条记录',
    '11 entries': '11 条记录',
    '12 entries': '12 条记录',
    '2 entries': '2 条记录',
    '3 entries': '3 条记录',
    '4 entries': '4 条记录',
    '5 entries': '5 条记录',
    '6 entries': '6 条记录',

    # ---- 抓包窗口与时间选项（数字 + 单位）
    '0.1s': '0.1 秒', '0.2s': '0.2 秒', '0.3s': '0.3 秒', '0.4s': '0.4 秒',
    '0.5s': '0.5 秒', '0.6s': '0.6 秒', '0.7s': '0.7 秒', '0.8s': '0.8 秒',
    '0.9s': '0.9 秒', '1.0s': '1.0 秒',
    '20 ms': '20 毫秒', '50 ms': '50 毫秒', '80 ms': '80 毫秒',
    '100 ms': '100 毫秒', '150 ms': '150 毫秒', '200 ms': '200 毫秒',
    '300 ms': '300 毫秒', '500 ms': '500 毫秒', '1000 ms': '1000 毫秒',
    '1 s': '1 秒', '350 ms BURST': '350 毫秒突发', '2 ms WAIT': '2 毫秒等待',

    # ---- 无线页
    'BLUETOOTH DATA': '蓝牙数据',
    'Send B6 Handshake': '发送 B6 握手',
    'Dashboard address': '面板地址',
    '0 events': '0 个事件',
}

# EAP 防重启（更多功能页新增的独立开关）
ZH_EXTRA.update({
    '1 entry · saved in flash': '1 项 · 存入 flash',
    'EAP anti-restart': 'EAP 防重启',
    '0x331 · keep the patched DAS config on the bus from the first frame after every power-up, so the touchscreen no longer needs a restart. Works only with Banned Car + TLSSC Restore on.':
        '0x331 · 每次上电后从第一帧起就把补丁过的 DAS 配置铺在总线上，'
        '车机不用再重启。仅在「被封车辆」+「TLSSC 恢复」开启时工作。',
    'Waiting for firmware': '等待固件上报',
})

# 帧号 + 描述的组合串，逐条保留前缀、翻译后半段
FRAME_LABELS = {
    '0x239 View Range': '0x239 可视距离',
    '0x239 Virtual Lane Width': '0x239 虚拟车道宽度',
    '0x239 age —': '0x239 时效 —',
    '0x247 raw': '0x247 原始值',
    '0x247 · bus / age / count': '0x247 · 总线 / 时效 / 计数',
    '0x370 raw': '0x370 原始值',
    '0x370 · bus · EPAS hands-on / torque / age / count': '0x370 · 总线 · EPAS 握持 / 扭矩 / 时效 / 计数',
    '0x389 raw': '0x389 原始值',
    '0x389 · bus / age / count': '0x389 · 总线 / 时效 / 计数',
    '0x399 raw': '0x399 原始值',
    '0x399 · bus · DAS hands-on / age / count': '0x399 · 总线 · DAS 握持 / 时效 / 计数',
    '0x5D9 raw': '0x5D9 原始值',
    '0x5D9 · bus / age / count': '0x5D9 · 总线 / 时效 / 计数',
}
ZH_EXTRA.update(FRAME_LABELS)


def inject_dyn(block):
    """把运行时状态兜底脚本放进 P4CAN-ZH 块（幂等）。"""
    if DYN_START in block:
        return re.sub(re.escape(DYN_START) + r'.*?' + re.escape(DYN_END),
                      lambda _m: DYN_JS, block, flags=re.S), False
    return block.rstrip('\n') + '\n' + DYN_JS + '\n', True


def main():
    html = DASH.read_text(encoding='utf-8')
    i, j = html.find(START), html.find(END)
    if i < 0 or j < 0:
        raise SystemExit('找不到 P4CAN-ZH 块，先确认页面还是中文语言版')
    block = html[i:j]

    m = re.search(r'var DICT=(\{.*?\});\s*\n', block, re.S)
    if not m:
        raise SystemExit('P4CAN-ZH 块里找不到 var DICT={...};')
    old = json.loads(m.group(1))

    added, skipped = [], []
    merged = dict(old)
    # 这些 key 的旧译法跟本轮改过的界面文案冲突，允许直接覆盖。
    FORCE_UPDATE = {
        'Theme · display · local options',
        'Theme, display and local options.',
        'Off-road · ESP · SERVICE',
        '5 groups',
    }
    for en, zh in ZH_EXTRA.items():
        if en in merged:
            if merged[en] != zh:
                if en in FORCE_UPDATE:
                    merged[en] = zh
                    added.append(en)
                else:
                    skipped.append((en, merged[en], zh))
            continue
        merged[en] = zh
        added.append(en)

    block2 = block
    if added:
        payload = json.dumps(merged, ensure_ascii=False, separators=(',', ': '))
        block2 = (block[:m.start()] + 'var DICT=' + payload + ';\n' + block[m.end():])

    block2, dyn_new = inject_dyn(block2)

    if block2 == block:
        print('中文包已是最新：%d 条词条，动态兜底已就位' % len(merged))
        if skipped:
            print('注意：%d 条已存在且译法不同（保留原译）' % len(skipped))
            for en, keep, want in skipped[:8]:
                print('  %-50s 保留 %r / 建议 %r' % (en[:50], keep, want))
        return

    DASH.write_text(html[:i] + block2 + html[j:], encoding='utf-8')

    print('中文词条  %d -> %d（新增 %d）' % (len(old), len(merged), len(added)))
    if dyn_new:
        print('注入运行时状态兜底：%d 个节点' % 5)
    if skipped:
        print('已存在但译法不同，保留原译：%d 条' % len(skipped))
        for en, keep, want in skipped[:8]:
            print('  %-50s 保留 %r' % (en[:50], keep))


if __name__ == '__main__':
    main()
