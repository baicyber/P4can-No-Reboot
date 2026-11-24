# -*- coding: utf-8 -*-
"""「更多功能」页可用性锁。

页面上只有部分行渲染了 P4 状态徽标，靠读 DOM 会漏掉一半条目。这里在构建时
把 data/feifan-menu-data.json 的 id -> p4 状态内联进脚本，按控件 id 精确匹配：
需新增 / 不可达 -> 控件 disabled + 置灰 + 标「需固件实现」；已有 / 本地UI -> 可用。
另加顶部统计与「仅看可用」筛选。幂等，按 P4CAN-FFLOCK 标记替换。
"""
import json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent
DASH = ROOT / 'firmware/P4can-unlock-more/dashboard_source.html'
DATA = ROOT / 'data/feifan-menu-data.json'

d = json.loads(DATA.read_text(encoding='utf-8'))
status = {}
for g in d['groups']:
    for s in g['sections']:
        for it in s['items']:
            status[it['id']] = it['p4']
KIND = {'需新增': 'new', '不可达': 'no', '已有': 'have', '本地UI': 'ui'}
p4map = {k: KIND.get(v, 'have') for k, v in status.items()}
cnt = {}
for v in p4map.values():
    cnt[v] = cnt.get(v, 0) + 1
print('数据统计:', cnt, '合计', sum(cnt.values()))

CSS = """<!--P4CAN-FFLOCK-CSS-START-->
body .ffScope .ffStats{display:flex;align-items:center;gap:7px;flex-wrap:wrap;margin-top:10px}
body .ffScope .ffStatPill{display:inline-flex;align-items:center;gap:5px;font-size:11px;font-weight:600;line-height:1;padding:7px 10px;border-radius:999px;border:1px solid var(--line);color:var(--muted)}
body .ffScope .ffStatPill b{font-size:12px;font-weight:700;color:var(--ink)}
body .ffScope .ffStatPill.ok b{color:var(--green)}
body .ffScope .ffStatPill.new b{color:var(--blue)}
body .ffScope .ffStatPill.no b{color:var(--red)}
body .ffScope .ffFilter{display:inline-flex;gap:0;margin-left:auto;border:1px solid var(--line);border-radius:999px;overflow:hidden}
body .ffScope .ffFilter button{font-family:inherit;font-size:11px;font-weight:600;line-height:1;padding:7px 11px;background:transparent;border:0;color:var(--muted)}
body .ffScope .ffFilter button[aria-pressed="true"]{background:var(--ink);color:var(--bg)}
body .ffScope .listrow.ffLock{opacity:.42}
body .ffScope .listrow.ffLock .toggle,body .ffScope .listrow.ffLock .settingChoiceButtons,body .ffScope .listrow.ffLock .ffBtn,body .ffScope .listrow.ffLock .ffRead{pointer-events:none}
body .ffScope .listrow.ffLock .listkey::after{content:attr(data-fflock);display:inline-block;margin-left:6px;padding:1px 5px;border:1px solid var(--line);border-radius:4px;font-size:9.5px;font-weight:600;color:var(--muted2);vertical-align:1.5px}
body .ffScope.ffOnlyUsable .listrow.ffLock{display:none}
body .ffScope.ffOnlyUsable .sectionhead.ffHide,body .ffScope.ffOnlyUsable section.card.ffHide{display:none}
<!--P4CAN-FFLOCK-CSS-END-->"""

