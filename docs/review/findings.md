# Napkid Sub EQ 代码审查台账（findings）

> 审查轮次：第 1 轮全量审查 · 2026-08-14
> 审查范围：`Source/**`（EQ 引擎、FFTProcessor、PluginProcessor、GUI 组件、DesignSystem）、`Tests/subeq_fft_test.cpp`、`Napkid Sub EQ.jucer`
> 验证基线：
> - Release x64 构建通过（MSBuild 18.8，仅 DesignLookAndFeel.cpp 的 C4244/C4100 警告，见 F-021）
> - `Tests/subeq_fft_test.exe` 96 项全部通过（按 AGENTS.md 命令重编译后运行）
> - 对照本机 JUCE 源码（`D:/Program Files/JUCE/modules`）核实：`APVTS::getRawParameterValue` 返回反归一化值（与项目注释一致，非缺陷）、Windows 滚轮 `deltaY = amount/256`（±0.234/格）
> 结论摘要：**P0 × 0，P1 × 1，P2 × 23**，另附 5 条 Informational 备注（不计入可行动项）。
>
> **处理记录：2026-08-14 · v0.3.4 —— 全部可行动项已修复**（F-001~F-024，含 6 项原"待确认"），
> 新增 6 项回归断言（96 → 102 项全绿），Release x64 构建零警告；Informational 备注第 4 条（ARA ID）
> 随版本号同步一并更新。各条目的修复方式与验证见"处理记录"（文末）。

---

## P1（应修复）

### F-001 [Source/SubEQ_FFTProcessor.cpp:256-267] epoch 校验与发布之间存在 TOCTOU 竞态：采样率切换时可发布过期系数

- **严重度**：P1
- **类别**：数据竞争 / 发布-订阅协议
- **状态**：已修复（v0.3.4）——epoch 复检移入发布临界区，与 prepare() 的"先 bump epoch、同锁清空 currentState"构成同锁序列化
- **证据**（原文）：
  - `:223` `const int epoch = designEpoch.load(std::memory_order_acquire);`
  - `:256-257` `if (epoch != designEpoch.load(std::memory_order_acquire)) continue;`
  - `:263-267`
    ```cpp
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        currentState = newState;
    }
    stateVersion.fetch_add(1, std::memory_order_release);
    ```
  - `prepare()` 中 `:54` `designEpoch.fetch_add(1, std::memory_order_release);` 与 `:102-110`（持 `stateMutex`）`currentState.reset();`
- **说明**：后台线程在 `:256` 通过 epoch 校验后、`:264` 发布前没有任何复检，二者也不与 `prepare()` 的 epoch 递增/`currentState.reset()` 同锁。存在如下交错（推断，基于上述原文）：后台线程 `:256` 校验通过 → 音频线程执行 `prepare()`（采样率切换：`:54` bump epoch、`:103-104` 锁内清空 `currentState`、末尾 `requestRedesign()`）→ 后台线程 `:264-265` 把按**旧采样率**设计的 `newState` 写回 `currentState`。结果：音频线程采纳过期的错误采样率系数并恢复输出，直至 `prepare` 请求的重设计完成发布（约一个设计周期，可达数百毫秒）。属自愈的瞬时错误滤波（故 P1 而非 P0），但恰好落在 v0.3.3 声称修复的"采样率切换系数错位"路径上。窗口为 `:256`→`:264` 数条指令，概率低但非零（宿主切换采样率/会话时可复现）。
- **建议**（仅参考）：把 epoch 复检移入发布临界区内——`{ lock; if (epoch != designEpoch.load(acquire)) { /* 丢弃 */ } else { currentState = newState; } }`。由于 `prepare()` 的 epoch 递增先于其锁内 `currentState.reset()`，同锁复检可覆盖上述交错。

---

## P2（考虑修复）

### F-002 [Source/SubEQ_FFTProcessor.cpp:191-195, 263-266, 275-277, 376, 390] 音频线程事件路径加锁，且后台线程持锁释放约 2.3 MB 的 FIRState 放大等待

- **严重度**：P2
- **类别**：线程安全 / 实时性
- **状态**：已修复（v0.3.4）——退役环排空改为"锁内移出、锁外析构"；发布处旧状态同样移出临界区析构；prepare/clearPublishedState 仅做指针搬移
- **证据**（原文）：音频线程锁点 `:275-277`（采纳新版本）、`:376`（淡化结束移交）、`:390`（淡化被跳过分支）；后台线程 `:191-195`（持锁 `slot.reset()` 排空退役环）、`:263-266`（持锁发布，旧 `currentState` 若为最后引用则在锁内析构）。
- **说明**：AGENTS.md 规定"音频线程不得加锁/阻塞"；本实现仅在版本切换/淡化结束的过渡期加锁（稳态 `:275` 快路径零锁），头文件 `:104` 已注明"rarely contended"，属**有意的过渡期妥协**。放大因素：后台线程排空退役环时持锁析构含 `coeffs`（最多 65536 double）+ `freqCoeffs`（最多 131072 complex\<double\>，约 2.1 MB）的 FIRState，若音频线程此刻恰好因版本变化等锁，会阻塞到释放完成（典型数十微秒级，堆碎片化下更长）。事件频率低（每次设计发布一次），未实测到可闻影响，故定 P2 而非 P1；但若坚持"音频线程绝对不加锁"的硬约束，需要改造。
- **建议**（仅参考）：把大块释放移出临界区——后台线程先在锁内把 `pendingDestroy` 槽位 `std::move` 到局部集合，解锁后再析构；发布处同理先取出旧 `currentState` 到局部变量、锁内只做指针交换。也可进一步把退役环改为音频线程单写者/后台线程单读者的无锁环形队列。

