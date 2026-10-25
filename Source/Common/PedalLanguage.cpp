#include "Common/PedalLanguage.h"

#include <juce_graphics/juce_graphics.h>

#include <utility>

namespace pedal
{

namespace
{

Language current = Language::english;
bool chosen = false;
juce::String family;

/*  Every line is "english" = "chinese". Lines that do not begin with a quote are
    comments to juce::LocalisedStrings, which is what parses this. The left-hand
    strings have to match the literals at the tr() call sites character for
    character, including runs of two or three spaces; _toolchain_test\
    check_translations.ps1 compares the two sets and reports the ones that do not
    line up, because a key that misses simply shows English with no warning. */
const char* const chineseTable = R"ZH(
language: Chinese (Simplified)

// ---- effect names ----------------------------------------------------------
"Noise Gate" = "噪声门"
"Compressor" = "压缩器"
"3-Band EQ" = "三段均衡"
"Auto Wah" = "自动哇音"
"Wah Pedal" = "哇音踏板"
"Overdrive" = "过载"
"Distortion" = "失真"
"Fuzz" = "法兹"
"Bitcrusher" = "位降采样"
"Chorus" = "合唱"
"Flanger" = "镶边"
"Phaser" = "移相"
"Tremolo" = "颤音"
"Vibrato" = "揉弦"
"Octave" = "八度"
"Pitch Shifter" = "移调"
"Delay" = "延迟"
"Reverb" = "混响"
"Cabinet Simulator" = "箱体模拟"
"Ring Modulator" = "环形调制"
"Looper" = "循环录音"
"Acoustic to Electric" = "木琴转电琴"
"Custom Chain" = "自定义链路"
"Plugin Host" = "插件宿主"

// ---- categories ------------------------------------------------------------
"Dynamics" = "动态"
"Filter" = "滤波"
"Modulation" = "调制"
"Pitch" = "音高"
"Time" = "时间"
"Special" = "特殊"
"Conversion" = "转换"
"Custom" = "自定义"

// ---- parameter names -------------------------------------------------------
"Threshold" = "阈值"
"Attack" = "启动"
"Release" = "释放"
"Hold" = "保持"
"Range" = "范围"
"Mix" = "混合"
"Ratio" = "压缩比"
"Knee" = "拐点"
"Makeup" = "补偿增益"
"Bass" = "低音"
"Treble" = "高音"
"Mid" = "中音"
"Low" = "低频"
"High" = "高频"
"Low Freq" = "低频频率"
"Mid Freq" = "中频频率"
"High Freq" = "高频频率"
"Mid Q" = "中频 Q"
"Min Freq" = "下限频率"
"Max Freq" = "上限频率"
"Centre" = "中心"
"Width" = "宽度"
"Drive" = "驱动"
"Tone" = "音色"
"Level" = "电平"
"Blend" = "混合度"
"Bias" = "偏压"
"Squash" = "压缩感"
"Presence" = "临场度"
"Body Cut" = "箱体共鸣削减"
"Quack Cut" = "压电鼻音削减"
"Air Cut" = "空气感削减"
"Depth" = "深度"
"Rate" = "速率"
"Feedback" = "反馈"
"Decay" = "衰减"
"Damping" = "阻尼"
"Pre-Delay" = "预延迟"
"Spread" = "立体声展开"
"Mod" = "磁带调制"
"Hysteresis" = "迟滞"
"Sidechain" = "侧链"
"Direction" = "方向"
"Up Sweep" = "向上扫频"
"Down Sweep" = "向下扫频"
"Band Pass" = "带通"
"Low Pass" = "低通"
"Sensitivity" = "灵敏度"
"Position" = "位置"
"Frequency" = "频率"
"Semitones" = "半音"
"Formant" = "共振峰"
"Octave Up" = "高八度"
"Octave Down" = "低八度"
"Delay Time" = "延迟时间"
"Param 1" = "参数 1"
"Param 2" = "参数 2"
"Param 3" = "参数 3"
"Param 4" = "参数 4"
"Param 5" = "参数 5"
"Param 6" = "参数 6"
"Param 7" = "参数 7"
"Param 8" = "参数 8"
"Window" = "窗长"
"Stages" = "级数"
"Shape" = "波形"
"Mode" = "模式"
"Source" = "来源"
"Slider" = "滑块"
"Bits" = "位深"
"Downsample" = "降采样"
"Tighten" = "收紧"
"Sine" = "正弦"
"Triangle" = "三角"
"Square" = "方波"
"Stereo" = "立体声"
"Ping-Pong" = "乒乓"

// ---- card buttons and per-effect notes -------------------------------------
"ON" = "开启"
"Up" = "上移"
"Down" = "下移"
"bypassed - the dry signal passes through" = "已旁通 — 直通原始信号"
"Rec" = "录音"
"Play / Stop" = "播放 / 停止"
"Overdub" = "叠加"
"Undo" = "撤销"
"Clear" = "清除"
"Load IR..." = "载入 IR…"
"Load VST3..." = "载入 VST3…"
"Load VST3 plugin" = "载入 VST3 插件"
"Open Editor" = "打开编辑器"
"No plugin loaded - use Load VST3" = "未载入插件 — 请点击「载入 VST3」"
"Plugin: %1 (%2 of %3 params on knobs)" = "插件：%1（%2 / %3 个参数已映射到旋钮）"
"Plugin error: %1" = "插件错误：%1"
"The previous plugin, if any, is still active. Only .vst3 effect files are supported." = "之前的插件（如果有）仍在工作。仅支持 .vst3 效果器文件。"
"Hosts a VST3 effect. The file stays where it is; this app does not install or copy plugins." = "用于载入一个 VST3 效果器。插件文件保留在原位；本程序不会安装或复制插件。"
"The eight knobs follow the plugin's first eight automatable parameters, skipping bypass; open the editor for all controls." = "八个旋钮对应插件的前八个可自动化参数（跳过旁通）；如需全部控件请打开编辑器。"
"One custom chain: drive, tone, chorus, delay, reverb - all stages share these knobs." = "一条自定义链路：驱动、音色、合唱、延迟、混响 — 所有级共用这些旋钮。"
"plugin file not found" = "找不到插件文件"
"no VST3 format could open this file" = "没有 VST3 格式能打开此文件"
"the plugin crashed or refused to be scanned" = "插件崩溃或拒绝被扫描"
"no plugin class found in this file" = "此文件中没有找到插件类"
"this file is an instrument, not an effect" = "此文件是乐器，不是效果器"
"the plugin could not be instantiated" = "无法创建插件实例"
"the plugin has no audio input or output" = "插件没有音频输入或输出"
"more than stereo input/output is not supported" = "不支持超过立体声的输入 / 输出"
"this plugin has no editor" = "此插件没有编辑器"
"the plugin editor could not be created" = "无法创建插件编辑器"
"1x12 V30" = "1x12 V30"
"2x12 Open" = "2x12 开放式"
"4x12 High Gain" = "4x12 高增益"
"No IR loaded - use Load IR to pick a file" = "尚未载入 IR — 请点击「载入 IR」选择文件"
"IR: %1" = "IR：%1"
"Recording" = "录音中"
"Playing" = "播放中"
"Overdubbing" = "叠加中"
"Stopped" = "已停止"
"%1  -  loop %2 s / %3 s" = "%1  -  循环 %2 秒 / %3 秒"
"Wah sweep follows MIDI CC 11" = "哇音扫频跟随 MIDI CC 11"
"Source is MIDI CC but no MIDI input is open - the Position knob drives the sweep" = "来源设为 MIDI CC，但没有打开的 MIDI 输入 — 扫频改由「位置」旋钮控制"
"Single notes track best - the divider and the rectifier are both monophonic." = "单音跟踪效果最好 — 分频器与整流器都只适用于单音。"
"Spectral pitch shifter: clean tracking on chords and single notes, about 80 ms latency." = "频谱移调：和弦与单音都能干净跟踪，约 80 ms 延迟。"
"IR error: %1" = "IR 错误：%1"
"file not found" = "找不到文件"
"unsupported or unreadable audio file" = "不支持或无法读取的音频文件"
"the file has no usable audio" = "文件内没有可用音频"
"impulse responses longer than 10 seconds are not supported" = "不支持超过 10 秒的冲激响应"
"The previous response is still active. Load a WAV, AIFF, FLAC, OGG or MP3 file." = "当前响应仍在工作。请载入 WAV、AIFF、FLAC、OGG 或 MP3 文件。"
"The cabinet stays inactive until an impulse response is loaded. WAV, AIFF, FLAC, OGG and MP3 files are supported." = "载入冲激响应前箱体不会工作。支持 WAV、AIFF、FLAC、OGG 与 MP3 文件。"
"Pick a built-in cabinet, load your own IR, then set Mix to taste (100 % is fully cabinet)." = "选择一个内置箱体，或载入自己的 IR，再用 Mix 调整比例（100 % 为全箱体）。"
"External IR: resampled, trimmed and normalised to the current rate. WAV, AIFF, FLAC, OGG and MP3 are supported." = "外部 IR：已重采样、裁剪并归一化到当前采样率。支持 WAV、AIFF、FLAC、OGG 与 MP3。"
"Rec starts on the next audio block; the tape is lost when the device changes." = "录音将在下一个音频块开始；更换音频设备会清除已录内容。"
"Fixed 190 Hz and 380 Hz body cuts: put a 3-Band EQ in front if your guitar's first mode sits elsewhere, and a Cabinet after it." = "固定的 190 Hz 与 380 Hz 箱体削减：若你的吉他第一共鸣点不在此处，请在前面加入三段均衡，后面加入箱体模拟。"
"The cabinet stays inactive until an impulse response is loaded. A new response is crossfaded in by the engine, so switching never clicks." = "在载入脉冲响应之前，箱体不会工作。新的响应由引擎交叉淡入，因此切换不会产生爆音。"
"External IR: resampled, trimmed and normalised to the current rate." = "外部 IR：已重采样、裁剪并归一化到当前采样率。"
"external file" = "外部文件"
"1x12 V30 closed back (synthetic)" = "1x12 V30 密闭背板（合成）"
"2x12 open back (synthetic)" = "2x12 开放背板（合成）"
"4x12 high gain (synthetic)" = "4x12 高增益（合成）"

// ---- window chrome ---------------------------------------------------------
"Add effect..." = "添加效果…"
"Dry/Wet" = "干湿比"
"Input Trim" = "输入增益"
"Expression" = "表情踏板"
"MONITOR" = "监听"
"TO CABLE" = "虚拟输出"
"CABLE" = "虚拟线"
"Devices" = "音频设备"
// "INPUT DEVICE", "MONITOR DEVICE" and "SAMPLE RATE" are shared with the chooser's
// captions below; a key appears in the table once.
"IN TYPE" = "输入类型"
"OUT TYPE" = "输出类型"
"BUFFER" = "缓冲区"
"INTERFACE LANGUAGE" = "界面语言"
"No audio device is running. Open Devices to pick an input and an output." = "当前没有正在运行的音频设备。请打开「音频设备」选择输入与输出。"
"SIGNAL CHAIN  (drag a card by its header to reorder)" = "效果链（拖动卡片标题栏即可重新排序）"
"The chain is empty." = "效果链是空的。"
"Pick an effect from the Add effect box at the top right." = "请从右上角的「添加效果」下拉框中选择。"
"That effect cannot be added: the chain holds %1 slots and no type may appear twice." = "无法添加该效果：效果链共有 %1 个插槽，且同一效果不能重复出现。"

// ---- device chooser --------------------------------------------------------
"Audio devices" = "音频设备"
"Input and output are separate on purpose: the guitar enters one device and the chain leaves through the other, and switching either one leaves the second running." = "输入与输出刻意分开：吉他从一个设备进入，效果链从另一个设备离开，切换其中任意一个都不会中断另一个。"
"INPUT DEVICE TYPE" = "输入设备类型"
"INPUT DEVICE" = "输入设备"
"MONITOR DEVICE TYPE" = "监听设备类型"
"MONITOR DEVICE" = "监听设备"
"BUFFER SIZE" = "缓冲区大小"
"SAMPLE RATE" = "采样率"
"VIRTUAL OUTPUT" = "虚拟输出"
"VIRTUAL OUT" = "虚拟输出"
"EXPRESSION PEDAL (CC11 / CC1)" = "表情踏板（CC11 / CC1）"
"Rescan" = "重新扫描"
"Apply" = "应用"
"Close" = "关闭"
"(no MIDI input)" = "（无 MIDI 输入）"
"Input: %1" = "输入：%1"
"Output: %1" = "输出：%1"
"Sample rate: %1" = "采样率：%1"
"Buffer size: %1" = "缓冲区：%1"
"MIDI: %1" = "MIDI：%1"
"VB-CABLE is present but the stream is not running. Press Apply to open it, or switch VIRTUAL OUT off and on again." = "已检测到 VB-CABLE，但数据流尚未运行。请点击「应用」打开它，或关闭再打开「虚拟输出」。"
"Recording through CABLE Output adds its own buffer on top of the %1 ms shown here, because the cable is opened over %2. The monitor path is the low-latency one; recording is not." = "通过 CABLE Output 录音会在这里显示的 %1 毫秒之上再叠加它自己的缓冲区，因为该线缆是通过 %2 打开的。监听路径才是低延迟的那一条，录音不是。"
"the default driver" = "默认驱动"

// ---- virtual cable status --------------------------------------------------
"Virtual recording is switched off." = "虚拟录音已关闭。"
"VB-CABLE is not installed." = "未安装 VB-CABLE。"
"VB-CABLE not detected - virtual recording unavailable" = "未检测到 VB-CABLE — 无法进行虚拟录音"
"VB-CABLE found - virtual recording switched off" = "已检测到 VB-CABLE — 虚拟录音已关闭"
"Input peak %1 dBFS   %2" = "输入峰值 %1 dBFS   %2"
"VB-CABLE found - stream not started" = "已检测到 VB-CABLE — 数据流未启动"
"Virtual Device: %1 - Active, adds about %2 ms to the recording path" = "虚拟设备：%1 — 运行中，为录音路径增加约 %2 毫秒"
"VB-CABLE could not be opened: %1" = "无法打开 VB-CABLE：%1"
"The cable device reported no open stream." = "线缆设备未返回已打开的音频流。"
"Recording through a virtual soundcard needs VB-CABLE, which this application neither bundles nor installs." = "通过虚拟声卡录音需要 VB-CABLE，本程序既不打包也不安装它。"
"VB-CABLE is Donationware by Vincent Burel. If you keep using it the author asks for a donation; the exact terms are on the download page." = "VB-CABLE 是 Vincent Burel 的 Donationware（捐赠软件）。若继续使用，作者请求捐赠；具体条款见下载页面。"
"Install it from https://vb-audio.com/Cable/ and then restart PedalBoard: Windows only publishes newly installed audio devices to an application at process start, so there is no way for this app to see it sooner." = "请从 https://vb-audio.com/Cable/ 安装，然后重启 PedalBoard：Windows 只在进程启动时向应用公布新安装的音频设备，因此本程序无法更早看到它。"
"Nothing else is blocked in the meantime. The input device, the effect chain, the physical monitoring path and both meters work without it." = "在此期间其他功能都不受影响。输入设备、效果链、实体监听路径和两个电平表都无需 VB-CABLE 即可工作。"

// ---- engine messages and the status bar ------------------------------------
"Midi device not found: %1" = "未找到 MIDI 设备：%1"
"Could not open %1" = "无法打开 %1"
"No input device selected." = "尚未选择输入设备。"
"No output device selected." = "尚未选择输出设备。"
"Input audio system: %1" = "输入音频系统：%1"
"Output audio system: %1" = "输出音频系统：%1"
"Virtual device: %1" = "虚拟设备：%1"
"Input %1 / %2   Output %3 / %4" = "输入 %1 / %2   输出 %3 / %4"
"Latency  input %1   monitor %2   chain %3" = "延迟  输入 %1   监听 %2   链路 %3"
" (dry compensation at its limit)" = "（直通补偿已达上限）"
"   virtual +%1" = "   虚拟 +%1"
"CPU %1%   sample rate %2 Hz   block %3   slots %4/%5   dropped blocks  input %6 (over %7 / under %8)  virtual %9 (over %10 / under %11)" = "CPU %1%   采样率 %2 Hz   块长 %3   插槽 %4/%5   丢弃块数  输入 %6（溢出 %7 / 不足 %8）  虚拟 %9（溢出 %10 / 不足 %11）"
"The monitoring path is not running. Open Devices and apply an output device." = "监听路径未运行。请打开「音频设备」并应用一个输出设备。"

// ---- dynamics and filter footers -------------------------------------------
"Gain reduction %1 dB" = "增益衰减 %1 dB"
"Open" = "开启"
"Closed" = "关闭"
"Q" = "Q 值"
"MIDI CC" = "MIDI CC"

// ---- native titles and device recovery ------------------------------------
"PedalBoard - guitar chain, monitor and virtual cable output" = "PedalBoard — 吉他效果链、监听与虚拟声卡输出"
"Load impulse response" = "载入冲激响应"
"the previous device could not be restored" = "无法恢复之前的设备"
)ZH";

/** Probed, not assumed: asking for a typeface that is not installed does not
    fail on Windows, DirectWrite hands back a substitute, so the only useful test
    is whether the name that comes back is the one that went in. */
juce::String probeChineseFamily()
{
    for (auto* candidate : { "Microsoft YaHei UI", "Microsoft YaHei", "SimHei", "DengXian" })
    {
        auto typeface = juce::Typeface::createSystemTypefaceFor (
            juce::Font (juce::FontOptions (candidate, 14.0f, juce::Font::plain)));

        if (typeface != nullptr && typeface->getName().containsIgnoreCase (candidate))
            return candidate;
    }

    return {};
}

} // namespace