JS_TMPL = """<!--P4CAN-FFLOCK-JS-START-->
<script>
(function(){
  if(document.querySelector('.ffScope[data-fflock]'))return;
  var P4=%s;
  var scopes=[].slice.call(document.querySelectorAll('.ffScope'));
  if(!scopes.length)return;
  scopes.forEach(function(s){s.setAttribute('data-fflock','1');});
  function isZh(){try{return localStorage.getItem('p4canLang')!=='en';}catch(e){return true;}}
  var T={zh:{ok:'可用',need:'需新增',no:'不可达',all:'全部',only:'仅看可用',lock:'需固件实现'},
         en:{ok:'usable',need:'to build',no:'unreachable',all:'All',only:'Usable',lock:'needs firmware'}};

  var rows=[].slice.call(document.querySelectorAll('.panel .listrow'));
  var n={ok:0,need:0,no:0};
  rows.forEach(function(row){
    var ctl=row.querySelector('[data-ff],[data-ff-seg],[data-ff-act],[data-ff-read]');
    var st=null;
    if(ctl){
      var id=ctl.getAttribute('data-ff')||ctl.getAttribute('data-ff-seg')||
             ctl.getAttribute('data-ff-act')||ctl.getAttribute('data-ff-read');
      st=P4[id]||null;
    }else if(row.querySelector('.ffTag.p4new'))st='new';
    else if(row.querySelector('.ffTag.p4no'))st='no';
    else if(row.querySelector('.ffTag.p4have'))st='have';
    else if(row.querySelector('.ffTag.p4ui'))st='ui';
    if(!st)return;
    if(st!=='new'&&st!=='no'){n.ok++;return;}
    row.classList.add('ffLock');
    if(st==='new')n.need++;else n.no++;
    [].slice.call(row.querySelectorAll('input,button,select')).forEach(function(el){el.disabled=true;});
    row.setAttribute('aria-disabled','true');
  });

  var page=document.querySelector('.page.ffScope')||scopes[0];
  var host=page.querySelector('.ffSrc')||page.querySelector('.pageSub');
  var bar=document.createElement('div');
  bar.className='ffStats';
  if(host&&host.parentNode===page)page.insertBefore(bar,host.nextSibling);else page.appendChild(bar);

  function syncSections(){
    var on=scopes.some(function(s){return s.classList.contains('ffOnlyUsable');});
    [].slice.call(document.querySelectorAll('.panel.ffScope')).forEach(function(p){
      [].slice.call(p.querySelectorAll('.sectionhead')).forEach(function(h){
        var card=h.nextElementSibling; if(!card)return;
        var total=card.querySelectorAll('.listrow').length,
            hidden=card.querySelectorAll('.listrow.ffLock').length;
        var empty=on&&total>0&&hidden===total;
        h.classList.toggle('ffHide',empty); card.classList.toggle('ffHide',empty);
      });
    });
  }
  function paint(){
    var t=T[isZh()?'zh':'en'];
    bar.innerHTML='<span class="ffStatPill ok"><b>'+n.ok+'</b>'+t.ok+'</span>'+
      '<span class="ffStatPill new"><b>'+n.need+'</b>'+t.need+'</span>'+
      (n.no?'<span class="ffStatPill no"><b>'+n.no+'</b>'+t.no+'</span>':'')+
      '<span class="ffFilter"><button type="button" data-ff-filter="all">'+t.all+'</button>'+
      '<button type="button" data-ff-filter="only">'+t.only+'</button></span>';
    var cur=scopes[0].classList.contains('ffOnlyUsable')?'only':'all';
    bar.querySelectorAll('[data-ff-filter]').forEach(function(b){
      b.setAttribute('aria-pressed', b.getAttribute('data-ff-filter')===cur?'true':'false');
    });
    [].slice.call(document.querySelectorAll('.panel .listrow.ffLock .listkey')).forEach(function(k){
      k.setAttribute('data-fflock',t.lock);
    });
    syncSections();
  }
  bar.addEventListener('click',function(e){
    var b=e.target.closest('[data-ff-filter]'); if(!b)return;
    var on=b.getAttribute('data-ff-filter')==='only';
    scopes.forEach(function(s){s.classList.toggle('ffOnlyUsable',on);});
    paint();
  });
  document.addEventListener('click',function(e){
    if(e.target.closest&&e.target.closest('[data-lang-choice]'))setTimeout(paint,60);
  });
  paint();
})();
</script>
<!--P4CAN-FFLOCK-JS-END-->"""

JS = JS_TMPL % json.dumps(p4map, separators=(',', ':'), ensure_ascii=False)

html = DASH.read_text(encoding='utf-8')
html = re.sub(r'<!--P4CAN-FFLOCK-CSS-START-->[\s\S]*?<!--P4CAN-FFLOCK-CSS-END-->', '', html)
html = re.sub(r'<!--P4CAN-FFLOCK-JS-START-->[\s\S]*?<!--P4CAN-FFLOCK-JS-END-->', '', html)

anchor = '/*P4CAN-FEIFAN-CSS-END*/'
i = html.rfind(anchor)
if i < 0:
    sys.exit('找不到 FEIFAN CSS 结束标记')
html = html[:i + len(anchor)] + '\n' + CSS + html[i + len(anchor):]

j = html.rfind('</body>')
if j < 0:
    sys.exit('找不到 </body>')
html = html[:j] + JS + '\n' + html[j:]

DASH.write_text(html, encoding='utf-8')
print('patched ->', len(html), 'chars')