### F-003 [Source/SubEQ_FFTProcessor.cpp:49, 86-87, 336] `prepare()` 早退条件不含 `maxBlockSize`：块尺寸增大时交叉淡化被静默关闭

- **严重度**：P2
- **类别**：功能缺陷 / 交叉淡化
- **状态**：已修复（v0.3.4）——prepare() 对增大的块尺寸提前重分配 xfade 缓冲（不动设计/epoch），process() 淡化分支不再被静默跳过
- **证据**（原文）：`:49` `if (sr == preparedSampleRate && channels == numChannels && !fftWork.empty()) return;`；`:86-87` `xfadeOld/xfadeNew.setSize(numChannels, std::max(1, maxBlockSize))`；`:336` `if (crossfadeRemaining > 0 && numSamples <= xfadeNew.getNumSamples())`。
- **说明**：若宿主在采样率/声道数不变的情况下以更大的块尺寸重入 `prepareToPlay`，早退会跳过 `xfadeOld/xfadeNew` 重分配；`:336` 条件不满足时走 `:381-394` 的 else 分支，直接把 `crossfadeRemaining` 清零——参数编辑的 1024 样本交叉淡化被静默跳过，重新出现输出阶跃。不会崩溃（有 `:336` 防护），但违背"参数编辑无输出阶跃"约定。
- **建议**（仅参考）：早退比较纳入块尺寸，如 `if (sr == preparedSampleRate && channels == numChannels && maxBlockSize <= xfadeNew.getNumSamples() && !fftWork.empty()) return;`，或块尺寸变化时仅重分配淡化缓冲而不动设计。

### F-004 [Source/SubEQ_FFTProcessor.cpp:307-329] 交叉淡化进行中再次发布新设计时，新侧滤波器中途切换、不开启新淡化

- **严重度**：P2（待确认听感）
- **类别**：交叉淡化正确性
- **状态**：已修复（v0.3.4）——淡化期间发布的新设计挂起（pendingState），淡化收尾后再切换；延迟不同的设计立即收尾淡化并冲刷切换（与 F-006 联动）
- **证据**（原文）：`:307` `if (version != processedVersion || L != activeConvFFTSize)`；`:309` `if (processedVersion != 0 && crossfadeRemaining <= 0)`（仅此时开启新淡化）；`:328-329` 淡化中命中新版本只更新 `activeConvFFTSize/processedVersion`；`:352-355` 淡化期间新侧始终用最新 `state` 推进。
- **说明**：1024 样本淡化进行到一半（混合权重 w 已到 0.5~1.0）时若又有新设计发布，本块新侧输出从上一设计直接跳到最新设计，被以 w≈0.5+ 的权重混入——两次设计差异大（FIR 长度/大增益变化）时可能产生可闻阶跃。需"21 ms 内连续两次编辑"才触发；典型小幅编辑下差异可忽略，故标待确认。
- **建议**（仅参考）：淡化期间命中新版本时，将旧淡化立即收尾（或以当前混合输出为基准重启淡化）；至少补充注释说明该边界取舍。

### F-005 [Source/SubEQ_FFTProcessor.cpp:33-34] `stopThread` 失败在 Release 下无兜底，存在潜在 use-after-free

- **严重度**：P2（低概率）
- **类别**：生命周期 / 内存安全
- **状态**：已修复（v0.3.4）——Release 下 stopThread 失败后继续阻塞等待线程退出（while (isThreadRunning()) wait(50)），消除 UAF 窗口
- **证据**（原文）：
  ```cpp
  if (!stopThread (5000))
      jassertfalse;   // worker did not exit in time — would be a lifetime bug
  ```
- **说明**：`jassertfalse` 在 Release 构建为空操作。若工作线程 5 s 内未退出（当前负载：最长 65536 抽头设计 + 131072 点 FFT 实测远小于 1 s，故概率极低），析构仍会返回，仍在运行的 `run()` 继续访问 `this`（`designEngine`、`sampleRate`）及经 `parameterSource` 指向的 `apvts`，构成 UAF。代码注释已自认此为 lifetime bug，但 Release 无硬保护。
- **建议**（仅参考）：Release 下也做硬兜底（例如失败时 `DBG` 记录并 `std::terminate` 或让 `run()` 的退出检查更短周期），或补注释量化"设计耗时上限远小于 5 s"的依据。

### F-006 [Source/PluginProcessor.cpp:347-355 + SubEQ_FFTProcessor.cpp:306-330] PDC 在发布当块立即切换，交叉淡化期间输出仍混新旧延迟

- **严重度**：P2（待确认）
- **类别**：延迟补偿一致性
- **状态**：已修复（v0.3.4）——仅 groupDelay 相同（两侧管线延迟一致）时交叉淡化；延迟不同的设计（FIR 长度/模式变化）冲刷累加器立即切换，PDC 翻转当块起即与输出一致（权衡语义写入代码注释与 CHANGELOG）
- **证据**（原文）：`PluginProcessor.cpp:349-354` 每块 `newLatency = fftProcessor.getLatencySamples()` 且不等即切换 `reportedLatency` 并置脏上报；`FFTProcessor::getLatencySamples()`（`:407-415`）返回**新发布状态**的 `groupDelay + 511`；而 `process()`（`:306-330`）同时仍在以 1024 样本淡化混合旧设计（旧 `groupDelay`）。
- **说明**：FIR 长度变化（如 4096→16384，线性相位 groupDelay 由 2047 跳至 8191）时，PDC 在发布当块跳变，而淡化窗口内旧设计尾音仍按旧延迟混入，出现最长 21 ms 的补偿错位。这是"不同延迟设计间交叉淡化"的固有矛盾（两侧不可能同时对齐），此前行为（冲刷）有输出阶跃、本行为有短暂错位，属权衡；听感待确认。
- **建议**（仅参考）：可在注释/文档中明确该语义；若需更严格对齐，可考虑仅在 `groupDelay` 相同（仅系数变化）时淡化、延迟变化时退回冲刷。

