# 底部面板排版改进实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 底部 ModeSelector 面板改为单行分组排版：补全 6 个控件标题、处理组/频谱组分隔、"1/6 oct" 完整显示、延迟文本胶囊 chip。

**Architecture:** 仅改 `Source/SubEQ_Editor/ModeSelector.h/.cpp`：新增 2 个标题 Label 与 2 个布局状态成员（`dividerX`、`latencyChipBounds`），重写 `resized()` 列布局，`paint()` 补绘竖分隔线与延迟胶囊。设计依据 `docs/superpowers/specs/2026-08-06-bottom-panel-layout-design.md`。

**Tech Stack:** C++ / JUCE 7.x / VS2022；MSBuild Release x64 验证；Standalone 截图人工核对。

## Global Constraints

- 窗口固定 900×620 不可缩放；底部面板高 60（`SubEQLookAndFeel::BottomPanelHeight`），不动该常量与 `PluginEditor`。
- 内容区 `getLocalBounds().reduced(10, 8)` = 880×44：标题行 14px + 控件行 30px。
- 列宽：Mode 156 / FIR 80 / 频谱 4×82 / 组内间距 8 / 组间距 24 / chip 232 右锚；弹性余量 ≈28px 落在频谱组与 chip 之间。
- 视觉 token 一律用设计系统：`DesignFonts::caption()`、`DesignColours::textSecondary()/surface()`；分隔线 alpha 0.25、chip alpha 0.7、chip 圆角 11（高 22 全圆）。
- 代码注释用简体中文；文件行尾保持 CRLF（ModeSelector.cpp/.h 现状，用 python 辅助脚本编辑时探测适配）。
- 版本约定：CHANGELOG 并入 `[0.3.0]` 条目；`.jucer` version 已为 0.3.0，不再变动。
- git 提交属变更操作，需用户明确指示后另行执行；本计划不含 commit 步骤。

---

### Task 1: ModeSelector 分组排版

**Files:**
- Modify: `Source/SubEQ_Editor/ModeSelector.h`
- Modify: `Source/SubEQ_Editor/ModeSelector.cpp`
- Modify: `CHANGELOG.md`（[0.3.0] 前端修复小节追加）
- Modify: `AGENTS.md`（SubEQ_Editor 行 ModeSelector 描述）
- Modify: `Interface.png`（重新截图）

**Interfaces:**
- Consumes: `DesignFonts::caption()`、`DesignColours::textSecondary()/surface()`（现有 API，签名不变）
- Produces: 无对外新接口（`ModeSelector` 公共接口不变；新增私有成员 `juce::Label modeLabel, firLabel`、`int dividerX = -1`、`juce::Rectangle<int> latencyChipBounds`）

- [ ] **Step 1: ModeSelector.h 新增成员**

在 `juce::Label fftSizeLabel;` 之前插入两个 Label 声明，在 `LatencyProvider latencyProvider;` 之前插入两个布局状态成员：

```cpp
    juce::Label modeLabel;
    juce::Label firLabel;
```

```cpp
    int dividerX = -1;                      // 处理组/频谱组竖分隔线 x（-1 = 尚未 layout）
    juce::Rectangle<int> latencyChipBounds; // 延迟文本胶囊背景
```

- [ ] **Step 2: 构造函数补两个标题 + 延迟文本改居中**

在 `latencyLabel.setText (...)` 一段之前插入：

```cpp
    // Captions for the processing group (spectrum captions are set in the loop below)
    for (auto* l : { &modeLabel, &firLabel })
    {
        l->setFont (DesignFonts::caption());
        l->setColour (juce::Label::textColourId, DesignColours::textSecondary());
        l->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (l);
    }
    modeLabel.setText ("Mode", juce::dontSendNotification);
    firLabel.setText ("FIR", juce::dontSendNotification);
```

并把 `latencyLabel.setJustificationType (juce::Justification::centredRight);` 改为 `juce::Justification::centred`。

- [ ] **Step 3: 重写 resized()**

整体替换现有 `resized()` 为：

