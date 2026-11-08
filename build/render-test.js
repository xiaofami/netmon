/*
 * 前端运行时自检（无需浏览器）：用极简 DOM 桩真实执行 index.html 的脚本，
 * 逐个切换视图、捕获运行时异常、断言关键容器确实渲染出内容。
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'www/index.html'), 'utf8');
const code = /<script>([\s\S]*?)<\/script>/.exec(html)[1];
/* 数据源优先用脱敏后的 demo.html（仓库里就有），这样测试可离线复现；
 * 只有在本地跑 make-preview.py 抓了真实数据后才用 preview-live.html。
 * 两者结构完全一致，断言不区分。 */
function pickData() {
  for (const f of ['www/demo.html', 'www/preview-live.html']) {
    const p = path.join(root, f);
    if (fs.existsSync(p)) { console.log('数据源: ' + f); return fs.readFileSync(p, 'utf8'); }
  }
  throw new Error('缺少 www/demo.html —— 先跑 python3 build/make-demo.py 生成脱敏数据');
}
const demoHtml = pickData();
const demo = JSON.parse(/var NETMON_DEMO=(\{.*?\});<\/script>/s.exec(demoHtml)[1].replace(/<\\\//g, '</'));

const els = new Map();
const listenerMap = {};
function mkEl(id) {
  const attrs = {};
  if (id) attrs.id = id;
  const e = {
    _id: id, innerHTML: '', textContent: '', value: '', className: '',
    style: {}, dataset: {},
    classList: {
      add(c) { e.className += ' ' + c; },
      remove(c) { e.className = e.className.replace(c, '').trim(); },
      toggle(c, on) {
        if (on === undefined) on = !e.classList.contains(c);
        if (on) e.classList.add(c); else e.classList.remove(c);
      },
      contains(c) { return e.className.indexOf(c) >= 0; }
    },
    getAttribute(k) { return attrs[k] !== undefined ? attrs[k] : null; },
    setAttribute(k, v) { attrs[k] = v; },
    addEventListener() {}, closest() { return null; },
    appendChild() {}, focus() {},
    onclick: null, oninput: null, onchange: null, onkeydown: null
  };
  return e;
}
function el(id) { if (!els.has(id)) els.set(id, mkEl(id)); return els.get(id); }
function navEls(attr, values) {
  return values.map(v => { const e = mkEl(''); e.getAttribute = k => k === attr ? v : null; return e; });
}
const views = ['overview','clients','client','blocks','traffic','queries','domains','system','detail']
  .map(v => { const e = mkEl('v-' + v); e.getAttribute = k => k === 'data-nav' ? v : null; return e; });

const document = {
  documentElement: mkEl('html'), body: mkEl('body'),
  getElementById: id => el(id),
  querySelectorAll(sel) {
    if (sel === '[data-nav]') return navEls('data-nav', ['overview','clients','blocks','traffic','queries','domains','system']);
    if (sel === '[data-since]') return navEls('data-since', ['3600','86400','604800']);
    if (sel === '[data-f]') return navEls('data-f', ['all','active','static','auto','fav']);
    if (sel === '[data-l]') return navEls('data-l', ['card','list']);
    if (sel === '[data-r]') return navEls('data-r', ['all','0','3','5']);
    if (sel === '[data-c]') return navEls('data-c', ['all','ok','1','2','3']);
    if (sel === '[data-s]') return navEls('data-s', ['total','down','up']);
    if (sel === '.view') return views;
    return [];
  },
  addEventListener(type, fn) {
    (listenerMap[type] = listenerMap[type] || []).push(fn);
  }
};
const location = { hash: '', pathname: '/', hostname: '192.168.1.1', protocol: 'http:' };
const sandbox = {
  document, location,
  window: { addEventListener() {}, scrollTo() {}, devicePixelRatio: 1 },
  localStorage: { getItem: () => null, setItem: () => {} },
  setInterval: () => 1, clearInterval: () => {},
  setTimeout: (f) => { f(); return 1; }, clearTimeout: () => {},
  NETMON_DEMO: demo,
  XMLHttpRequest: function () { throw new Error('预览模式不该发 XHR'); },
  console
};

let fail = 0;
function check(name, cond, extra) {
  console.log((cond ? '  [ok] ' : '  [FAIL] ') + name + (extra ? '  ' + extra : ''));
  if (!cond) fail++;
}
function has(id, needle) { return el(id).innerHTML.indexOf(needle) >= 0; }
function go(hash) { location.hash = hash; vm.runInNewContext('navigate();', sandbox); }

try {
  vm.runInNewContext(code, sandbox, { filename: 'index.html:script' });
} catch (e) {
  console.error('脚本执行抛异常：', e.stack);
  process.exit(1);
}

console.log('\n== 初始：总览 ==');
check('KPI 渲染', has('kpi-grid', 'DNS 查询'));
check('活跃客户端 KPI', has('kpi-grid', '活跃客户端'));
check('DNS 趋势图有柱子', (el('chart-dns').innerHTML.match(/bcol/g) || []).length > 5);
check('流量趋势图有柱子', (el('chart-traffic').innerHTML.match(/bcol/g) || []).length > 0);
check('设备榜有下钻项', (el('ov-clients').innerHTML.match(/data-goto=/g) || []).length > 0);
check('域名榜有内容', has('ov-domains', '.'));
check('拦截概览取代线路质量', has('ov-blocked', 'class="row') || has('ov-blocked', '暂无'));
check('最近查询有设备名', has('ov-recent', 'data-goto'));
check('状态为实时', el('status-text').textContent === '实时');
check('一句话摘要有结论', has('digest', '过去') && has('digest', '次域名查询'));
check('一句话摘要区分拦截与真实失败', has('digest', '广告规则拦截') && has('digest', '解析失败'));
check('站内告警铃铛已渲染', has('btn-alerts', '<svg'));
check('告警面板有结论', has('alert-pop', '站内告警'));
check('一周热力图 7×24 格', (el('heat-grid').innerHTML.match(/heat-cell/g) || []).length === 168);
check('热力图有星期标签', has('heat-grid', '周一') && has('heat-grid', '周日'));
check('热力图小时轴', has('heat-axis', '21'));
check('热力图说明不绑定时间范围', el('heat-sub').textContent.indexOf('7 天') >= 0);
check('平均响应 KPI 不再指向已删视图', !has('kpi-grid', '#/network'));

console.log('\n== 客户端列表 ==');
go('#/clients');
const devCount = demo.dashboard.devices.length;
check('卡片渲染全部设备', (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length === devCount,
  (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length + ' 张');
check('NAS-1 有名字', has('clients-grid', 'NAS-1'));
check('静态分配徽章', has('clients-grid', '静态地址分配'));
check('卡片不再写「查看详情」', !has('clients-grid', '查看详情'));
/* 该状态依赖实时数据：设备拿到新 IP 后就不再是 stale。
   有样本时断言必须渲染出来，没样本时跳过，避免数据变化导致假失败。 */
const staleCount = demo.dashboard.devices.filter(x => x.stale_static).length;
check('静态未生效提示', staleCount ? has('clients-grid', '静态未生效') : true,
  staleCount ? staleCount + ' 台样本' : '当前无样本，跳过');
check('多网卡徽章', has('clients-grid', '多网卡'));
check('列表视图有内容', has('clients-list', 'NAS-1'));
check('计数条正常', has('clients-count', '<b>' + devCount + '</b>'));
check('卡片有收藏星标', (el('clients-grid').innerHTML.match(/data-star=/g) || []).length >= devCount);
check('工具栏有只看收藏', html.indexOf('只看收藏') >= 0);

console.log('\n== 设备详情（含流量趋势） ==');
go('#/client/' + demo.dashboard.devices[0].device.ip);
check('hero 显示设备名', has('detail-head', demo.dashboard.devices[0].device.display));
check('hero 显示 MAC', /[0-9A-F]{2}(:[0-9A-F]{2}){5}/.test(el('detail-head').innerHTML));
check('详情 KPI', has('detail-kpi', '不同域名'));
check('查询活动曲线', (el('detail-timeline').innerHTML.match(/bcol/g) || []).length > 5);
check('设备流量趋势图', (el('detail-flow').innerHTML.match(/bcol/g) || []).length > 0);
check('流量趋势有合计', el('detail-flow-total').textContent.indexOf('合计') >= 0);
check('常访域名有内容', (el('detail-domains').innerHTML.match(/class="row/g) || []).length > 0);
check('环形图', has('detail-donut', '<svg'));
check('流量目的地有内容', (el('detail-traffic').innerHTML.match(/class="row/g) || []).length > 0);
check('查询日志分页每页 15 条', (el('detail-recent').innerHTML.match(/class="row g-log2"/g) || []).length === 15);
check('三个榜单都有分页条', has('pg-domains', '条') && has('pg-traffic', '条') && has('pg-recent', '共'));
check('页面标题跟随设备', el('page-title').textContent === demo.dashboard.devices[0].device.display);
check('设备详情有备注框', has('client-note', 'data-notetype'));
check('设备详情备注框带星标', has('client-note', 'data-star'));

console.log('\n== 切换设备不残留旧数据 ==');
const devA = demo.dashboard.devices[0].device;
const devB = (demo.dashboard.devices.filter(d => d.device.ip !== devA.ip)[0] || {}).device;
if (devB) {
  go('#/client/' + devB.ip);
  check('hero 换成另一台', has('detail-head', devB.display));
  check('不再显示上一台', !has('detail-head', devA.display));
} else {
  check('demo 里至少两台设备', false);
}

console.log('\n== 拦截与规则 ==');
go('#/blocks');
check('拦截 KPI 渲染', has('blocks-kpi', '广告 / 规则拦截'));
check('规则清单含 BlockAD', has('blocks-rules', 'BlockAD'));
check('规则同步状态', has('blocks-hpinfo', '实时同步'));
check('被拦截域名榜有内容', (el('blocks-blocked').innerHTML.match(/class="row/g) || []).length > 0);
check('真实异常榜', (el('blocks-unresolved').innerHTML.match(/class="row/g) || []).length > 0);
check('规则变更历史', has('blocks-events', '配置变更') || has('blocks-events', '初始加载'));
check('不再有代理可达性探测', !has('blocks-kpi', '建议加代理规则'));
check('导航改为拦截记录', has('nav-blocks', '拦截记录'));
check('页标题改为拦截规则', el('page-title').textContent === '拦截规则');
check('「排入探测」已删除', !has('blocks-unresolved', '排入探测'));
check('解析失败榜显示占比', has('blocks-unresolved', '%'));
check('被拦截域名榜有分页', has('pg-blocked', '共'));
check('解析失败榜有分页', has('pg-unresolved', '共'));
check('判读说明不再提代理探测', !has('blocks-note', '代理探测队列'));
check('判读说明讲清三类异常', has('blocks-note', '本地域'));
check('拦截占比趋势有柱子', (el('block-trend').innerHTML.match(/pct-bar/g) || []).length > 0);
check('占比趋势有解读', has('block-trend-note', '拦截应答占'));
check('被拦截最多设备榜', has('block-dev', 'data-goto'));
check('被拦截设备榜有分页条', has('pg-blockdev', '共'));
check('拦截榜翻页内容会变化', (function () {
  const first = el('blocks-blocked').innerHTML;
  vm.runInNewContext('S.page.blocked=2;renderView();', sandbox);
  const second = el('blocks-blocked').innerHTML;
  vm.runInNewContext('S.page.blocked=1;renderView();', sandbox);
  return first !== second && second.length > 0;
})());

console.log('\n== 流量排行 ==');
go('#/traffic');
check('流量 KPI 渲染', has('traffic-kpi', '总流量'));
check('设备流量榜可点', has('traffic-dev', 'data-goto'));
check('域名 IP 流量榜', has('traffic-host', 'pill'));
check('不再有服务大类分布', !has('traffic-kpi', '最耗流量的服务'));
check('下载最多的设备榜', has('rank-dev-down', 'class="row'));
check('上传最多的设备榜', has('rank-dev-up', 'class="row'));
check('下载最多的目标榜', has('rank-host-down', 'data-detail'));
check('上传最多的目标榜', has('rank-host-up', 'data-detail'));
check('目的网段', has('traffic-cidr', 'class="row') || has('traffic-cidr', '暂无'));

console.log('\n== 其余视图 ==');
go('#/queries');
check('查询日志表格', (el('queries-body').innerHTML.match(/data-goto/g) || []).length > 10);
check('状态说明', has('queries-note', 'NXDOMAIN'));
go('#/domains');
check('DNS 排行', has('domains-dns', 'pill'));
check('流量排行', has('domains-traffic', 'bar'));
go('#/system');
check('系统状态 KPI', has('sys-kpi', '运行时间'));
check('接口统计表', has('sys-iface', 'class="row'));
check('连接状态摘要', has('sys-conn', 'ESTABLISHED'));
check('每日流量总量', has('sys-daily', 'class="row') || has('sys-daily', '暂无'));
check('数据来源说明', has('system-src', 'AF_PACKET'));
check('常用命令', has('system-cmd', 'netmon-report client'));
go('#/overview');
check('返回总览仍正常', has('kpi-grid', 'DNS 查询'));

console.log('\n== 通用条目详情（下钻） ==');
go('#/detail/host/1.1.1.1');
check('关联设备表可继续下钻', has('d2-devices', 'data-goto'));
check('连接明细有内容', (el('d2-conns').innerHTML.match(/class="row/g) || []).length > 0);
check('分时段流量图', (el('d2-timeline').innerHTML.match(/bcol/g) || []).length > 0);
check('详情说明卡', has('d2-note', '下钻'));
go('#/detail/qname/v2mi.gdt.qq.com');
check('DNS 明细有内容', (el('d2-dns').innerHTML.match(/class="row/g) || []).length > 0);
check('DNS 详情按小时查询趋势', (el('d2-timeline').innerHTML.match(/bcol/g) || []).length > 0);
check('DNS 详情 KPI 用次数', has('d2-kpi', '查询总数'));
check('切换条目不残留旧数据', !has('d2-conns', 'class="row'));
check('通用详情有备注框', has('detail-note', 'data-notetype'));
check('通用详情不再有服务判断', !has('d2-kpi', '服务判断'));

console.log('\n== 本轮新功能：收藏 / 备注 / 告警 ==');
check('收藏切换能过滤列表', (function () {
  vm.runInNewContext("S.filter='fav';S.q='';renderView();", sandbox);
  go('#/clients');
  const zero = (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length;
  vm.runInNewContext("favToggle('ip','192.168.2.242');", sandbox);
  const one = (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length;
  return zero === 0 && one === 1;
})());
check('收藏状态显示在卡片上', has('clients-grid', 'star on'));
check('备注可保存并显示在卡片', (function () {
  vm.runInNewContext("noteSet('ip','192.168.2.242','客厅 NAS');S.filter='all';renderView();", sandbox);
  return has('clients-grid', '客厅 NAS');
})());
check('备注参与搜索', (function () {
  vm.runInNewContext("S.q='客厅';", sandbox);
  go('#/clients');
  return has('clients-grid', 'NAS-1') &&
         (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length === 1;
})());
check('告警结论基于真实指标', has('alert-pop', '站内告警'));
check('告警只提示不改配置', has('alert-pop', '不会改动任何配置'));
go('#/overview');
check('从详情返回总览正常', has('kpi-grid', 'DNS 查询'));

console.log('\n== 翻页、筛选、搜索与皮肤 ==');
go('#/client/192.168.2.242');
const p1 = el('detail-recent').innerHTML;
vm.runInNewContext("S.page.recent=2;renderView();", sandbox);
check('翻到第 2 页内容变化', el('detail-recent').innerHTML !== p1);
check('页码高亮第 2 页', has('pg-recent', 'class="on">2<'));
vm.runInNewContext("S.page.recent=1;renderView();", sandbox);
go('#/queries');
check('查询日志分页条', has('pg-queries', '共'));
const qall = el('queries-body').innerHTML;
vm.runInNewContext("S.cls='1';S.page.queries=1;renderView();", sandbox);
check('归因筛选生效', el('queries-body').innerHTML !== qall);
vm.runInNewContext("S.cls='all';S.qq='';renderView();", sandbox);
go('#/clients');
vm.runInNewContext("S.csort='name';renderView();", sandbox);
check('按名称排序可渲染', has('clients-grid', 'dcard'));
vm.runInNewContext("S.csort='queries';", sandbox);
vm.runInNewContext("globalSearch('1.1.1.1');", sandbox);
check('全局搜索 IP 跳转', location.hash.indexOf('1.1.1.1') >= 0);
vm.runInNewContext("globalSearch('gdt.qq.com');", sandbox);
check('全局搜索域名跳转', location.hash.indexOf('qname') >= 0);
vm.runInNewContext("S.theme='nord';applyTheme();", sandbox);
check('皮肤切换生效', vm.runInNewContext("document.documentElement.getAttribute('data-theme')", sandbox) === 'nord');
vm.runInNewContext("S.theme='dark';applyTheme();", sandbox);
check('CSV 导出函数存在', vm.runInNewContext("typeof exportCurrent", sandbox) === 'function');

console.log('\n== 交互：告警下拉与时间范围 ==');
const fire = (type, ev) => (listenerMap[type] || []).forEach(fn => fn(ev));
const clickOutside = { target: { closest: () => null } };
const clickInside  = { target: { closest: (s) => s === '#alert-pop' ? {} : null } };
const clickBell    = { target: { closest: (s) => s === '#btn-alerts' ? {} : null } };
const alertOpen = () => vm.runInNewContext(
  "document.getElementById('alert-pop').classList.contains('on')", sandbox);
const alertReset = () => vm.runInNewContext(
  "$('alert-pop').classList.remove('on');", sandbox);

alertReset();
vm.runInNewContext("$('btn-alerts').onclick();", sandbox);
check('铃铛可点开告警', alertOpen());
fire('click', clickOutside);
check('点击页面其它位置即关闭', !alertOpen());
vm.runInNewContext("$('btn-alerts').onclick();", sandbox);
fire('click', clickBell);
check('点铃铛自身不被冒泡误关', alertOpen());
fire('click', clickInside);
check('点面板内部保持打开', alertOpen());
fire('keydown', { key: 'Escape' });
check('Esc 收起告警', !alertOpen());
check('时间范围改为下拉选择', html.indexOf('<select class="sel" id="since-custom"') >= 0);
check('不再有 number 输入框', html.indexOf('id="since-custom" type="number"') < 0);
check('下拉含常用范围', html.indexOf('value="21600">6 小时') >= 0 &&
                        html.indexOf('value="2592000">30 天') >= 0);
check('选择更多范围即时生效', (function () {
  vm.runInNewContext("$('since-custom').value='172800';$('since-custom').onchange();", sandbox);
  return vm.runInNewContext('S.since', sandbox) === 172800;
})());
check('选择后页码归位', vm.runInNewContext('S.page.blocked', sandbox) === 1);
/* 还原，避免影响后续断言 */
vm.runInNewContext("S.since=86400;S.q='';S.filter='all';", sandbox);

console.log('\n== 部署路径自适应（LuCI 同源 vs 独立端口） ==');
/* 这条是 LuCI 内嵌面板加载失败那次的回归测试：
   页面在 /netmon/ 下必须用 /cgi-bin/netmon-data，否则取不到数据。 */
check('独立端口模式用 /cgi-bin/data',
  vm.runInNewContext('API', sandbox) === '/cgi-bin/data',
  vm.runInNewContext('API', sandbox));
(function () {
  const before = vm.runInNewContext('apiBase()', sandbox);
  location.pathname = '/netmon/index.html';
  const inLuci = vm.runInNewContext('apiBase()', sandbox);
  location.pathname = '/';
  check('LuCI 同源模式切到 /cgi-bin/netmon-data',
        inLuci === '/cgi-bin/netmon-data', inLuci);
  check('两种模式基址不同', before !== inLuci);
  check('回到根路径恢复 /cgi-bin/data',
        vm.runInNewContext('apiBase()', sandbox) === '/cgi-bin/data');
})();

console.log('\n== 交互：时间范围与搜索 ==');
vm.runInNewContext("S.since=604800; load(); renderView();", sandbox);
check('7 天范围可渲染', has('kpi-grid', 'DNS 查询'));
const anyDev = demo.dashboard.devices[0].device;
const key = String(anyDev.display).split(/[-\s]/)[0].slice(0, 4).toLowerCase();
const countCards = () => (el('clients-grid').innerHTML.match(/<div class="dcard[ "]/g) || []).length;
go('#/clients');
const before = countCards();
vm.runInNewContext("S.q=" + JSON.stringify(key) + "; renderView();", sandbox);
const after = countCards();
check('搜索过滤生效', after > 0 && after < before, key + ' → ' + after + '/' + before);

console.log('\n' + (fail ? '✗ ' + fail + ' 项失败' : '✓ 全部渲染检查通过'));
process.exit(fail ? 1 : 0);