### F-007 [Source/SubEQ_Core.cpp:82, 91-98] Tilt→单 biquad 淡出窗口内再次 `update()` 会瞬间丢弃半淡出的第二 biquad

- **严重度**：P2
- **类别**：系数平滑状态机
- **状态**：已修复（v0.3.4）——淡化计划抽为纯函数 computeBiquadFadePlan（SubEQ_BiquadDesign.h），淡出窗口内再次 update() 从当前半淡出系数继续淡出；新增 5 项回归断言
- **证据**（原文）：`:82` `extraBiquadFadeOut = false;`（无条件复位）；`:84-98` 仅当 `numBiquads > 1` 或 `prevNumBiquads > 1` 时重武装淡出/淡入；当第二次 `update()` 时 `prevNumBiquads` 已被第一次更新改为 1（`numBiquads == 1`），两个分支都不命中。
- **说明**：Tilt→Bell 切换后约 15 ms 淡出窗口内，若同一节点再次收到参数变化（如 GUI 连续写参、宿主自动化），`activeBiquads()` 由 2 直接变为 1，半淡出的第二 biquad 的滤波贡献瞬间消失——与相邻样本存在阶跃（推断，可闻性取决于当时第二 biquad 的输出幅度）。`smoothingEnabled=false` 的路径（设计引擎）不受影响。
- **建议**（仅参考）：在 `numBiquads == 1 && prevNumBiquads == 1` 且 `coeffs[1]` 尚非单位（或记录"上一状态曾有第二 biquad"标志）时，重武装 `extraBiquadFadeOut = true` 并让 `smoothStart[1]` 取当前（半淡出）系数继续淡出。

### F-008 [Source/SubEQ_Core.cpp:162-166] 全局 bypass 以 `memcpy` 直通，无交叉淡化且不清节点状态

- **严重度**：P2（设计取舍）
- **类别**：旁路瞬态
- **状态**：已修复（v0.3.4）——旁路改为 ~15 ms 湿/干交叉淡化；旁路期间节点链保持处理（热旁路），解除旁路时无陈旧状态瞬态
- **证据**（原文）：
  ```cpp
  if (bypass || tempBufferSize <= 0)
  {
      std::memcpy(output, input, static_cast<size_t>(numSamples) * sizeof(float));
      return;
  }
  ```
- **说明**：旁路切换是瞬时直通，被旁路期间节点状态冻结在旁路时刻的历史值；解除旁路时滤波器以约 2 个样本的陈旧状态恢复，且旁路/解除本身无淡化，滤波输出（可能含增益）与直通之间的跳变未被平滑——存在瞬态/咔哒风险。节点启停有 ~15 ms 淡化，但主 bypass 没有；属设计取舍，待确认是否为有意简化。
- **建议**（仅参考）：若需点击无感旁路，可在切换时对 dry/wet 做短淡化（复用 fadeGain 机制），或在解除旁路时 `reset()` 节点状态。

### F-009 [Source/PluginProcessor.cpp:169 + PluginProcessor.h:62-67] `prepareToPlay` 调用声明为 "audio thread only" 的 `updateEQParameters()`，且公开 `getEQEngine()` 存在误用隐患

- **严重度**：P2
- **类别**：线程契约 / 文档
- **状态**：已修复（v0.3.4）——注释改为"audio thread 或宿主已停止处理的 prepare 上下文"；getEQEngine() 注明非线程安全、全仓无调用方
- **证据**（原文）：`PluginProcessor.cpp:169`（`prepareToPlay` 内）`updateEQParameters();`；`PluginProcessor.h:65-67` 注释 "audio thread only — writes eqEngine and the non-atomic caches; never call from the GUI thread"；`PluginProcessor.h:62-63` 公开返回音频线程引擎的非 const 引用。
- **说明**：JUCE 不保证 `prepareToPlay` 一定在音频线程执行（部分宿主在消息线程调用）。当前无实际竞争：`eqEngine` 未被任何非音频线程读取（grep 核实 `getEQEngine()` 全仓无调用方），且宿主在 prepare 期间通常已停止处理。属契约违背 + 公开接口埋雷，而非现网缺陷。
- **建议**（仅参考）：将注释改为"audio thread 或宿主已停止处理的 prepare 上下文"；或将 `updateEQParameters()` 拆为纯参数缓存刷新（prepare 内只做无害部分）；长期可移除/收紧公开 `getEQEngine()` 访问器。

### F-010 [Source/SubEQ_DSPMath.h:53, 76-80 + SubEQ_FFTProcessor.cpp:94-98 + SubEQ_Spectrum.cpp:59-63] twiddle 预热依赖 prepare 与 processBlock 同线程，否则音频线程首次 FFT 会堆分配

