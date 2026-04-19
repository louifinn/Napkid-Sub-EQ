# Changelog

All notable changes to Napkid Sub EQ will be documented in this file.

## [0.1.1] - 2026-04-19

### Added

- **Band Pass filter type**: New "Band Pass" node type added to the 8 filter options.
- **Smart vertical drag for non-gain types**: HP / LP / Notch / Band Pass nodes now adjust Q on vertical drag instead of gain, with the node fixed on the 0 dB reference line.
- **Type-aware double-click reset**: Bell / Shelf / Tilt reset gain + Q; HP / LP / Notch / BP reset Q only.

### Fixed

- **Notch crash**: Notch nodes at center frequency produced `-Inf` dB response, causing JUCE Path assertion failure. Now clamped to `-120 ~ +60 dB` safe range.
- **Q drag direction**: Upward drag now increases Q, downward decreases Q for all non-gain-sensitive types (intuitive match with gain drag direction).
- **Text editor dismissal**: Text edit boxes now close when clicking or dragging anywhere outside the editor.
- **Gain reset on type switch**: Switching from gain-sensitive type (Bell/Shelf/Tilt) to non-gain type (HP/LP/Notch/BP) now resets Gain to 0 dB automatically.

### Changed

- Node vertical position is now type-aware: gain-sensitive types (Bell, LowShelf, HighShelf, Tilt) follow gain; others anchor to 0 dB.
- **Q adjustment now uses logarithmic mapping** for both drag and scroll wheel: equal input change produces equal *ratio* change (0.1→1 takes same motion as 1→10).
- **Frequency display precision unified**: All frequency readouts (node labels, text editor, grid tooltips) now consistently show 2 decimal places, removing the previous `≥100 Hz → 0 decimals` branch that caused inconsistent display.

## [0.1.0] - Initial Release

### Added

- 8-node parametric EQ (0.5 Hz ~ 500 Hz)
- 8 filter types: Bell, High Pass, Low Pass, Low Shelf, High Shelf, Notch, Tilt, Band Pass
- Real-time frequency response and phase curves
- Real-time spectrum analyzer (1/6 octave, 8192-point FFT)
- Pro-Q2 style node interaction (click, drag, right-click delete, scroll Q)
- Double-click reset
- Master gain control
- VST3 / Standalone builds
- ASIO support
