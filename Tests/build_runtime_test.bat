@echo off
rem Build the JUCE console integration test against the Release SharedCode lib.
call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0.."

set DEFS=/DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 /DJUCE_PROJUCER_VERSION=0x7000c ^
 /DJUCE_ASIO=0 /DJUCE_WEB_BROWSER=0 /DJUCE_USE_MP3AUDIOFORMAT=1 ^
 /DJUCE_USE_LAME_AUDIO_FORMAT=1 /DJUCE_STRICT_REFCOUNTEDPOINTER=1 ^
 /DJUCE_PLUGINHOST_VST3=1 /DJUCE_VST3_CAN_REPLACE_VST2=0 ^
 /DJUCE_DISPLAY_SPLASH_SCREEN=0 /DJUCE_USE_DARK_SPLASH_SCREEN=1 ^
 /DJucePlugin_Build_VST=0 /DJucePlugin_Build_VST3=0 /DJucePlugin_Build_Standalone=0

cl /nologo /EHsc /std:c++17 /O2 /MD /utf-8 ^
 /I"Source" /I"JuceLibraryCode" /I"D:/Program Files/JUCE/modules" ^
 %DEFS% Tests\subeq_runtime_test.cpp ^
 "Builds\VisualStudio2022\x64\Release\Shared Code\Napkid Sub EQ.lib" ^
 /Fe:Tests\subeq_runtime_test.exe /link /SUBSYSTEM:CONSOLE ^
 gdi32.lib user32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib ^
 winmm.lib advapi32.lib comdlg32.lib rpcrt4.lib wininet.lib version.lib ^
 ws2_32.lib dbghelp.lib opengl32.lib gdiplus.lib Msimg32.lib comctl32.lib ^
 dwmapi.lib