- **严重度**：P2
- **类别**：实时性 / 内存分配
- **状态**：已修复（v0.3.4）——FFTProcessor::process 与 SpectrumAnalyzer::process 对实际尺寸做惰性预热（幂等、命中即 O(log n) 查找），不再依赖 prepareToPlay 线程亲和
- **证据**（原文）：`SubEQ_DSPMath.h:53` `static thread_local TwiddleCache<T> cache;`，`:60-62` 尺寸不匹配时 `e.w.resize(n/2)` + O(N) `cos/sin`；`SubEQ_FFTProcessor.cpp:97-98` 与 `SubEQ_Spectrum.cpp:62-63` 在 prepare 路径调用 `prewarmTwiddleTable<double>(...)`；`SubEQ_DSPMath.h:76-80` 注释已自认 "effective only if called on the same thread that later runs the FFT"。
- **说明**：若宿主在非音频线程调用 `prepareToPlay`（JUCE 未保证线程），预热无效，音频线程首次 FFT 会执行一次性的 ~0.5–2 MB 分配 + O(N) 三角函数，违反"音频线程不得分配"。一次性、稳态无复发，代码注释已披露该限制；是否触发取决于宿主线程模型。
- **建议**（仅参考）：在 `processBlock` 首块（音频线程内）对实际用到的尺寸惰性调用 `prewarmTwiddleTable`（幂等且命中即廉价查找）。

### F-011 [Source/SubEQ_Spectrum.cpp:44-52, 80] `configure()` 在音频线程重生成 Hann 窗（O(N) 次 `std::cos` 尖峰）

- **严重度**：P2
- **类别**：CPU 使用 / 实时性
- **状态**：已修复（v0.3.4）——三档 Hann 窗在构造函数（消息线程）一次性生成，configure() 仅切换窗表指针
- **证据**（原文）：`SubEQ_Spectrum.cpp:44-52` `regenerateWindow()` 对 `fftSize`（最大 16384）逐点 `hannWindowValue`（内含 `std::cos`）；`:80` 由 `configure()` 调用；调用链 `PluginProcessor.cpp:251`（`updateEQParameters`，音频线程）→ `configure()`。
- **说明**：频谱参数变更时在音频线程执行最多 16384 次 `cos`（另 `prepare()` 还有三档 twiddle 预热 ≈ 28672 次 `cos/sin`）。无分配、无锁、无阻塞（不违反硬约束的字面），但属一次性有界 CPU 尖峰（亚毫秒级），仅配置变更时发生。
- **建议**（仅参考）：窗表按 12..14 三档预计算缓存，`configure()` 仅切换指针/尺寸。

### F-012 [Source/SubEQ_Editor/MasterGainSlider.cpp:356-364 vs 215-226] 0dB "跨越失速" detent 的第二分支为死代码

- **严重度**：P2
- **类别**：交互逻辑
- **状态**：已修复（v0.3.4）——mouseDrag 先用上一事件 RAW 值做跨零比较、再记录本事件 RAW 值，0dB 失速吸附第二分支恢复生效
- **证据**（原文）：`:362` `dragLastValue = newValue;` 先于 `:363` `snapToZeroDetent(newValue);`；`snapToZeroDetent` 内 `:222` `(dragLastValue * value < 0.0f && std::abs(value) < zeroDetent * 2.0f)`。
- **说明**：`dragLastValue` 在 snap 前已被覆盖为 `newValue`，故 snap 内 `dragLastValue * value == value² ≥ 0` 恒成立，跨零分支永假——只剩 `|value| < zeroDetent` 的绝对吸附，头注释（`.h:17` "0dB stall detent"）描述的跨零失速吸附不生效。注释（`:360-361`）声称"先记录 RAW 值"与代码实际顺序矛盾。
- **建议**（仅参考）：`const float raw = newValue; snapToZeroDetent(newValue); dragLastValue = raw; setValue(newValue);`（用上一事件原始值做符号比较）。

### F-013 [Source/SubEQ_Editor/MasterGainSlider.cpp:429-430 + FrequencyResponse.cpp:1185-1186] 滚轮步进注释与实际 deltaY 不符

- **严重度**：P2
- **类别**：注释失实 / 交互调参
- **状态**：已修复（v0.3.4）——两处注释按实测 deltaY 更正（主增益 ≈±2.3 dB/格、Q ≈+5.5%/格），乘数保持不变
- **证据**（原文）：`MasterGainSlider.cpp:429-430` 注释 "Wheel deltaY is typically ±0.05..0.15 per notch → ~±1 dB per notch"，代码 `value += wheel.deltaY * 10.0f;`；`FrequencyResponse.cpp:1185-1186` 注释 "each wheel step = fixed ratio change (~25%)"，代码 `stepLogQ(qVal, wheel.deltaY * 0.1f)`。本机 JUCE 源码 `juce_Windowing_windows.cpp:3217`：`wheel.deltaY = isVertical ? amount / 256.0f : 0.0f;`，其中 `amount = 0.5 * (short)HIWORD(wParam) = ±60`/格 → **deltaY ≈ ±0.234/格**。
- **说明**：按实际 deltaY，主增益实际约 ±2.34 dB/格（注释称 ±1 dB），Q 实际约 +5.5%/格（注释称 ~25%）——注释幅度失实 2~4 倍，误导后续调参；步进数值本身尚属合理，属注释/行为说明失准。
- **建议**（仅参考）：修正两处注释（主增益 ~2.3 dB/格、Q ~5.5%/格），或若希望严格 1 dB / 25% 则调整乘数（主增益约 ×4.3、Q 约 ×0.43）。

### F-014 [Source/SubEQ_Editor/FrequencyResponse.cpp:355-357] 频谱 dB 映射（-60..0）与 EQ 网格 ±24 dB 同轴叠加，纵轴不对齐

