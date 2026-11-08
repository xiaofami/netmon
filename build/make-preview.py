#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成离机预览页 preview-live.html：
把线上真实 dashboard + 几台设备的 client 详情 + 五类通用条目详情
内嵌进 index.html，通过 NETMON_DEMO 钩子让 xhr 直接返回本地数据，
file:// 打开即可完整交互（包括所有榜单下钻）。

用途：不用连路由器也能看面板效果、给用户看交付物、无头浏览器验证。
"""
import json
import urllib.request
from pathlib import Path
from urllib.parse import quote

BASE = 'http://192.168.1.1:8090/cgi-bin/data'
ROOT = Path('/home/pico/WorkBuddy/immortalwrt/netmon/www')


def get(url):
    with urllib.request.urlopen(url, timeout=20) as r:
        return json.loads(r.read().decode('utf-8'))


dash = get(BASE + '?since=86400')
ips = ['192.168.1.101', '192.168.1.102', '192.168.1.100']
clients = {}
for ip in ips:
    try:
        clients[ip] = get(BASE + '?view=client&ip=' + ip + '&since=86400')
    except Exception as e:
        print('  [warn] %s: %s' % (ip, e))

# 五类实体各取一个真实样本，保证预览里点任何条目都能看到正确版式
cases = [
    ('host', '1.1.1.1'),
    ('qname', (dash.get('dns') or [{'q': 'example.com'}])[0]['q']),
    ('cidr', (dash.get('top_cidr') or [{'k': '0.0.0.0/0'}])[0]['k']),
    ('app', (dash.get('app_stats') or [{'cat': '其他'}])[0]['cat']),
    ('verdict', 'need_proxy'),
]
details = {}
for t, k in cases:
    try:
        details[t + '|' + k] = get(BASE + '?view=detail&type=' + t +
                                   '&key=' + quote(str(k), safe='') + '&since=86400')
    except Exception as e:
        print('  [warn] detail %s/%s: %s' % (t, k, e))

demo = {
    'dashboard': dash,
    'client': clients.get(ips[0], {}),
    'clients': clients,
    'details': details,
    'detail': details.get('host|1.1.1.1', {}),
}
blob = json.dumps(demo, ensure_ascii=False, separators=(',', ':'))
# 防止数据里出现 </script> 提前闭合脚本
blob = blob.replace('</', '<\\/')

html = (ROOT / 'index.html').read_text(encoding='utf-8')
inject = '<script>var NETMON_DEMO=' + blob + ';</script>\n'
marker = '<script>\n\'use strict\';'
if marker not in html:
    raise SystemExit('找不到 <script> 注入点')
html = html.replace(marker, inject + marker, 1)

out = ROOT / 'preview-live.html'
out.write_text(html, encoding='utf-8')
print('生成 %s (%d KB)' % (out, out.stat().st_size // 1024))
print('  设备 %d 台，内嵌设备详情 %d 台，小时数据点 %d 个，条目详情 %d 类' %
      (len(dash['devices']), len(clients), len(dash['dns_timeline']), len(details)))
