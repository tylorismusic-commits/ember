# Changelog

## 0.1.2

- The IN/BYPASS control is now a normal plugin parameter, not the CLAP host
  bypass. Toggling it no longer changes REAPER's own FX bypass state.

## 0.1.1

- CLAP builds now feed the plugin's own VU and gain-reduction meters, so the
  native UI behaves the same in CLAP and LV2.
- The MASTERING CHAIN detail panel is hidden by default; the new **INFO**
  button in the header shows or hides it.
- Added a CLAP host test covering gain-reduction reporting and UI embedding.

## 0.1.0

- Initial Ember release: a one-knob mastering finish for the Hearth family.
- Amount macro drives a staged low shelf, presence bell, air shelf, glue
  compressor, harmonic stage, and finishing level.
- Added programme-dependent auto makeup and a band-limited ADAA harmonic stage
  that keeps the dry air band open.
- Added hidden +0.2 dBFS soft-clip protection and a clean master bypass.
- Added the slate-blue console UI with a large Amount knob, glass VU,
  glue-reduction meter, five stage meters, and six presets.
- Added LV2 and CLAP builds, factory presets, and DSP/host/UI tests.