- **严重度**：P2（待确认是否有意）
- **类别**：视觉 / 交互
- **状态**：已修复（v0.3.4）——频谱改用与网格一致的 gainToY（±24 dB）同轴映射，低于 -24 dB 钉在底边；纵轴与 EQ 曲线/刻度对齐
- **证据**（原文）：`FrequencyResponse.cpp:355-357` `float norm = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);`（-60 dB 在底、0 dB 在顶）；而网格经 `gainToY`（`SubEQ_CoordinateMapper.h:57-61`，bottom=-24、top=+24）绘制。
- **说明**：频谱把 -60..0 dB 压进整个绘图区高度：0 dB 谱峰显示在网格 +24 dB 线上，-30 dB 落在 0 dB 网格线——频谱峰值与 EQ 曲线/网格刻度在纵轴错位（推断）。若"60 dB 动态范围的独立频谱"是有意设计则此项可关闭。
- **建议**（仅参考）：若希望同轴对齐，频谱映射改用与 `gainToY` 一致的比例（或提供独立右轴并标注刻度）。

### F-015 [Source/SubEQ_Editor/FrequencyResponse.cpp:1282-1286, 1482-1487] 部分参数写入未包裹 begin/endChangeGesture

- **严重度**：P2
- **类别**：交互 / 自动化
- **状态**：已修复（v0.3.4）——createNodeAt 的 Q/Type 默认值写入与类型菜单切换（含增益重置）补 begin/endChangeGesture
- **证据**（原文）：`createNodeAt` 内 Q/Type 默认值写入（`:1282-1286`）与类型菜单切换及增益重置（`:1482-1487`）直接 `setValueNotifyingHost`；同文件拖拽（`beginNodeGesture/endNodeGesture`）、滚轮（`:1187-1189`）、双击重置（`:1156-1168`）均包裹手势。
- **说明**：单点写入在宿主自动化记录中不被归组（undo/automation 分组不完整），非崩溃、非音频问题。
- **建议**（仅参考）：为这些写入补 `beginChangeGesture/endChangeGesture`（或抽 `setNodeParamWithGesture` 助手）。

### F-016 [Source/SubEQ_Editor/FrequencyResponse.h:80, 88, 155-156, 162] 未使用成员与方法（死状态/死代码）

- **严重度**：P2
- **类别**：规范 / 未使用代码
- **状态**：已修复（v0.3.4）——删除 dragStartNodeScreen、dragStartedOnNode 成员及 getGainArea/getPhaseArea/yToPhase（含全部赋值点）
- **证据**（grep 全库）：`dragStartNodeScreen` 仅写入（`.cpp:966, 1005`）无读取；`dragStartedOnNode` 仅写入（`.cpp:895, 944, 989, 1118`）无读取；`getGainArea()`（`.cpp:212-215`）、`getPhaseArea()`（`.cpp:217-222`）、`yToPhase()`（`.cpp:254-258`）仅有声明与定义，无调用点（`phaseToY` 有使用）。
- **说明**：拖拽分支实际使用 `isDraggingNode/draggedNode/dragStartScale`，上述成员被赋值但从不参与判定，误导读者。
- **建议**（仅参考）：删除这些成员/方法及其赋值语句（或补充读取逻辑）。

### F-017 [Source/SubEQ_Editor/DesignSystem/DesignLookAndFeel.cpp:141-231, 43-46, 164-166] drawRotarySlider/drawLinearSlider 重载为死代码，且 knobRadius 无下限防护

- **严重度**：P2
- **类别**：死代码 / 边界防护
- **状态**：已修复（v0.3.4）——删除 drawRotarySlider/drawLinearSlider 死重载与 Slider 颜色设置（AGENTS.md 描述同步）；knobRadius 无下限隐患随代码一并消失
- **证据**（grep 全库）：无任何 `juce::Slider` 实例化——主增益为自定义 `juce::Component`（MasterGainSlider.h:29），EQ 节点由 FrequencyResponse 自绘；两个重载与 `:43-46` 的 `juce::Slider::*` 颜色设置永不执行。`:146` `float radius = juce::jmax(4.0f, ...)`，`:164` `float knobRadius = radius - 12.0f;` 无下限——尺寸 < 24px 时产生负尺寸几何（当前因无 Slider 不可达）。
- **说明**：与 AGENTS.md "LookAndFeel_V4 全套重载" 的描述存在偏差（重载存在但无对应控件）；一旦未来启用 Slider 即触发负尺寸隐患。
- **建议**（仅参考）：删除两个死重载与 Slider 颜色设置；若为将来预留，加注释并补 `knobRadius = juce::jmax(1.0f, radius - 12.0f)`。

### F-018 [Source/SubEQ_Editor/DesignSystem/DesignConstants.h:13-74 + DesignFonts.cpp:48-51 + ModeSelector.cpp:196-201 + DesignLookAndFeel.cpp:790-809] 设计 token 大量未引用，字号/布局/阴影存在双重来源