```cpp
void ModeSelector::resized()
{
    auto bounds = getLocalBounds().reduced (10, 8);

    // Column widths (design doc 2026-08-06): processing group | divider |
    // spectrum group | elastic gap | right-anchored latency chip
    const int modeW = 156;
    const int firW = 80;
    const int spectrumW = 82;
    const int gap = 8;
    const int groupGap = 24;
    const int chipW = 232;

    // Latency chip: right edge, vertically centred on the combo row
    auto chipColumn = bounds.removeFromRight (chipW);
    chipColumn.removeFromTop (14); // skip the caption row
    latencyChipBounds = chipColumn.withSizeKeepingCentre (chipW, 22);
    latencyLabel.setBounds (latencyChipBounds);

    // Caption row mirrors the combo columns
    auto labelRow = bounds.removeFromTop (14);

    // Processing group: Mode + FIR
    modeBox.setBounds (bounds.removeFromLeft (modeW));
    bounds.removeFromLeft (gap);
    firLengthBox.setBounds (bounds.removeFromLeft (firW));

    modeLabel.setBounds (labelRow.removeFromLeft (modeW));
    labelRow.removeFromLeft (gap);
    firLabel.setBounds (labelRow.removeFromLeft (firW));

    // Group separator at the midpoint of the 24px gap
    bounds.removeFromLeft (groupGap);
    labelRow.removeFromLeft (groupGap);
    dividerX = firLengthBox.getRight() + groupGap / 2;

    // Spectrum group: 4 selectors with captions
    struct Col { juce::ComboBox* box; juce::Label* label; };
    const Col cols[] =
    {
        { &fftSizeBox, &fftSizeLabel },
        { &densityBox, &densityLabel },
        { &refreshBox, &refreshLabel },
        { &hopBox, &hopLabel }
    };

    for (const auto& c : cols)
    {
        c.box->setBounds (bounds.removeFromLeft (spectrumW));
        c.label->setBounds (labelRow.removeFromLeft (spectrumW));
        bounds.removeFromLeft (gap);
        labelRow.removeFromLeft (gap);
    }
}
```

注意：构造函数中的 `SpectrumControl controls[]` 数组与设置循环保持不变（它负责标题文本与附件绑定，与本布局无关）；原 `resized()` 里被删掉的 labelRow 单列排布由上面新逻辑覆盖。

- [ ] **Step 4: paint() 补绘分隔线与胶囊**

在 `paint()` 的 `drawMatteSurface` 调用之后追加：

```cpp
    // Group separator between processing controls and spectrum controls
    if (dividerX > 0)
    {
        g.setColour (DesignColours::textSecondary().withAlpha (0.25f));
        g.fillRect ((float) dividerX, (float) modeBox.getY(),
                    1.0f, (float) modeBox.getHeight());
    }

    // Latency capsule chip behind the label
    if (! latencyChipBounds.isEmpty())
    {
        g.setColour (DesignColours::surface().withAlpha (0.7f));
        g.fillRoundedRectangle (latencyChipBounds.toFloat(), 11.0f);
    }
```

- [ ] **Step 5: MSBuild 编译验证**

Run:
```bash
"D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Builds/VisualStudio2022/Napkid Sub EQ.sln" //p:Configuration=Release //p:Platform=x64 //v:q //nologo
```
Expected: exit code 0；仅既有 C4244/C4819/C4100 警告，无 error。

- [ ] **Step 6: Standalone 截图人工核对**

启动 `Builds/VisualStudio2022/x64/Release/Standalone Plugin/Napkid Sub EQ.exe`（后台），等待 3 秒后用 PowerShell `PrintWindow` 截取主窗口，保存覆盖 `Interface.png`；用 ReadMediaFile 读回核对：

- 6 个标题齐全（Mode / FIR / FFT Size / Density / Refresh / Hop），Mode、FIR 与频谱 4 标题同风格
- 处理组与频谱组之间有竖分隔线，组间距明显大于组内间距
- Density 完整显示 "1/6 oct"（不再截断为 "1/6 o"）
- 右端延迟文本位于浅色胶囊 chip 内，水平居中
- 切到 Minimum/Linear Phase 后 FIR 下拉可用、延迟文本刷新；切回 Zero Latency FIR 置灰

截图后关闭 Standalone 进程。

- [ ] **Step 7: 文档同步**

- `CHANGELOG.md` [0.3.0] → 前端 → 修复 小节追加一条：
  `- **底部面板分组排版**（2026-08-06）：Mode/FIR 补齐标题标签（原无标题）、处理组与频谱组加分组间距与竖分隔线、频谱下拉 76→82px（"1/6 oct" 不再截断）、延迟文本改为右端浅色胶囊 chip；面板高度与窗口布局不变。`
- `AGENTS.md` SubEQ_Editor 行中 `ModeSelector`（底部模式/FIR 长度/延迟面板）改为 `ModeSelector`（底部面板：处理组模式/FIR + 频谱组 + 延迟 chip 分组排版）。

## Self-Review 记录

- Spec 覆盖：补标题 ✓（Step 1/2）、分组+分隔线 ✓（Step 3/4）、82px 加宽 ✓（Step 3）、延迟 chip ✓（Step 3/4）、验证 ✓（Step 5/6）、文档 ✓（Step 7）。
- 无占位符；`dividerX`/`latencyChipBounds` 命名在 .h 与 .cpp 各 Step 间一致。
- 宽度核算：156+8+80+24 + 82×4+8×3 + 232 = 852 ≤ 880（弹性 28px）。
- 项目无 GUI 测试框架（AGENTS.md 明确），验证 = 编译 + 截图人工核对，不虚构自动化 UI 测试。
