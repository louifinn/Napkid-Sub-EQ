# 底部面板排版改进设计（ModeSelector）

日期：2026-08-06 ｜ 状态：已确认 ｜ 目标版本：并入 0.3.0

## 背景与问题

底部面板（`Source/SubEQ_Editor/ModeSelector.h/.cpp`，900×60，窗口 900×620 固定不可缩放）当前存在四处排版缺陷：

1. **标题不一致**：Mode 与 FIR 长度下拉框无标题标签，而 4 个频谱控件（FFT Size / Density / Refresh / Hop）均有标题——初见无法理解 "Zero Latency"、"4096" 的语义。
2. **无分组**：EQ 处理控制（模式、FIR 长度）与频谱显示控制（4 个）在视觉上是连续一排 6 个相同盒子，功能语义被抹平。
3. **文本截断**：频谱盒子宽 76px，Density 项 "1/6 oct" 被截断为 "1/6 o"。
4. **延迟显示无状态感**：右侧延迟文本为裸右对齐文字，与整体液态玻璃设计语言脱节。

## 方案选择

| 方案 | 说明 | 取舍 |
|------|------|------|
| **A（采用）** | 单行分组排版：补全标题、分组间距 + 竖分隔线、盒子加宽、延迟胶囊 chip | 改动集中在 ModeSelector 一个组件；主绘图区不受影响 |
| B | 面板加高至 84px、两行布局 | 语义最清晰，但挤压频响区 24px，牵涉 `SubEQLookAndFeel` 布局常量与文档截图，代价大 |
| C | 标题内嵌进 ComboBox（"Mode: Zero Latency"） | 垂直最省，但需重写 `drawComboBox` 自绘，与现有 caption 行风格断裂 |

## 布局设计（方案 A）

内容区：`getLocalBounds().reduced(10, 8)` = 880×44。标题行 14px + 控件行 30px（现状不变）。

```
[Mode ▾]  [FIR ▾]     │     [FFT Size][Density][Refresh][Hop]   …弹性…   ┌ Latency: 0.0 ms (0 samples) ┐
  156      80       24px+竖线      82      82       82     82      ≥14px           232px 胶囊 chip
```

- **标题行**：6 个控件全部有居中标题（新增 `modeLabel`="Mode"、`firLabel`="FIR"），复用现有 `DesignFonts::caption()` + `DesignColours::textSecondary()`，与频谱 4 标题完全一致。
- **分组**：处理组（Mode 156px + 8px + FIR 80px）与频谱组（4×82px、组内间距 8px）之间留 24px，并在其中点绘制 1px 竖分隔线（`textSecondary` × 0.25 alpha，纵跨控件行高度）。分隔线 x 坐标由 `resized()` 存入成员 `dividerX`，`paint()` 中绘制。
- **频谱盒子**：宽度 76 → 82px，保证 "1/6 oct" 完整显示。
- **延迟 chip**：右端固定 232×22 圆角胶囊（`DesignColours::surface()` × 0.7 alpha，圆角 11px 全圆），文本居中（`Justification::centred`）；`paint()` 依据 `latencyChipBounds` 绘制胶囊背景，`resized()` 中把 `latencyLabel` 置于胶囊内。胶囊垂直居中于控件行。
- 频谱组与 chip 之间为弹性间距（≥14px，吸收约 28px 余量），chip 右端贴内容区右边距。
- FIR 长度选择器在 Zero Latency 模式下的置灰行为（已有实现）保持不变。

## 组件改动（仅 ModeSelector）

- `ModeSelector.h`：新增 `juce::Label modeLabel; juce::Label firLabel;` 与 `int dividerX = -1;`、`juce::Rectangle<int> latencyChipBounds;` 成员。
- `ModeSelector.cpp`：
  - 构造函数：为两个新 Label 设置文本/字体/颜色/居中并 `addAndMakeVisible`。
  - `resized()`：按上表重排；记录 `dividerX` 与 `latencyChipBounds`。
  - `paint()`：在 `drawMatteSurface` 之后绘制竖分隔线与延迟胶囊背景。
- 不动 `SubEQLookAndFeel` 布局常量、`PluginEditor`、窗口尺寸。

## 错误处理与边界

- 无新运行时风险：纯布局常量与绘制；`dividerX = -1` 时 `paint()` 跳过分隔线（尚未 layout）。
- 分隔线仅纵跨控件行（30px），不穿过标题行，避免视觉噪声。
- 延迟文本最长情形（"Latency: 754.6 ms (33278 samples)"，Linear 65536 @44.1 kHz，约 33 字符 ≈ 208px @12px + 内边距）在 232px 胶囊内可完整显示。

## 验证

1. MSBuild Release x64 编译通过。
2. Standalone 运行截图，人工核对：6 标题齐全、分组分隔线、"1/6 oct" 完整、chip 样式、切模式后 FIR 置灰与延迟文本刷新。
3. `CHANGELOG.md` [0.3.0] 前端条目追加本改进；README/AGENTS.md 涉及底部面板描述处同步（AGENTS.md 的 ModeSelector 描述）。