- **严重度**：P2
- **类别**：一致性 / 死代码
- **状态**：已修复（v0.3.4）——删除全部未引用 token 与未引用颜色；弹簧常量/字号/底部面板布局收敛到 DesignConstants 单一事实来源；drawDropShadow 注明 cornerRadius 被精灵方案忽略的原因
- **证据**（grep 全库仅命中定义行）：`DesignConstants.h` 的 `dragDamping/springStiffness/springDamping`（:44-46，实际弹簧常量硬编码于 `FrequencyResponse.cpp:687-688` 的 wn2=400/twoZetaWn=20 与 `MasterGainSlider.cpp:60-61` 的 986.96/31.416）、`shadowOffsetX/Y/Radius/Spread`（:28-31）、`fontTitle/Body/Label/Caption/Value`（:70-74，`DesignFonts.cpp:48-51` 实际 caption 用 11.0f 而 token 为 10.0f）、`knobSizeDefault/knobAngleStart/End`、`eqNodeRadiusDefault/Drag/eqMaxNodes/eqStretchFactor`、`toggleWidth/Height/ThumbSize`、`minScale/maxScale/toolbarHeight/infoBarHeight/paddingLarge/cornerRadiusXL` 均无引用点；`DesignColours.h` 的 `panelBackground`（:19）、`accentSoft`（:23）、`accentGlow`（:24，与 `glassGlow()`（:63，alpha 0.15）语义重复但 alpha 0.16/0.15 不一致）、`accentHover`（:25）、`textOnDark`（:35）同样全仓无引用。`ModeSelector.cpp:196-201` 布局列宽/间距/圆角/行高全部硬编码（156/80/82/8/24/232/14/22/11.0f）。`DesignLookAndFeel.cpp:798-809` `drawDropShadow` 显式 `(void)cornerRadius` 丢弃圆角参数、偏移硬编码 `translated(0.0f, 2.0f)`（与 shadow token 2/4 不一致），阴影圆角由 sprite 烘焙的 6px 决定。
- **说明**：AGENTS.md 声称 DesignConstants 提供"圆角/动画时长/物理参数 token"，实际大量 token 是误导性死代码——维护者改 token 不会生效；字号与布局存在双重事实来源。
- **建议**（仅参考）：删除未引用 token（或让 DesignFonts/组件直接引用），收敛弹簧常量与布局数值到 token；`drawDropShadow` 若保留 sprite 方案，注释说明 cornerRadius 被忽略的原因。

### F-019 [Source/SubEQ_Editor/DesignSystem/LiquidGlassEffect.cpp:16-46] 公共静态绘制助手对 0/负尺寸无防护

- **严重度**：P2
- **类别**：边界防护
- **状态**：已修复（v0.3.4）——drawRoundedRect 入口对空/零尺寸、非正圆角与零缩放直接返回
- **证据**（原文）：`:16-17` 以 `radius` 构造 bounds，`:41-46` `gradRadius = bounds.getWidth() * 0.7f * scale` 构造 radial `ColourGradient`，入口无 `radius <= 0 / bounds.isEmpty()` 检查。
- **说明**：`gradRadius == 0` 时渐变起止点重合，存在 JUCE 内部分母为零/NaN 退化风险。当前唯一调用点 `FrequencyResponse.cpp:654` 传 `r + 4.0f`（r ≥ 1）不会触发；作为公共静态助手缺少防御。
- **建议**（仅参考）：函数入口加 `if (radius <= 0.0f || bounds.isEmpty()) return;`。

### F-020 [Source/SubEQ_Editor/DesignSystem/DesignLookAndFeel.cpp:773-786 + DesignFonts.cpp:11-22] 静态可变缓存/初始化标志无同步

- **严重度**：P2
- **类别**：线程安全（潜在）
- **状态**：已修复（v0.3.4）——DesignFonts 改用函数局部 magic static（线程安全一次初始化）；tinted sprite 缓存注释明确"仅限 GUI 线程"
- **证据**（原文）：`DesignLookAndFeel.cpp:773` `static std::map<juce::uint32, juce::Image> cache;`、`:786` 无同步写入；`DesignFonts.cpp:11-12` 静态 `avenirTypeface/initialised`，`:14-22` 以普通 bool 判断后惰性初始化。
- **说明**：当前所有调用方均在 GUI 消息线程 paint 路径（无实际竞争）；但 `drawDropShadow` 是 public static、`getAvenirTypeface()` 惰性触发 `initialise()`，若未来被非 GUI 线程调用即构成 `std::map` 并发插入 UB / bool 竞态。缓存键为少量固定 ARGB 色，不会无限增长。
- **建议**（仅参考）：注释明确"仅限 GUI 线程调用"；或 `DesignFonts` 改用函数局部 magic static（C++11 线程安全）承载 Typeface；tinted sprite 缓存改用固定数组或 `juce::SpinLock`。

### F-021 [Source/SubEQ_Editor/DesignSystem/DesignLookAndFeel.cpp:398, 805-806] 构建窄化转换警告 C4244（int→float、float→int）

- **严重度**：P2
- **类别**：构建质量 / 类型安全
- **状态**：已修复（v0.3.4）——drawHorizontalLine 显式 static_cast<float>、drawImage 用 roundToInt、必需未用参数 juce::ignoreUnused；Release 构建零警告（C4244/C4100 全消）
- **证据**（MSBuild Release 输出，exit 0）：`DesignLookAndFeel.cpp(398,85)/(398,63): warning C4244: "参数": 从"ValueType"转换到"float"，可能丢失数据 [ValueType=int]`（`drawHorizontalLine(area.getCentreY(), sepBounds.getX(), sepBounds.getRight())`）；`(805,51)/(805,32)/(806,60)/(806,36): warning C4244: 从"ValueType"转换到"int" [ValueType=float]`（`g.drawImage(..., shadowArea.getX(), shadowArea.getY(), shadowArea.getWidth(), shadowArea.getHeight(), ...)`）。另有三处 C4100（未引用参数 `hasSubMenu`/`icon`，为 LAF 虚函数签名所必需，属 JUCE 模式，可不处理）。
- **说明**：编译通过但留下窄化转换噪音；阴影区域 float→int 截断属像素级取整，无功能影响，但显式转换可表明意图并保持构建零警告。
- **建议**（仅参考）：改用 `static_cast<float>(...)` 与 `juce::roundToInt(...)`；对必需未用参数加 `juce::ignoreUnused`。

### F-022 [Source/SubEQ_DSPMath.h:108 + Tests/subeq_fft_test.cpp:113 + Source/SubEQ_Editor/DesignSystem/AnimationUtils.h:22-23] 三处依赖传递包含（自包含性）

