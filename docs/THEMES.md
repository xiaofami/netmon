# 界面皮肤

顶栏右侧下拉可切换 6 套皮肤，选择存 `localStorage`，刷新后保持。

| 值 | 名称 | 特点 |
|---|---|---|
| `dark` | 深色（默认） | 深蓝灰底，蓝色强调 |
| `light` | 浅色 | 白底，适合白天 / 亮屏 |
| `midnight` | 午夜蓝 | 更深的蓝黑，高饱和强调色 |
| `nord` | 北欧灰蓝 | 低饱和灰蓝，长时间看更柔和 |
| `solar` | 暖阳 | 米黄暖色浅色主题 |
| `contrast` | 高对比 | 纯黑底 + 加粗边框，弱视 / 强光环境友好 |

## 实现方式

全部通过 CSS 变量覆盖，**不复制任何布局规则**：

```css
:root[data-theme="nord"]{ --bg:#242933; --surface:#2b313c; --accent:#88c0d0; ... }
```

`applyTheme()` 只做 `document.documentElement.setAttribute('data-theme', S.theme)`。
颜色变量被 KPI、图表、徽章、表格、SVG 折线共用，切主题时整页一致变化。

## 新增一套皮肤

1. 在 `www/index.html` 的 `:root{ --mono: ... }` 之前加一组
   `:root[data-theme="名字"]{...}`，至少覆盖：
   `--bg --side --surface --surface2 --surface3 --line --line2 --fg --fg2 --fg3
    --accent --accent2 --teal --ok --warn --bad --down --up --shadow --glow`；
2. 在顶栏 `#theme-box` 的 `<option>` 里加一项；
3. 用真实浏览器在 1600px 与 390px 各看一眼，检查文字对比度。

> 中国区惯例：流量涨用红、跌用绿只适用于行情类；本面板是网络监控，
> 沿用「下行绿 / 上行蓝」的通用色，不套用股票配色。