//==============================================================================
void setLanguage (Language newLanguage)
{
    current = newLanguage;

    juce::LocalisedStrings::setCurrentMappings (
        newLanguage == Language::chinese
            ? new juce::LocalisedStrings (juce::String::fromUTF8 (chineseTable), false)
            : nullptr);

    /*  Recomputed rather than cached once: the probe costs a DirectWrite call and
        a language switch is something that happens a handful of times per
        session, if that. */
    family = newLanguage == Language::chinese ? probeChineseFamily() : juce::String();
}

Language currentLanguage() noexcept { return current; }
bool languageWasChosen() noexcept    { return chosen; }

void noteLanguageChosen() noexcept { chosen = true; }

juce::String languageName (Language language)
{
    return language == Language::chinese ? u8 ("中文") : juce::String ("English");
}

const juce::String& uiFontFamily() noexcept { return family; }

/*  fromUTF8 rather than the implicit const char* conversion: the table's values
    are Chinese, and juce::String's narrow constructor would hand them over as one
    Latin-1 character per byte. The keys are ASCII, so this costs a scan for
    high bytes and nothing else. */
juce::String tr (const char* english)
{
    return juce::translate (u8 (english));
}

juce::String tr (const juce::String& english)
{
    return juce::translate (english);
}

} // namespace pedal