- **严重度**：P2
- **类别**：自包含性
- **状态**：已修复（v0.3.4）——SubEQ_DSPMath.h 补 <utility>、Tests 补 <string>、AnimationUtils.h 补 <functional>
- **证据**（原文）：`SubEQ_DSPMath.h` 使用 `std::swap`（:108）但只 include `<complex>/<cmath>/<vector>`（未含 `<utility>`）；`Tests/subeq_fft_test.cpp` 使用 `std::string/std::to_string`（:113, 158 等 13 处）但 include 列表（:29-36）无 `<string>`；`AnimationUtils.h` 使用 `std::function`（:22-23）但只 include `<JuceHeader.h>`。
- **说明**：当前 MSVC 经传递包含可编译，但标准未保证（SF.11）；GCC/Clang 下可能编译失败。共享纯计算头文件被 AGENTS.md 明确要求自包含，测试文件由独立 cl 命令编译。
- **建议**（仅参考）：分别补 `#include <utility>`、`#include <string>`、`#include <functional>`。

### F-023 [Source/SubEQ_BiquadDesign.h:176-177] `computeBiquadCoefficients` 的 switch 无 default，非法枚举值返回 1 且不写 out[0]

- **严重度**：P2
- **类别**：防御性缺口
- **状态**：已修复（v0.3.4）——switch 补 default 分支写单位系数并返回 1；新增 1 项回归断言
- **证据**（原文）：`:35` `switch (type)` 覆盖全部 8 个 `FilterType` 后 `:177` 直接 `return 1;`，无 default 分支。
- **说明**：生产路径经 `intToFilterType`（未知值回落 Bell）保证传入合法，当前不可达；但作为共享纯计算头文件，非法值将返回"1 个 biquad"却未写 `out[0]`，调用方会使用陈旧系数。属防御性缺口。
- **建议**（仅参考）：default 分支写单位系数（`BiquadCoefficients{}`）或 `jassertfalse` 语义的硬断言后返回 1。

### F-024 [Tests/subeq_fft_test.cpp（整体）] v0.3.3 三项头条修复与 FIR 交叉淡化/epoch 过期无回归测试；Test 5b 注释与实现不符

- **严重度**：P2
- **类别**：测试覆盖缺口 / 文档
- **状态**：已修复（v0.3.4）——F-007 平滑状态机抽为纯函数并补 5 项回归、F-023 补 1 项回归（96 → 102）；Test 5b 注释更正为 DC-only 滤波器；CHANGELOG 明确新增项与头条修复的测试边界
- **证据**：96 项断言与 AGENTS.md"96 项"一致（无硬编码总数断言），但全部经由共享纯头文件覆盖；v0.3.3 头条修复——`EQNode::prepare` 采样率重算（SubEQ_Core.cpp:35-36，依赖 JUCE 不可测）、PDC 延迟上报（PluginProcessor.cpp:62-69）、同会话重复 prepare 不重设计（PluginProcessor.cpp:167-168）——均位于 JUCE 依赖文件中，独立测试无法覆盖，且 CHANGELOG"88→96"实际新增的是 BandPass 忽略增益/Nyquist 强调/NaN 毒化/NaN 回退/forceStable 分支（与本轮 F-001/F-003/F-006 相关路径同样无测试）。另 Test 5b 注释 `// identity filter` 实际是未做 FFT 的 DC-only 滤波器（断言仍正确，恒等路径由 Test 7 覆盖）。
- **说明**：AGENTS.md 要求"变更验证 = 编译 + 测试 + 人工"，本项指出自动化覆盖对近期修复路径的空白（非现网缺陷）。
- **建议**（仅参考）：为可无 JUCE 复现的部分（如 F-007 的平滑状态机逻辑，可通过抽取纯状态机到共享头文件实现）补回归；修正 Test 5b 注释；CHANGELOG 中明确"88→96 新增项"与头条修复无测试的边界。

---

## Informational 备注（不计入可行动项）

1. **频谱分析率与 GUI 刷新率解耦**（SubEQ_Spectrum.cpp:109-113 + PluginProcessor.cpp:360-361, 386-387）：hop=512 @48kHz 时两个分析器合计约 187 次 8192 点 FFT/秒，而 GUI 默认 30 Hz 消费——约 3× 未消费分析。hop（时间分辨率）与 refresh（显示刷新）正交属正常设计，CPU 量级不大，用户可调大 hop；如需优化建议改为"被消费后按 hop 推进"的拉模式。
2. **GUI paint 路径每帧新建 Path/ColourGradient/Font**（DesignLookAndFeel.cpp:562-705、LiquidGlassEffect.cpp:30-82、DesignFonts.cpp:48-51）：GUI 线程可接受，且阴影 sprite 已做静态/按色缓存（正确优化）；如追求极致可缓存恒定几何与 Font 实例。
3. **退役环 64 槽无溢出检查**（SubEQ_FFTProcessor.h:116-118）：音频线程每次排空窗口（≤50ms）最多追加 2~3 槽，现实不可达；头注释已说明该假设。
4. **`JucePlugin_ARADocumentArchiveID` 为 "0.3.1"**（JuceLibraryCode/JucePluginDefines.h:158）：自动生成字段未随版本更新（ARA 未启用，无功能影响）。→ v0.3.4 已随版本号同步更新。
5. **`EQNode::getResponse()` 在第二 biquad 淡出窗口内不包含淡出中的 biquad**（SubEQ_Core.cpp:121-131）：仅影响音频线程引擎的瞬时响应查询；GUI/设计引擎 smoothing 关闭、走精确目标，无实际影响。

---

## 本轮已验证无问题的方面（锚定核对）

