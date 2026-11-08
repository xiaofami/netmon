#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""前端静态自检：抽出 <script> 交给 node --check，并比对 DOM id 与 JS 引用。"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
P = ROOT / 'www' / 'index.html'
html = P.read_text(encoding='utf-8')

m = re.search(r'<script>(.*?)</script>', html, re.S)
if not m:
    sys.exit('没找到 <script>')
js = m.group(1)
jsfile = ROOT / 'build' / '.check-extracted.js'
jsfile.write_text(js, encoding='utf-8')

node = shutil.which('node')
if not node:
    sys.exit('找不到 node，请先安装 Node.js')
r = subprocess.run([node, '--check', str(jsfile)], capture_output=True, text=True)
print('node --check :', 'PASS' if r.returncode == 0 else 'FAIL')
if r.returncode:
    print(r.stderr[:3000])
    sys.exit(1)

# HTML 里静态存在的 id
html_ids = set(re.findall(r'\bid="([^"]+)"', html))
# JS 里动态生成的 id（innerHTML 字符串里的 id="xxx"）
dyn_ids = set(re.findall(r'\\?id=\\?"([^"\\]+)\\?"', js))
# searchBox() 用字符串拼接生成 <input id="<容器 id>-in">
for sid in re.findall(r"searchBox\('([^']+)'", js):
    dyn_ids.add(sid + '-in')
# JS 里引用的 id
refs = set(re.findall(r"\$\('([^']+)'\)", js))
refs |= set(re.findall(r"getElementById\('([^']+)'\)", js))
# 通过 helper 传 id 的调用也要算引用
for helper in ('rowsHtml', 'setHead', 'searchBox'):
    refs |= set(re.findall(helper + r"\('([^']+)'", js))
# nav-xxx 由循环拼接，单独补
refs |= {'nav-' + k for k in re.findall(r"\{k:'([a-z]+)',\s*t:", js)}
refs |= {i for i in html_ids if re.search(r"['\"]" + re.escape(i) + r"['\"]", js)}

all_ids = html_ids | dyn_ids
missing = sorted(r for r in refs if r not in all_ids)
unused = sorted(i for i in html_ids if i not in refs and not i.startswith('v-'))

print('HTML 静态 id :', len(html_ids))
print('JS 动态 id   :', len(dyn_ids))
print('JS 引用 id   :', len(refs))
print('缺失的 id    :', missing if missing else '无  ✓')
print('未被引用 id  :', unused if unused else '无')
if missing:
    sys.exit('缺失 DOM id，页面会白屏')

# 视图 id 与路由一致性
views = set(re.findall(r'class="view[^"]*" id="v-([a-z]+)"', html))
navs = set(re.findall(r"\{k:'([a-z]+)',t:", js))
print('视图容器     :', sorted(views))
navs = set(re.findall(r"\{k:'([a-z]+)',\s*t:", js)); print('导航目标     :', sorted(navs))
if 'client' not in views or views - navs - {'client'}:
    print('  [warn] 视图与导航项不完全对应（client 由路由动态进入，属正常）')

# CSS 变量在 SVG presentation attribute 里的误用（应放 style）
bad = re.findall(r'\b(?:stroke|fill)="var\(--', js)
print('SVG 属性误用 CSS 变量 :', len(bad), '处' if bad else '✓')
if bad:
    sys.exit('SVG 里的 stroke="var(...)" 不会被解析')

print('\n全部静态检查通过')