- **processBlock 稳态路径零分配/零锁**：`updateEQParameters`（48 个 `atomic<float>` 读 + 纯数学系数计算）、IIR `processChunk`（复用 `tempBuffer`、栈上无容器）、`processOlaChannel`（`outQueue.reserve(1024)` 后稳态无扩容、就地卷积每样本先读后写安全）逐链核对无 `new/malloc/锁/阻塞`。
- **发布-订阅协议**：`currentState` 所有读写均在 `stateMutex` 下，无撕裂 shared_ptr；`stateVersion` release/acquire 配对正确；`localState/lastSeenVersion/processingState/oldState` 音频线程独占；`getLatencySamples/isReady` 经 grep 核实仅 processBlock 调用，GUI 走原子 `reportedLatency`。
- **overlap-add 与 PDC**：`ConvBlockLen-1 = 511` 管道延迟与 `groupDelay + 511`、AGENTS.md 公式一致；FIR 抽头与卷积全程 double。
- **NaN/Inf 遏制双路径**：IIR 逐样本检查 + 节点状态复位（SubEQ_Core.cpp:188-208）；FIR 块级毒化冲刷（SubEQ_FFTConvolver.h:78-93）+ 设计侧 clamp/impulse 回退。
- **JUCE 7 语义核实**：`getRawParameterValue` 返回反归一化值（本机源码 `&p->getRawDenormalisedValue()`），项目注释与用法正确；ModeSelector 手工同步经 callAsync + SafePointer 正确（v0.3.3 修复有效）。
- **频谱模块**：输出原子逐元素、门控 `spectrumEditorCount` 原子引用计数正确、16/N² 校准推导正确（满幅正弦 = 0 dB）、配置钳位无越界、`configure` 无分配。
- **GUI 同步**：responseEngine 为 double 同款引擎副本（非重复实现），仅 GUI 线程写；曲线路径缓存 + `parametersChanged` 原子门控；采样率变化经 timer 侦测。
- **版本一致性**：`.jucer` version、`JucePluginDefines.h`、CHANGELOG 均为 0.3.4；102 项测试全部通过；Release 构建通过且零警告。

---

## 处理记录（2026-08-14 · v0.3.4）

| 编号 | 修复方式 | 验证 |
|------|----------|------|
| F-001 | epoch 复检移入发布临界区（与 prepare 的 bump+reset 同锁序列化） | 代码审查 + Release 构建 |
| F-002 | 退役环排空与发布替换改为锁外析构；prepare/clearPublishedState 仅指针搬移 | 代码审查 + 构建 |
| F-003 | prepare() 按块尺寸提前重分配 xfade 缓冲（不动设计/epoch） | 代码审查 |
| F-004 | 淡化期间新设计挂起（pendingState），收尾后切换；延迟不同者立即收尾 | 代码审查（与 F-006 联动） |
| F-005 | Release 下 stopThread 失败后阻塞等待线程退出 | 代码审查 |
| F-006 | 仅 groupDelay 相同才交叉淡化；延迟变化冲刷累加器立即切换，PDC 与输出一致 | 代码审查 + CHANGELOG 权衡记录 |
| F-007 | 淡化计划抽为纯函数 `computeBiquadFadePlan`；淡出窗口内再次 update 继续淡出 | 新增 5 项回归（Test 21） |
| F-008 | 旁路 ~15 ms 湿/干交叉淡化 + 热旁路（节点链保持处理） | 代码审查 |
| F-009 | 线程契约注释更正；getEQEngine 注明非线程安全/无调用方 | 代码审查 |
| F-010 | FFTProcessor/Spectrum 音频线程惰性 twiddle 预热（幂等去重） | 代码审查 + 全量测试 |
| F-011 | 三档 Hann 窗构造时一次性生成，configure 仅切指针 | 代码审查 + 全量测试 |
| F-012 | 0dB detent 先比较上一事件 RAW 值、后记录 | 代码审查 |
| F-013 | 两处滚轮注释按实测 deltaY（±0.234/格）更正 | 代码审查 |
| F-014 | 频谱改 gainToY 同轴映射（±24 dB 钳位） | 代码审查 |
| F-015 | createNodeAt Q/Type 与类型菜单补 begin/endChangeGesture | 代码审查 |
| F-016 | 删除 2 个死成员、3 个死方法及全部赋值点 | grep 核实 + 构建 |
| F-017 | 删除死滑杆重载与 Slider 颜色设置；AGENTS.md 描述同步 | grep 核实 + 构建 |
| F-018 | 删除未引用 token/颜色；弹簧/字号/布局收敛到 DesignConstants | grep 核实 + 构建 |
| F-019 | drawRoundedRect 入口防御（空/零尺寸、非正圆角、零缩放） | 代码审查 |
| F-020 | DesignFonts 改 magic static；tinted cache 注释限 GUI 线程 | 代码审查 |
| F-021 | 显式转换 + roundToInt + ignoreUnused | Release 构建零警告 |
| F-022 | 补 `<utility>`/`<string>`/`<functional>` | cl 编译测试通过 |
| F-023 | switch 补 default 写单位系数 | 新增 1 项回归 |
| F-024 | 状态机抽纯函数补回归、Test 5b 注释更正、CHANGELOG 明确测试边界 | 96 → 102 项全绿 |

**验证汇总**：`Tests/subeq_fft_test.exe` 102 项全部通过；Release x64 构建成功、零警告；
VST3 与 Standalone 产物正常生成。剩余人工项：F-006 的"延迟变化冲刷切换"与 F-014 的
"频谱同轴映射"需在宿主中试听确认（原条目即标注待确认听感/是否有意）。
