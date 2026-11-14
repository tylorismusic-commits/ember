// Ember — X11/Cairo one-knob mastering UI
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include "macro.hpp"
#include "param_info.hpp"
#include "hearth_theme.hpp"

namespace ember {
namespace ui {

// Logical layout.  The UI is rendered at this size and scaled to whatever the
// host gives us, so it stays sharp and fits small and large displays.
constexpr int UI_W = 620;
constexpr int UI_H = 900;
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Palette — the same vintage console faceplate as the rest of the family.
// The values are loaded from the selected suite skin at runtime.
// ---------------------------------------------------------------------------
using Color = hearth::theme::Color;

static Color kBg;
static Color kBgTop;
static Color kBgBottom;
static Color kHeaderTop;
static Color kHeaderBottom;
static Color kPanelTop;
static Color kPanelBottom;
static Color kPanelEdge;
static Color kPanelInner;
static Color kPanelHeaderTop;
static Color kPanelHeaderBottom;
static Color kPanelHeaderText;
static Color kText;
static Color kCream;
static Color kMuted;
static Color kRed;
static Color kRedDark;
static Color kAmber;
static Color kKnobTop;
static Color kKnobBottom;
static Color kKnobRing;
static Color kSilver;
static Color kTrack;
static Color kMeterFace;
static Color kMeterInk;
static Color kGreen;
static Color kHighlight;
static Color kShadow;
static Color kChassisLight;
static Color kChassisMid;
static Color kChassisDark;
static double gTextureScale = 1.0;

static void applyTheme(const hearth::theme::Theme& t) {
  kBg = t.background;
  kBgTop = t.background_top;
  kBgBottom = t.background_bottom;
  kHeaderTop = t.header_top;
  kHeaderBottom = t.header_bottom;
  kPanelTop = t.panel_top;
  kPanelBottom = t.panel_bottom;
  kPanelEdge = t.panel_edge;
  kPanelInner = t.panel_inner;
  kPanelHeaderTop = t.panel_header_top;
  kPanelHeaderBottom = t.panel_header_bottom;
  kPanelHeaderText = t.panel_header_text;
  kText = t.text;
  kCream = t.cream;
  kMuted = t.muted;
  kRed = t.accent;
  kRedDark = t.accent_dark;
  kAmber = t.amber;
  kKnobTop = t.knob_top;
  kKnobBottom = t.knob_bottom;
  kKnobRing = t.knob_ring;
  kSilver = t.silver;
  kTrack = t.track;
  kMeterFace = t.meter_face;
  kMeterInk = t.meter_ink;
  kGreen = t.green;
  kHighlight = t.highlight;
  kShadow = t.shadow;
  kChassisLight = t.chassis_light;
  kChassisMid = t.chassis_mid;
  kChassisDark = t.chassis_dark;
  const double base = 0.06;
  gTextureScale = base > 0.0 ? std::min(std::max(t.texture_opacity / base, 0.0), 2.0)
                            : 0.0;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
enum class Kind { Knob, Toggle, Vu, Meter };

struct Widget {
  Kind kind;
  uint32_t port;
  bool bipolar;
  float cx, cy, r;
  float x, y, w, h;
};

constexpr Widget K(uint32_t port, float cx, float cy, float r,
                   bool bipolar = false) {
  return Widget{Kind::Knob, port, bipolar, cx, cy, r, 0, 0, 0, 0};
}
constexpr Widget T(uint32_t port, float x, float y, float w, float h) {
  return Widget{Kind::Toggle, port, false, 0, 0, 0, x, y, w, h};
}
constexpr Widget V(uint32_t port, float x, float y, float w, float h) {
  return Widget{Kind::Vu, port, false, 0, 0, 0, x, y, w, h};
}
constexpr Widget M(uint32_t port, float x, float y, float w, float h) {
  return Widget{Kind::Meter, port, false, 0, 0, 0, x, y, w, h};
}

constexpr Widget kWidgets[] = {
    // The one control that matters.
    K(PORT_AMOUNT, 235.0f, 288.0f, 108.0f),

    // Output column
    V(PORT_METER_OUT, 472.0f, 120.0f, 130.0f, 130.0f),
    M(PORT_METER_GR, 536.0f, 270.0f, 28.0f, 500.0f),

    // Global
    T(PORT_ENABLE, 512.0f, 22.0f, 88.0f, 32.0f),
};

constexpr int kNumWidgets = static_cast<int>(sizeof(kWidgets) / sizeof(kWidgets[0]));

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct State {
  float values[PORT_COUNT] = {};
  float meterGr = 0.0f;
  float meterOut = -60.0f;
  float meterGrSmooth = 0.0f;
  float vuSmooth = -60.0f;
  int hover = -1;
  int active = -1;
  double dragY = 0.0;
  float dragValue = 0.0f;
  int lastClickWidget = -1;
  unsigned long lastClickTime = 0;
  int presetIndex = -1;
  bool presetDirty = false;
  int presetHover = 0;
  int infoHover = 0;
  bool showChain = false;
  bool skinHover = false;
  char skinToast[64] = {};
  double skinToastUntil = 0.0;
  bool dirty = true;
};

static const ParamInfo& infoFor(uint32_t port) {
  for (int i = 0; i < kParamInfoCount; ++i) {
    if (kParams[i].index == port) {
      return kParams[i];
    }
  }
  static ParamInfo fallback;
  return fallback;
}

static void initState(State& s) {
  for (int i = 0; i < kParamInfoCount; ++i) {
    s.values[kParams[i].index] = kParams[i].def;
  }
  s.meterGr = 0.0f;
  s.meterOut = -60.0f;
  s.meterGrSmooth = 0.0f;
  s.vuSmooth = -60.0f;
}

static double nowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void showSkinToast(State& s, const char* name) {
  std::snprintf(s.skinToast, sizeof(s.skinToast), "%s",
                name && *name ? name : "Classic");
  s.skinToastUntil = nowSeconds() + 1.6;
}

static float normOf(const ParamInfo& p, float v) {
  if (p.toggle || p.enumeration) {
    return v >= 0.5f ? 1.0f : 0.0f;
  }
  if (p.logarithmic && p.min > 0.0f && p.max > p.min) {
    return std::log(std::max(v, p.min) / p.min) / std::log(p.max / p.min);
  }
  if (p.max <= p.min) {
    return 0.0f;
  }
  return (v - p.min) / (p.max - p.min);
}

static float valueOf(const ParamInfo& p, float n) {
  n = std::min(std::max(n, 0.0f), 1.0f);
  if (p.toggle || p.enumeration) {
    return n >= 0.5f ? 1.0f : 0.0f;
  }
  if (p.logarithmic && p.min > 0.0f && p.max > p.min) {
    return p.min * std::pow(p.max / p.min, n);
  }
  return p.min + n * (p.max - p.min);
}

static std::string formatValue(uint32_t port, float v) {
  char buf[64];
  if (port == PORT_ENABLE) {
    return v >= 0.5f ? "ON" : "OFF";
  }
  std::snprintf(buf, sizeof(buf), "%.0f %%", v);
  return buf;
}

static const char* shortLabel(uint32_t port) {
  return port == PORT_AMOUNT ? "AMOUNT" : "";
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
struct PresetValue {
  uint32_t port;
  float value;
};

struct Preset {
  const char* name;
  const char* shortName;
  const PresetValue* values;
  int count;
};

#define PV(port_, value_) PresetValue{port_, value_}

constexpr PresetValue kPresetTouch[] = {
    PV(PORT_AMOUNT, 18.0f), PV(PORT_ENABLE, 1.0f)};
constexpr PresetValue kPresetVocal[] = {
    PV(PORT_AMOUNT, 35.0f), PV(PORT_ENABLE, 1.0f)};
constexpr PresetValue kPresetMixBus[] = {
    PV(PORT_AMOUNT, 48.0f), PV(PORT_ENABLE, 1.0f)};
constexpr PresetValue kPresetDrumBus[] = {
    PV(PORT_AMOUNT, 58.0f), PV(PORT_ENABLE, 1.0f)};
constexpr PresetValue kPresetMaster[] = {
    PV(PORT_AMOUNT, 72.0f), PV(PORT_ENABLE, 1.0f)};
constexpr PresetValue kPresetBroken[] = {
    PV(PORT_AMOUNT, 100.0f), PV(PORT_ENABLE, 1.0f)};

#undef PV

constexpr Preset kPresets[] = {
    {"Touch of Glow", "TOUCH", kPresetTouch,
     static_cast<int>(sizeof(kPresetTouch) / sizeof(kPresetTouch[0]))},
    {"Vocal Bus", "VOCAL", kPresetVocal,
     static_cast<int>(sizeof(kPresetVocal) / sizeof(kPresetVocal[0]))},
    {"Mix Bus Finish", "MIX BUS", kPresetMixBus,
     static_cast<int>(sizeof(kPresetMixBus) / sizeof(kPresetMixBus[0]))},
    {"Drum Bus Glue", "DRUMS", kPresetDrumBus,
     static_cast<int>(sizeof(kPresetDrumBus) / sizeof(kPresetDrumBus[0]))},
    {"Mastering", "MASTER", kPresetMaster,
     static_cast<int>(sizeof(kPresetMaster) / sizeof(kPresetMaster[0]))},
    {"Broken Ember", "BROKEN", kPresetBroken,
     static_cast<int>(sizeof(kPresetBroken) / sizeof(kPresetBroken[0]))},
};

constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

struct Rect {
  double x, y, w, h;
};

constexpr Rect kPresetLcd{170.0, 24.0, 190.0, 36.0};
constexpr Rect kPresetPrev{366.0, 24.0, 26.0, 36.0};
constexpr Rect kPresetNext{396.0, 24.0, 26.0, 36.0};
constexpr Rect kInfoButton{462.0, 30.0, 44.0, 22.0};
constexpr Rect kSkinChip{430.0, 24.0, 26.0, 26.0};

static bool inRect(double x, double y, const Rect& r) {
  return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void setColor(cairo_t* cr, const Color& c) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static void setColorA(cairo_t* cr, const Color& c, double alpha) {
  cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

static void roundedRect(cairo_t* cr, double x, double y, double w, double h,
                        double r) {
  r = std::min(r, std::min(w, h) * 0.5);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -kPi * 0.5, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, kPi * 0.5);
  cairo_arc(cr, x + r, y + h - r, r, kPi * 0.5, kPi);
  cairo_arc(cr, x + r, y + r, r, kPi, kPi * 1.5);
  cairo_close_path(cr);
}

static void textAt(cairo_t* cr, double x, double y, const char* text,
                   double size, bool bold, const Color& c,
                   const char* family = "DejaVu Sans") {
  cairo_select_font_face(cr, family, CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD
                              : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, size);
  setColor(cr, c);
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, text);
}

static void textCentered(cairo_t* cr, double cx, double y, const char* text,
                         double size, bool bold, const Color& c,
                         const char* family = "DejaVu Sans") {
  cairo_select_font_face(cr, family, CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD
                              : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, size);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, text, &ext);
  setColor(cr, c);
  cairo_move_to(cr, cx - (ext.width * 0.5 + ext.x_bearing), y);
  cairo_show_text(cr, text);
}

static void paintTexture(cairo_t* cr, double x, double y, double w, double h,
                         double alpha);

static void drawScrew(cairo_t* cr, double x, double y) {
  cairo_arc(cr, x, y + 0.8, 5.2, 0.0, kPi * 2.0);
  setColorA(cr, kBg, 0.55);
  cairo_fill(cr);

  cairo_pattern_t* head = cairo_pattern_create_radial(
      x - 1.8, y - 1.8, 0.5, x, y, 4.8);
  cairo_pattern_add_color_stop_rgb(head, 0.0, 0.74, 0.75, 0.72);
  cairo_pattern_add_color_stop_rgb(head, 0.55, 0.40, 0.42, 0.42);
  cairo_pattern_add_color_stop_rgb(head, 1.0, 0.13, 0.15, 0.17);
  cairo_arc(cr, x, y, 4.2, 0.0, kPi * 2.0);
  cairo_set_source(cr, head);
  cairo_fill(cr);
  cairo_pattern_destroy(head);

  cairo_arc(cr, x, y, 4.2, 0.0, kPi * 2.0);
  setColorA(cr, kBg, 0.85);
  cairo_set_line_width(cr, 0.8);
  cairo_stroke(cr);

  setColorA(cr, kBg, 0.8);
  cairo_set_line_width(cr, 1.2);
  cairo_move_to(cr, x - 2.2, y - 2.2);
  cairo_line_to(cr, x + 2.2, y + 2.2);
  cairo_stroke(cr);

  setColorA(cr, kHighlight, 0.30);
  cairo_set_line_width(cr, 0.7);
  cairo_move_to(cr, x - 2.0, y - 2.6);
  cairo_line_to(cr, x + 1.6, y - 1.0);
  cairo_stroke(cr);
}

static void drawPanel(cairo_t* cr, double x, double y, double w, double h,
                      const char* title) {
  // raised chassis shadow
  roundedRect(cr, x + 1.0, y + 4.0, w, h, 8.0);
  setColorA(cr, kBg, 0.38);
  cairo_fill(cr);
  roundedRect(cr, x + 0.5, y + 2.0, w, h, 8.0);
  setColorA(cr, kBg, 0.24);
  cairo_fill(cr);

  cairo_pattern_t* pat = cairo_pattern_create_linear(x, y, x, y + h);
  cairo_pattern_add_color_stop_rgb(pat, 0.0, kPanelTop.r, kPanelTop.g,
                                   kPanelTop.b);
  cairo_pattern_add_color_stop_rgb(pat, 1.0, kPanelBottom.r, kPanelBottom.g,
                                   kPanelBottom.b);
  roundedRect(cr, x, y, w, h, 7.0);
  cairo_set_source(cr, pat);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(pat);
  setColor(cr, kPanelEdge);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  // brushed texture and recessed top shadow inside the panel
  cairo_save(cr);
  roundedRect(cr, x, y, w, h, 7.0);
  cairo_clip(cr);
  paintTexture(cr, x, y, w, h, 0.065);
  cairo_pattern_t* inner = cairo_pattern_create_linear(x, y, x, y + 18.0);
  cairo_pattern_add_color_stop_rgba(inner, 0.0, 0.0, 0.0, 0.0, 0.28);
  cairo_pattern_add_color_stop_rgba(inner, 1.0, 0.0, 0.0, 0.0, 0.0);
  cairo_rectangle(cr, x, y, w, 18.0);
  cairo_set_source(cr, inner);
  cairo_fill(cr);
  cairo_pattern_destroy(inner);
  cairo_restore(cr);

  // top bevel and bottom shadow
  setColorA(cr, kPanelInner, 0.75);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, x + 9.0, y + 1.0);
  cairo_line_to(cr, x + w - 9.0, y + 1.0);
  cairo_stroke(cr);
  setColorA(cr, kBg, 0.55);
  cairo_move_to(cr, x + 9.0, y + h - 1.0);
  cairo_line_to(cr, x + w - 9.0, y + h - 1.0);
  cairo_stroke(cr);

  // recessed title plate
  roundedRect(cr, x + 8.0, y + 8.0, w - 16.0, 27.0, 3.5);
  cairo_pattern_t* plate = cairo_pattern_create_linear(
      x, y + 8.0, x, y + 35.0);
  cairo_pattern_add_color_stop_rgb(plate, 0.0, kPanelHeaderTop.r,
                                   kPanelHeaderTop.g, kPanelHeaderTop.b);
  cairo_pattern_add_color_stop_rgb(plate, 1.0, kPanelHeaderBottom.r,
                                   kPanelHeaderBottom.g,
                                   kPanelHeaderBottom.b);
  cairo_set_source(cr, plate);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(plate);
  setColorA(cr, kPanelEdge, 0.8);
  cairo_set_line_width(cr, 0.8);
  cairo_stroke(cr);

  textAt(cr, x + 16.0, y + 26.0, title, 11.0, true, kPanelHeaderText);
  setColor(cr, kRed);
  cairo_rectangle(cr, x + 16.0, y + 33.0, 24.0, 2.0);
  cairo_fill(cr);

  cairo_arc(cr, x + w - 18.0, y + 21.0, 2.2, 0.0, kPi * 2.0);
  setColorA(cr, kRed, 0.85);
  cairo_fill(cr);
  cairo_arc(cr, x + w - 18.0, y + 21.0, 4.5, 0.0, kPi * 2.0);
  setColorA(cr, kRed, 0.15);
  cairo_fill(cr);

  drawScrew(cr, x + 12.0, y + h - 12.0);
  drawScrew(cr, x + w - 12.0, y + h - 12.0);
}

static void debugLog(const char* fmt, ...);

static cairo_surface_t* gKnobImage = nullptr;
static cairo_surface_t* gMetalTexture = nullptr;
static FT_Library gFtLibrary = nullptr;
static FT_Face gLogoFace = nullptr;
static cairo_font_face_t* gLogoFont = nullptr;
static bool gLogoTried = false;

static void destroyThemeAssets() {
  if (gKnobImage) {
    cairo_surface_destroy(gKnobImage);
    gKnobImage = nullptr;
  }
  if (gMetalTexture) {
    cairo_surface_destroy(gMetalTexture);
    gMetalTexture = nullptr;
  }
}

static bool loadPng(const std::string& path, cairo_surface_t** out) {
  if (path.empty()) return false;
  cairo_surface_t* surface =
      cairo_image_surface_create_from_png(path.c_str());
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return false;
  }
  *out = surface;
  return true;
}

static std::string bundleDirOf(const char* bundlePath) {
  std::string dir(bundlePath ? bundlePath : "");
  if (!dir.empty() && dir.back() != '/') dir += '/';
  return dir;
}

static void loadThemeAssets(const char* bundlePath) {
  destroyThemeAssets();
  const auto& manager = hearth::theme::global();
  const auto& t = manager.theme();
  const std::string dir = bundleDirOf(bundlePath);
  const std::string skinDir = manager.currentSkinDir();

  if (!skinDir.empty() && !t.knob.empty()) {
    loadPng(skinDir + "/" + t.knob, &gKnobImage);
  }
  if (!gKnobImage && !t.knob.empty()) {
    loadPng(dir + t.knob, &gKnobImage);
  }

  if (t.texture_opacity > 0.0 && !t.texture.empty()) {
    if (!skinDir.empty()) {
      loadPng(skinDir + "/" + t.texture, &gMetalTexture);
    }
    if (!gMetalTexture) {
      loadPng(dir + t.texture, &gMetalTexture);
    }
  }
  debugLog("theme %s: skinDir=%s knob=%s texture=%s", t.id.c_str(),
           skinDir.c_str(), gKnobImage ? "yes" : "no",
           gMetalTexture ? "yes" : "no");
}

static void paintTexture(cairo_t* cr, double x, double y, double w, double h,
                         double alpha) {
  if (!gMetalTexture || gTextureScale <= 0.0) {
    return;
  }
  cairo_pattern_t* pattern = cairo_pattern_create_for_surface(gMetalTexture);
  cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
  cairo_rectangle(cr, x, y, w, h);
  cairo_set_source(cr, pattern);
  cairo_paint_with_alpha(cr, alpha * gTextureScale);
  cairo_pattern_destroy(pattern);
}

static void loadLogoFont(const char* bundlePath) {
  if (gLogoTried || !bundlePath || !*bundlePath) {
    return;
  }
  gLogoTried = true;
  std::string dir(bundlePath);
  if (!dir.empty() && dir.back() != '/') {
    dir += '/';
  }
  const std::string path = dir + "Jost-ExtraBold.ttf";
  if (FT_Init_FreeType(&gFtLibrary) != 0) {
    gFtLibrary = nullptr;
    return;
  }
  if (FT_New_Face(gFtLibrary, path.c_str(), 0, &gLogoFace) != 0) {
    FT_Done_FreeType(gFtLibrary);
    gFtLibrary = nullptr;
    gLogoFace = nullptr;
    return;
  }
  gLogoFont = cairo_ft_font_face_create_for_ft_face(gLogoFace, 0);
  if (cairo_font_face_status(gLogoFont) != CAIRO_STATUS_SUCCESS) {
    cairo_font_face_destroy(gLogoFont);
    gLogoFont = nullptr;
  }
  debugLog("loaded logo font %s", path.c_str());
}

static void drawWordmark(cairo_t* cr, double x, double baseline, double size) {
  if (!gLogoFont) {
    textAt(cr, x, baseline, "Ember", size, true, kCream);
    return;
  }
  cairo_save(cr);
  cairo_set_font_face(cr, gLogoFont);
  cairo_set_font_size(cr, size);
  setColor(cr, kCream);
  const char* text = "Ember";
  const double tracking = size * 0.025;
  double px = x;
  for (const char* p = text; *p; ++p) {
    char glyph[2] = {*p, 0};
    cairo_text_extents_t ext;
    cairo_text_extents(cr, glyph, &ext);
    cairo_move_to(cr, px - ext.x_bearing, baseline);
    cairo_show_text(cr, glyph);
    px += ext.x_advance - tracking;
  }
  cairo_restore(cr);
}

static void drawKnobBody(cairo_t* cr, double cx, double cy, double r,
                         bool hovered) {
  // soft contact shadow
  cairo_pattern_t* shadow = cairo_pattern_create_radial(
      cx, cy + 3.0, r * 0.15, cx, cy + 3.0, r * 1.55);
  cairo_pattern_add_color_stop_rgba(shadow, 0.0, 0.0, 0.0, 0.0, 0.55);
  cairo_pattern_add_color_stop_rgba(shadow, 1.0, 0.0, 0.0, 0.0, 0.0);
  cairo_arc(cr, cx, cy + 3.0, r * 1.55, 0.0, kPi * 2.0);
  cairo_set_source(cr, shadow);
  cairo_fill(cr);
  cairo_pattern_destroy(shadow);

  if (gKnobImage) {
    cairo_save(cr);
    cairo_translate(cr, cx - r, cy - r);
    const double scale = (2.0 * r) / 512.0;
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, gKnobImage, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    // procedural fallback
    cairo_pattern_t* pat = cairo_pattern_create_radial(
        cx - r * 0.35, cy - r * 0.40, r * 0.10, cx, cy, r * 1.15);
    cairo_pattern_add_color_stop_rgb(pat, 0.0, kKnobTop.r, kKnobTop.g,
                                     kKnobTop.b);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, kKnobBottom.r, kKnobBottom.g,
                                     kKnobBottom.b);
    cairo_arc(cr, cx, cy, r, 0.0, kPi * 2.0);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    cairo_arc(cr, cx, cy, r, 0.0, kPi * 2.0);
    setColor(cr, kKnobRing);
    cairo_set_line_width(cr, 1.1);
    cairo_stroke(cr);
  }

  // top rim light
  setColorA(cr, kHighlight, 0.10);
  cairo_set_line_width(cr, 1.2);
  cairo_arc(cr, cx, cy, r * 0.95, kPi * 1.12, kPi * 1.72);
  cairo_stroke(cr);

  if (hovered) {
    cairo_arc(cr, cx, cy, r, 0.0, kPi * 2.0);
    setColor(cr, kAmber);
    cairo_set_line_width(cr, 1.8);
    cairo_stroke(cr);
  }
}

static void drawAmountKnob(cairo_t* cr, const Widget& w, float value,
                           bool hovered, bool enabled) {
  const ParamInfo& p = infoFor(w.port);
  const float n = normOf(p, value);
  const double start = kPi * 0.75;
  const double sweep = kPi * 1.5;
  const double cx = w.cx;
  const double cy = w.cy;
  const double r = w.r;

  // Fine tick ring around the large knob.
  for (int i = 0; i <= 20; ++i) {
    const double t = i / 20.0;
    const double a = start + sweep * t;
    const bool major = (i % 5) == 0;
    setColor(cr, major ? kMuted : kPanelEdge);
    cairo_set_line_width(cr, major ? 1.5 : 0.9);
    const double r0 = r + (major ? 9.0 : 6.0);
    const double r1 = r + (major ? 19.0 : 13.0);
    cairo_move_to(cr, cx + std::cos(a) * r0, cy + std::sin(a) * r0);
    cairo_line_to(cr, cx + std::cos(a) * r1, cy + std::sin(a) * r1);
    cairo_stroke(cr);
  }

  drawKnobBody(cr, cx, cy, r, hovered);

  // Track and value arcs.
  cairo_set_line_width(cr, 3.6);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  setColorA(cr, kBg, 0.9);
  cairo_arc(cr, cx, cy, r - 6.0, start, start + sweep);
  cairo_stroke(cr);

  const double pos = start + sweep * n;
  setColorA(cr, enabled ? kAmber : kMuted, 0.42);
  cairo_set_line_width(cr, 2.6);
  cairo_arc(cr, cx, cy, r - 6.0, start, pos);
  cairo_stroke(cr);

  // Pointer.
  const double a = pos;
  setColor(cr, kCream);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, cx + std::cos(a) * (r * 0.20),
                cy + std::sin(a) * (r * 0.20));
  cairo_line_to(cr, cx + std::cos(a) * (r * 0.86),
                cy + std::sin(a) * (r * 0.86));
  cairo_stroke(cr);

  // "DRY" / "MAX" marks at the ends of the travel.
  textCentered(cr, cx + std::cos(start) * (r + 34.0),
               cy + std::sin(start) * (r + 34.0) + 14.0, "DRY", 7.0, true,
               kMuted);
  textCentered(cr, cx + std::cos(start + sweep) * (r + 34.0),
               cy + std::sin(start + sweep) * (r + 34.0) + 14.0, "MAX", 7.0,
               true, kMuted);

  textCentered(cr, cx, cy + r + 40.0, shortLabel(w.port), 10.0, true, kMuted);
  const std::string valueText = formatValue(w.port, value);
  textCentered(cr, cx, cy + r + 64.0, valueText.c_str(), 16.0, false, kText);
}

static void drawPowerButton(cairo_t* cr, const Widget& w, bool on,
                            const char* onLabel, const char* offLabel) {
  const double r = 6.0;
  roundedRect(cr, w.x + 1.0, w.y + 3.0, w.w, w.h, r);
  setColorA(cr, kBg, 0.45);
  cairo_fill(cr);

  roundedRect(cr, w.x, w.y, w.w, w.h, r);
  cairo_pattern_t* pat = cairo_pattern_create_linear(w.x, w.y, w.x,
                                                     w.y + w.h);
  if (on) {
    cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.86, 0.35, 0.30);
    cairo_pattern_add_color_stop_rgb(pat, 0.55, 0.62, 0.18, 0.15);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.34, 0.08, 0.07);
  } else {
    cairo_pattern_add_color_stop_rgb(pat, 0.0, 0.21, 0.18, 0.18);
    cairo_pattern_add_color_stop_rgb(pat, 1.0, 0.06, 0.05, 0.05);
  }
  cairo_set_source(cr, pat);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(pat);
  setColorA(cr, kBg, 0.9);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  cairo_save(cr);
  roundedRect(cr, w.x, w.y, w.w, w.h, r);
  cairo_clip(cr);
  cairo_pattern_t* gloss = cairo_pattern_create_linear(
      w.x, w.y, w.x, w.y + w.h * 0.62);
  cairo_pattern_add_color_stop_rgba(gloss, 0.0, 1.0, 1.0, 1.0, 0.22);
  cairo_pattern_add_color_stop_rgba(gloss, 1.0, 1.0, 1.0, 1.0, 0.0);
  cairo_rectangle(cr, w.x, w.y, w.w, w.h * 0.62);
  cairo_set_source(cr, gloss);
  cairo_fill(cr);
  cairo_pattern_destroy(gloss);
  cairo_restore(cr);

  setColorA(cr, kHighlight, 0.18);
  cairo_set_line_width(cr, 0.8);
  cairo_move_to(cr, w.x + 9.0, w.y + 1.0);
  cairo_line_to(cr, w.x + w.w - 9.0, w.y + 1.0);
  cairo_stroke(cr);

  const double ledX = w.x + 16.0;
  const double ledY = w.y + w.h * 0.5;
  if (on) {
    cairo_pattern_t* glow = cairo_pattern_create_radial(
        ledX, ledY, 1.0, ledX, ledY, 10.0);
    cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.35, 0.20, 0.45);
    cairo_pattern_add_color_stop_rgba(glow, 1.0, 1.0, 0.20, 0.10, 0.0);
    cairo_arc(cr, ledX, ledY, 10.0, 0.0, kPi * 2.0);
    cairo_set_source(cr, glow);
    cairo_fill(cr);
    cairo_pattern_destroy(glow);
  }
  cairo_arc(cr, ledX, ledY, 3.4, 0.0, kPi * 2.0);
  setColor(cr, on ? kAmber : kRedDark);
  cairo_fill(cr);
  cairo_arc(cr, ledX - 1.0, ledY - 1.0, 1.1, 0.0, kPi * 2.0);
  setColorA(cr, kHighlight, on ? 0.7 : 0.15);
  cairo_fill(cr);

  textCentered(cr, w.x + w.w * 0.5 + 7.0, w.y + w.h * 0.5 + 3.5,
               on ? onLabel : offLabel, 9.0, true, on ? kCream : kMuted);
}

static void drawPresetArrow(cairo_t* cr, const Rect& r, bool left,
                            bool hovered) {
  roundedRect(cr, r.x, r.y, r.w, r.h, 3.0);
  cairo_pattern_t* pat = cairo_pattern_create_linear(r.x, r.y, r.x,
                                                     r.y + r.h);
  cairo_pattern_add_color_stop_rgb(pat, 0.0, kChassisLight.r,
                                   kChassisLight.g, kChassisLight.b);
  cairo_pattern_add_color_stop_rgb(pat, 1.0, kChassisMid.r, kChassisMid.g,
                                   kChassisMid.b);
  cairo_set_source(cr, pat);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(pat);
  setColor(cr, hovered ? kAmber : kPanelEdge);
  cairo_set_line_width(cr, hovered ? 1.4 : 0.9);
  cairo_stroke(cr);

  const double cx = r.x + r.w * 0.5;
  const double cy = r.y + r.h * 0.5;
  const double d = 5.0;
  cairo_move_to(cr, cx + (left ? d : -d), cy - d);
  cairo_line_to(cr, cx + (left ? -d : d), cy);
  cairo_line_to(cr, cx + (left ? d : -d), cy + d);
  cairo_close_path(cr);
  setColor(cr, hovered ? kCream : kText);
  cairo_fill(cr);
}

static void drawPresetBar(cairo_t* cr, const State& s) {
  textAt(cr, kPresetLcd.x, 18.0, "PRESET", 5.8, true, kMuted);

  // recessed amber LCD
  roundedRect(cr, kPresetLcd.x - 2.0, kPresetLcd.y - 2.0,
              kPresetLcd.w + 4.0, kPresetLcd.h + 4.0, 4.0);
  setColor(cr, kPanelEdge);
  cairo_fill(cr);
  roundedRect(cr, kPresetLcd.x, kPresetLcd.y, kPresetLcd.w, kPresetLcd.h,
              2.5);
  cairo_pattern_t* lcd = cairo_pattern_create_linear(
      kPresetLcd.x, kPresetLcd.y, kPresetLcd.x, kPresetLcd.y + kPresetLcd.h);
  cairo_pattern_add_color_stop_rgb(lcd, 0.0, 0.075, 0.055, 0.025);
  cairo_pattern_add_color_stop_rgb(lcd, 1.0, 0.025, 0.020, 0.012);
  cairo_set_source(cr, lcd);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(lcd);
  setColor(cr, s.presetHover == 3 ? kAmber : kBg);
  cairo_set_line_width(cr, s.presetHover == 3 ? 1.2 : 1.0);
  cairo_stroke(cr);

  cairo_save(cr);
  roundedRect(cr, kPresetLcd.x, kPresetLcd.y, kPresetLcd.w, kPresetLcd.h,
              2.5);
  cairo_clip(cr);

  for (double y = kPresetLcd.y + 2.0; y < kPresetLcd.y + kPresetLcd.h;
       y += 2.0) {
    setColorA(cr, kBg, 0.18);
    cairo_rectangle(cr, kPresetLcd.x, y, kPresetLcd.w, 1.0);
    cairo_fill(cr);
  }

  cairo_pattern_t* glass = cairo_pattern_create_linear(
      kPresetLcd.x, kPresetLcd.y, kPresetLcd.x,
      kPresetLcd.y + kPresetLcd.h * 0.65);
  cairo_pattern_add_color_stop_rgba(glass, 0.0, 1.0, 1.0, 1.0, 0.10);
  cairo_pattern_add_color_stop_rgba(glass, 1.0, 1.0, 1.0, 1.0, 0.0);
  cairo_set_source(cr, glass);
  cairo_rectangle(cr, kPresetLcd.x, kPresetLcd.y, kPresetLcd.w,
                  kPresetLcd.h * 0.65);
  cairo_fill(cr);
  cairo_pattern_destroy(glass);
  cairo_restore(cr);

  const char* name =
      (s.presetIndex >= 0) ? kPresets[s.presetIndex].shortName : "INIT";
  char display[32];
  std::snprintf(display, sizeof(display), "%s%s", name,
                s.presetDirty ? "*" : "");
  textAt(cr, kPresetLcd.x + 14.0, kPresetLcd.y + 23.0, display, 10.5, true,
         kAmber, "DejaVu Sans Mono");

  char index[16];
  if (s.presetIndex >= 0) {
    std::snprintf(index, sizeof(index), "%d/%d", s.presetIndex + 1,
                  kPresetCount);
  } else {
    std::snprintf(index, sizeof(index), "--/%d", kPresetCount);
  }
  textAt(cr, kPresetLcd.x + kPresetLcd.w - 38.0,
         kPresetLcd.y + kPresetLcd.h - 5.0, index, 5.5, false, kMuted,
         "DejaVu Sans Mono");

  cairo_arc(cr, kPresetLcd.x + 7.0, kPresetLcd.y + 18.0, 2.2, 0.0,
            kPi * 2.0);
  setColor(cr, s.presetDirty ? kRed : kGreen);
  cairo_fill(cr);

  drawPresetArrow(cr, kPresetPrev, true, s.presetHover == 1);
  drawPresetArrow(cr, kPresetNext, false, s.presetHover == 2);
}

static void drawInfoToggle(cairo_t* cr, bool on, bool hovered) {
  roundedRect(cr, kInfoButton.x + 1.0, kInfoButton.y + 2.0, kInfoButton.w,
              kInfoButton.h, 4.0);
  setColorA(cr, kBg, 0.45);
  cairo_fill(cr);

  roundedRect(cr, kInfoButton.x, kInfoButton.y, kInfoButton.w,
              kInfoButton.h, 4.0);
  cairo_pattern_t* pat = cairo_pattern_create_linear(
      kInfoButton.x, kInfoButton.y, kInfoButton.x,
      kInfoButton.y + kInfoButton.h);
  cairo_pattern_add_color_stop_rgb(pat, 0.0, kChassisLight.r,
                                   kChassisLight.g, kChassisLight.b);
  cairo_pattern_add_color_stop_rgb(pat, 1.0, kChassisMid.r, kChassisMid.g,
                                   kChassisMid.b);
  cairo_set_source(cr, pat);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(pat);
  setColor(cr, hovered || on ? kAmber : kPanelEdge);
  cairo_set_line_width(cr, hovered ? 1.4 : 0.9);
  cairo_stroke(cr);

  cairo_save(cr);
  roundedRect(cr, kInfoButton.x, kInfoButton.y, kInfoButton.w,
              kInfoButton.h, 4.0);
  cairo_clip(cr);
  cairo_pattern_t* gloss = cairo_pattern_create_linear(
      kInfoButton.x, kInfoButton.y, kInfoButton.x,
      kInfoButton.y + kInfoButton.h * 0.6);
  cairo_pattern_add_color_stop_rgba(gloss, 0.0, 1.0, 1.0, 1.0, 0.18);
  cairo_pattern_add_color_stop_rgba(gloss, 1.0, 1.0, 1.0, 1.0, 0.0);
  cairo_rectangle(cr, kInfoButton.x, kInfoButton.y, kInfoButton.w,
                  kInfoButton.h * 0.6);
  cairo_set_source(cr, gloss);
  cairo_fill(cr);
  cairo_pattern_destroy(gloss);
  cairo_restore(cr);

  cairo_arc(cr, kInfoButton.x + 8.0,
            kInfoButton.y + kInfoButton.h * 0.5, 2.0, 0.0, kPi * 2.0);
  setColor(cr, on ? kAmber : kMuted);
  cairo_fill(cr);
  textCentered(cr, kInfoButton.x + 27.0,
               kInfoButton.y + kInfoButton.h * 0.5 + 2.8, "INFO", 7.2, true,
               on ? kAmber : kText);
}

static void drawSkinChip(cairo_t* cr, const State& s) {
  const Rect& r = kSkinChip;
  cairo_save(cr);
  roundedRect(cr, r.x, r.y, r.w, r.h, 5.0);
  setColorA(cr, kBg, 0.78);
  cairo_fill_preserve(cr);
  setColor(cr, s.skinHover ? kAmber : kPanelEdge);
  cairo_set_line_width(cr, s.skinHover ? 1.2 : 0.8);
  cairo_stroke(cr);

  roundedRect(cr, r.x + 2.0, r.y + 2.0, r.w - 4.0, r.h - 4.0, 4.0);
  cairo_clip(cr);
  setColor(cr, kRed);
  cairo_rectangle(cr, r.x + 2.0, r.y + 2.0, (r.w - 4.0) / 3.0, r.h - 4.0);
  cairo_fill(cr);
  setColor(cr, kAmber);
  cairo_rectangle(cr, r.x + 2.0 + (r.w - 4.0) / 3.0, r.y + 2.0,
                  (r.w - 4.0) / 3.0, r.h - 4.0);
  cairo_fill(cr);
  setColor(cr, kCream);
  cairo_rectangle(cr, r.x + 2.0 + 2.0 * (r.w - 4.0) / 3.0, r.y + 2.0,
                  (r.w - 4.0) / 3.0, r.h - 4.0);
  cairo_fill(cr);
  cairo_restore(cr);

  setColorA(cr, kHighlight, 0.45);
  cairo_set_line_width(cr, 0.7);
  cairo_move_to(cr, r.x + 4.0, r.y + r.h - 3.5);
  cairo_line_to(cr, r.x + r.w - 4.0, r.y + r.h - 3.5);
  cairo_stroke(cr);
}

static void drawSkinToast(cairo_t* cr, const State& s) {
  if (nowSeconds() >= s.skinToastUntil || s.skinToast[0] == '\0') {
    return;
  }
  const double w = 200.0;
  const double h = 20.0;
  const double x = (UI_W - w) * 0.5;
  const double y = 84.0;
  roundedRect(cr, x, y, w, h, 5.0);
  setColorA(cr, kBg, 0.94);
  cairo_fill_preserve(cr);
  setColorA(cr, kAmber, 0.9);
  cairo_set_line_width(cr, 0.8);
  cairo_stroke(cr);
  textCentered(cr, x + w * 0.5, y + 13.0, s.skinToast, 7.0, true, kCream);
}

static void drawHeader(cairo_t* cr, const State& s) {
  cairo_pattern_t* pat = cairo_pattern_create_linear(0, 0, 0, 80);
  cairo_pattern_add_color_stop_rgb(pat, 0.0, kHeaderTop.r, kHeaderTop.g,
                                   kHeaderTop.b);
  cairo_pattern_add_color_stop_rgb(pat, 1.0, kHeaderBottom.r, kHeaderBottom.g,
                                   kHeaderBottom.b);
  cairo_rectangle(cr, 0, 0, UI_W, 80);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  paintTexture(cr, 0.0, 0.0, UI_W, 80.0, 0.06);

  cairo_pattern_t* glass = cairo_pattern_create_linear(0, 0, 0, 26);
  cairo_pattern_add_color_stop_rgba(glass, 0.0, 1.0, 1.0, 1.0, 0.09);
  cairo_pattern_add_color_stop_rgba(glass, 1.0, 1.0, 1.0, 1.0, 0.0);
  cairo_rectangle(cr, 0, 0, UI_W, 26);
  cairo_set_source(cr, glass);
  cairo_fill(cr);
  cairo_pattern_destroy(glass);

  drawWordmark(cr, 22.0, 56.0, 38.0);
  textAt(cr, 24.0, 71.0, "ONE-KNOB MASTERING", 8.5, true, kMuted);
  drawPresetBar(cr, s);
  drawSkinChip(cr, s);
  textAt(cr, 430.0, 62.0, "v0.1.2", 5.5, false, kMuted);
  drawInfoToggle(cr, s.showChain, s.infoHover != 0);

  // brushed-metal strip along the bottom of the header
  cairo_pattern_t* strip = cairo_pattern_create_linear(0, 76, 0, 80);
  cairo_pattern_add_color_stop_rgb(strip, 0.0, kChassisLight.r,
                                   kChassisLight.g, kChassisLight.b);
  cairo_pattern_add_color_stop_rgb(strip, 1.0, kChassisDark.r, kChassisDark.g,
                                   kChassisDark.b);
  cairo_rectangle(cr, 0, 76, UI_W, 4);
  cairo_set_source(cr, strip);
  cairo_fill(cr);
  cairo_pattern_destroy(strip);

  setColorA(cr, kHighlight, 0.16);
  cairo_set_line_width(cr, 0.8);
  cairo_move_to(cr, 14.0, 76.0);
  cairo_line_to(cr, UI_W - 14.0, 76.0);
  cairo_stroke(cr);
  setColorA(cr, kBg, 0.65);
  cairo_move_to(cr, 14.0, 79.5);
  cairo_line_to(cr, UI_W - 14.0, 79.5);
  cairo_stroke(cr);

  setColor(cr, kRed);
  cairo_rectangle(cr, 14.0, 80.0, 24.0, 2.0);
  cairo_fill(cr);
  (void)s;
}

static void drawVu(cairo_t* cr, const Widget& w, float levelDb) {
  const double x = w.x;
  const double y = w.y;
  const double width = w.w;
  const double height = w.h;
  const double cx = x + width * 0.5;
  const double pivotY = y + height - 14.0;
  const double radius = std::min(width * 0.42, height * 0.74);

  // metal bezel
  roundedRect(cr, x - 4.0, y - 4.0, width + 8.0, height + 8.0, 7.0);
  cairo_pattern_t* bezel = cairo_pattern_create_linear(
      x - 4.0, y - 4.0, x - 4.0, y + height + 4.0);
  cairo_pattern_add_color_stop_rgb(bezel, 0.0, 0.68, 0.71, 0.73);
  cairo_pattern_add_color_stop_rgb(bezel, 0.45, 0.30, 0.34, 0.38);
  cairo_pattern_add_color_stop_rgb(bezel, 1.0, 0.09, 0.11, 0.13);
  cairo_set_source(cr, bezel);
  cairo_fill_preserve(cr);
  cairo_pattern_destroy(bezel);
  setColorA(cr, kBg, 0.85);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  roundedRect(cr, x, y, width, height, 4.0);
  setColor(cr, kMeterFace);
  cairo_fill(cr);
  roundedRect(cr, x, y, width, height, 4.0);
  setColorA(cr, kMeterInk, 0.7);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  const double start = kPi * 1.14;
  const double end = kPi * 1.86;
  const double sweep = end - start;
  cairo_set_line_width(cr, 1.0);
  setColor(cr, kMeterInk);
  cairo_arc(cr, cx, pivotY, radius, start, end);
  cairo_stroke(cr);

  const float marks[] = {-20.0f, -10.0f, -5.0f, 0.0f, 3.0f};
  const char* labels[] = {"-20", "-10", "-5", "0", "+3"};
  for (int i = 0; i < 5; ++i) {
    const float norm = (marks[i] + 20.0f) / 23.0f;
    const double a = start + sweep * norm;
    const double x0 = cx + std::cos(a) * (radius - 2.0);
    const double y0 = pivotY + std::sin(a) * (radius - 2.0);
    const double x1 = cx + std::cos(a) * (radius + 4.0);
    const double y1 = pivotY + std::sin(a) * (radius + 4.0);
    setColor(cr, kMeterInk);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_stroke(cr);

    const double lx = cx + std::cos(a) * (radius - 12.0);
    const double ly = pivotY + std::sin(a) * (radius - 12.0) + 2.5;
    textCentered(cr, lx, ly, labels[i], 7.0, false, kMeterInk);
  }

  const float norm = std::min(std::max((levelDb + 20.0f) / 23.0f, 0.0f), 1.0f);
  const double a = start + sweep * norm;
  setColor(cr, kRed);
  cairo_set_line_width(cr, 1.5);
  cairo_move_to(cr, cx, pivotY);
  cairo_line_to(cr, cx + std::cos(a) * (radius - 6.0),
                pivotY + std::sin(a) * (radius - 6.0));
  cairo_stroke(cr);
  cairo_arc(cr, cx, pivotY, 3.0, 0.0, kPi * 2.0);
  setColor(cr, kMeterInk);
  cairo_fill(cr);

  // glass reflection over the meter face
  cairo_save(cr);
  roundedRect(cr, x, y, width, height, 4.0);
  cairo_clip(cr);
  cairo_pattern_t* glass = cairo_pattern_create_linear(
      x, y, x, y + height * 0.72);
  cairo_pattern_add_color_stop_rgba(glass, 0.0, 1.0, 1.0, 1.0, 0.20);
  cairo_pattern_add_color_stop_rgba(glass, 1.0, 1.0, 1.0, 1.0, 0.0);
  cairo_rectangle(cr, x, y, width, height * 0.72);
  cairo_set_source(cr, glass);
  cairo_fill(cr);
  cairo_pattern_destroy(glass);

  cairo_move_to(cr, x - 10.0, y + height * 0.18);
  cairo_line_to(cr, x + width * 0.48, y - 20.0);
  cairo_line_to(cr, x + width * 0.70, y - 20.0);
  cairo_line_to(cr, x + 2.0, y + height * 0.40);
  cairo_close_path(cr);
  setColorA(cr, kHighlight, 0.06);
  cairo_fill(cr);
  cairo_restore(cr);

  setColorA(cr, kHighlight, 0.16);
  roundedRect(cr, x, y, width, height, 4.0);
  cairo_set_line_width(cr, 0.8);
  cairo_stroke(cr);

  textAt(cr, x + 8.0, y + 14.0, "VU", 8.0, true, kMeterInk);
  textAt(cr, x + width - 28.0, y + height - 5.0, "dBFS", 5.8, false,
         kMeterInk);

  const bool peak = levelDb > -1.0f;
  if (peak) {
    cairo_pattern_t* glow = cairo_pattern_create_radial(
        x + width - 10.0, y + 10.0, 1.0, x + width - 10.0, y + 10.0, 11.0);
    cairo_pattern_add_color_stop_rgba(glow, 0.0, 1.0, 0.20, 0.12, 0.5);
    cairo_pattern_add_color_stop_rgba(glow, 1.0, 1.0, 0.10, 0.05, 0.0);
    cairo_arc(cr, x + width - 10.0, y + 10.0, 11.0, 0.0, kPi * 2.0);
    cairo_set_source(cr, glow);
    cairo_fill(cr);
    cairo_pattern_destroy(glow);
  }
  cairo_arc(cr, x + width - 10.0, y + 10.0, 3.2, 0.0, kPi * 2.0);
  setColor(cr, peak ? kRed : kRedDark);
  cairo_fill(cr);
}

static void drawGr(cairo_t* cr, const Widget& w, float grDb) {
  roundedRect(cr, w.x - 1.5, w.y - 1.5, w.w + 3.0, w.h + 3.0, 3.5);
  setColorA(cr, kBg, 0.55);
  cairo_fill(cr);

  roundedRect(cr, w.x, w.y, w.w, w.h, 2.5);
  cairo_pattern_t* bg = cairo_pattern_create_linear(
      w.x, w.y, w.x + w.w, w.y);
  cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.055, 0.065, 0.075);
  cairo_pattern_add_color_stop_rgb(bg, 0.5, 0.015, 0.020, 0.025);
  cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.045, 0.055, 0.065);
  cairo_set_source(cr, bg);
  cairo_fill(cr);
  cairo_pattern_destroy(bg);
  setColorA(cr, kPanelEdge, 0.8);
  cairo_set_line_width(cr, 0.8);
  roundedRect(cr, w.x, w.y, w.w, w.h, 2.5);
  cairo_stroke(cr);

  const float n = std::min(std::max(-grDb / 30.0f, 0.0f), 1.0f);
  if (n > 0.0f) {
    cairo_pattern_t* red = cairo_pattern_create_linear(
        w.x, w.y, w.x + w.w, w.y);
    cairo_pattern_add_color_stop_rgb(red, 0.0, 0.38, 0.045, 0.035);
    cairo_pattern_add_color_stop_rgb(red, 0.5, 0.82, 0.16, 0.10);
    cairo_pattern_add_color_stop_rgb(red, 1.0, 0.45, 0.055, 0.040);
    cairo_rectangle(cr, w.x + 1.0, w.y + 1.0, w.w - 2.0,
                    (w.h - 2.0) * n);
    cairo_set_source(cr, red);
    cairo_fill(cr);
    cairo_pattern_destroy(red);
  }

  setColorA(cr, kHighlight, 0.16);
  cairo_rectangle(cr, w.x + 1.0, w.y + 1.0, 2.0, w.h - 2.0);
  cairo_fill(cr);

  for (int i = 1; i < 4; ++i) {
    const double y = w.y + w.h * (i / 4.0);
    setColorA(cr, kBg, 0.55);
    cairo_set_line_width(cr, 0.8);
    cairo_move_to(cr, w.x, y);
    cairo_line_to(cr, w.x + w.w, y);
    cairo_stroke(cr);
  }

  char grText[16];
  std::snprintf(grText, sizeof(grText), "%.1f", std::max(0.0f, -grDb));
  textCentered(cr, w.x + w.w * 0.5, w.y + w.h + 18.0, grText, 8.0, false,
               kText);
  textCentered(cr, w.x + w.w * 0.5, w.y + w.h + 30.0, "GLUE", 7.5, true,
               kMuted);
}

static void drawChainRow(cairo_t* cr, double y, const char* label,
                         float amount, const char* detail) {
  const double x = 30.0;
  const double barX = 148.0;
  const double barW = 258.0;
  const double barH = 8.0;
  const float n = std::min(std::max(amount, 0.0f), 1.0f);

  textAt(cr, x, y + 7.0, label, 8.2, true, kMuted);

  roundedRect(cr, barX, y, barW, barH, 4.0);
  setColor(cr, kTrack);
  cairo_fill(cr);
  setColorA(cr, kBg, 0.85);
  cairo_set_line_width(cr, 0.8);
  roundedRect(cr, barX, y, barW, barH, 4.0);
  cairo_stroke(cr);

  if (n > 0.001f) {
    cairo_pattern_t* fill = cairo_pattern_create_linear(
        barX, y, barX + barW, y);
    cairo_pattern_add_color_stop_rgb(fill, 0.0, kAmber.r, kAmber.g, kAmber.b);
    cairo_pattern_add_color_stop_rgb(fill, 0.78, 0.94, 0.55, 0.24);
    cairo_pattern_add_color_stop_rgb(fill, 1.0, kRed.r, kRed.g, kRed.b);
    roundedRect(cr, barX + 1.0, y + 1.0, (barW - 2.0) * n, barH - 2.0, 3.0);
    cairo_set_source(cr, fill);
    cairo_fill(cr);
    cairo_pattern_destroy(fill);
    setColorA(cr, kHighlight, 0.22);
    cairo_rectangle(cr, barX + 2.0, y + 2.0, (barW - 4.0) * n, 1.0);
    cairo_fill(cr);
  }

  textAt(cr, barX + barW + 12.0, y + 7.0, detail, 7.2, false, kText);
}

static void drawChassis(cairo_t* cr) {
  cairo_save(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  roundedRect(cr, 4.0, 4.0, UI_W - 8.0, UI_H - 8.0, 14.0);
  roundedRect(cr, 12.0, 12.0, UI_W - 24.0, UI_H - 24.0, 10.0);
  cairo_pattern_t* metal = cairo_pattern_create_linear(0, 0, UI_W, UI_H);
  cairo_pattern_add_color_stop_rgb(metal, 0.0, kChassisLight.r,
                                   kChassisLight.g, kChassisLight.b);
  cairo_pattern_add_color_stop_rgb(metal, 0.45, kChassisDark.r, kChassisDark.g,
                                   kChassisDark.b);
  cairo_pattern_add_color_stop_rgb(metal, 0.55, kChassisMid.r, kChassisMid.g,
                                   kChassisMid.b);
  cairo_pattern_add_color_stop_rgb(metal, 1.0, kChassisLight.r,
                                   kChassisLight.g, kChassisLight.b);
  cairo_set_source(cr, metal);
  cairo_fill(cr);
  cairo_pattern_destroy(metal);
  cairo_restore(cr);

  roundedRect(cr, 4.0, 4.0, UI_W - 8.0, UI_H - 8.0, 14.0);
  setColorA(cr, kBg, 0.9);
  cairo_set_line_width(cr, 1.2);
  cairo_stroke(cr);
  roundedRect(cr, 12.0, 12.0, UI_W - 24.0, UI_H - 24.0, 10.0);
  setColorA(cr, kBg, 0.75);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  setColorA(cr, kHighlight, 0.18);
  cairo_set_line_width(cr, 0.8);
  cairo_move_to(cr, 18.0, 5.5);
  cairo_line_to(cr, UI_W - 18.0, 5.5);
  cairo_stroke(cr);
  setColorA(cr, kBg, 0.75);
  cairo_move_to(cr, 18.0, UI_H - 5.5);
  cairo_line_to(cr, UI_W - 18.0, UI_H - 5.5);
  cairo_stroke(cr);

  drawScrew(cr, 16.0, 16.0);
  drawScrew(cr, UI_W - 16.0, 16.0);
  drawScrew(cr, 16.0, UI_H - 16.0);
  drawScrew(cr, UI_W - 16.0, UI_H - 16.0);
}

static void render(const State& s, cairo_t* cr) {
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

  cairo_pattern_t* bg = cairo_pattern_create_linear(0, 0, 0, UI_H);
  cairo_pattern_add_color_stop_rgb(bg, 0.0, kBgTop.r, kBgTop.g, kBgTop.b);
  cairo_pattern_add_color_stop_rgb(bg, 1.0, kBgBottom.r, kBgBottom.g,
                                   kBgBottom.b);
  cairo_rectangle(cr, 0, 0, UI_W, UI_H);
  cairo_set_source(cr, bg);
  cairo_fill(cr);
  cairo_pattern_destroy(bg);

  cairo_pattern_t* vignette = cairo_pattern_create_radial(
      UI_W * 0.5, UI_H * 0.45, UI_H * 0.10, UI_W * 0.5, UI_H * 0.5,
      UI_H * 0.82);
  cairo_pattern_add_color_stop_rgba(vignette, 0.0, 0.0, 0.0, 0.0, 0.0);
  cairo_pattern_add_color_stop_rgba(vignette, 1.0, 0.0, 0.0, 0.0, 0.42);
  cairo_rectangle(cr, 0, 0, UI_W, UI_H);
  cairo_set_source(cr, vignette);
  cairo_fill(cr);
  cairo_pattern_destroy(vignette);

  paintTexture(cr, 0.0, 0.0, UI_W, UI_H, 0.035);

  drawHeader(cr, s);

  drawPanel(cr, 14.0, 84.0, 442.0, s.showChain ? 478.0 : 808.0, "FINISH");
  if (s.showChain) {
    drawPanel(cr, 14.0, 570.0, 442.0, 322.0, "MASTERING CHAIN");
  }
  drawPanel(cr, 464.0, 84.0, 142.0, 808.0, "OUTPUT");

  const float amount = s.values[PORT_AMOUNT] / 100.0f;
  const MacroSettings m = macroFor(amount);
  const bool enabled = s.values[PORT_ENABLE] >= 0.5f;

  for (int i = 0; i < kNumWidgets; ++i) {
    const Widget& w = kWidgets[i];
    const bool hovered = (s.hover == i);
    switch (w.kind) {
      case Kind::Knob:
        drawAmountKnob(cr, w, s.values[w.port], hovered, enabled);
        break;
      case Kind::Toggle:
        drawPowerButton(cr, w, s.values[PORT_ENABLE] >= 0.5f, "IN", "BYPASS");
        break;
      case Kind::Vu:
        drawVu(cr, w, s.vuSmooth);
        break;
      case Kind::Meter:
        drawGr(cr, w, s.meterGrSmooth);
        break;
    }
  }

  // Small line under the big knob.  When the stage panel is hidden the
  // finish panel expands and the line moves down to keep the layout balanced.
  textCentered(cr, 235.0, s.showChain ? 530.0 : 812.0,
               "TURN TO FINISH  /  ONE KNOB, FIVE STAGES", 7.2, false, kMuted);
  if (!s.showChain) {
    textCentered(cr, 235.0, 838.0, "STAGE DETAILS HIDDEN  /  CLICK INFO",
                 6.3, false, kMuted);
  }

  if (s.showChain) {
    // Under-the-hood stage meters.
    const float low = smoothStep(0.0f, 0.90f, amount);
    const float presence = smoothStep(0.04f, 0.90f, amount);
    char detail[24];

    std::snprintf(detail, sizeof(detail), "%+.1f dB", m.lowGainDb);
    drawChainRow(cr, 626.0, "LOW END", low, detail);
    std::snprintf(detail, sizeof(detail), "%+.1f dB", m.presenceGainDb);
    drawChainRow(cr, 674.0, "PRESENCE", presence, detail);
    std::snprintf(detail, sizeof(detail), "%+.1f dB", m.airGainDb);
    drawChainRow(cr, 722.0, "AIR", m.airAmount, detail);
    std::snprintf(detail, sizeof(detail), "%d%%", static_cast<int>(m.glueAmount * 100.0f + 0.5f));
    drawChainRow(cr, 770.0, "GLUE", m.glueAmount, detail);
    std::snprintf(detail, sizeof(detail), "%d%%", static_cast<int>(m.harmonicAmount * 100.0f + 0.5f));
    drawChainRow(cr, 818.0, "HARMONICS", m.harmonicAmount, detail);

    textAt(cr, 30.0, 874.0, "EQ  /  COMPRESSION  /  HARMONIC FINISH", 6.3,
           false, kMuted);
  }

  // Output-column notes.
  textAt(cr, 478.0, 324.0, "GLUE", 6.2, true, kMuted);
  textAt(cr, 478.0, 334.0, "REDUCTION", 6.2, false, kMuted);
  textCentered(cr, 512.0, 836.0, "STEREO / ZERO LATENCY", 6.0, false, kMuted);
  textCentered(cr, 512.0, 850.0, "CLASS-A FINISH", 6.3, true, kAmber);

  drawChassis(cr);
  drawSkinToast(cr, s);
}

// ---------------------------------------------------------------------------
// Hit testing and interaction
// ---------------------------------------------------------------------------
static int hitTest(double x, double y) {
  for (int i = kNumWidgets - 1; i >= 0; --i) {
    const Widget& w = kWidgets[i];
    if (w.kind == Kind::Knob) {
      const double dx = x - w.cx;
      const double dy = y - w.cy;
      const double rr = w.r + 12.0;
      if (dx * dx + dy * dy <= rr * rr) {
        return i;
      }
    } else {
      if (x >= w.x && x <= w.x + w.w && y >= w.y && y <= w.y + w.h) {
        return i;
      }
    }
  }
  return -1;
}

struct Host {
  LV2UI_Write_Function write = nullptr;
  LV2UI_Controller controller = nullptr;
};

static void sendValue(const Host& host, uint32_t port, float value) {
  if (host.write) {
    host.write(host.controller, port, sizeof(float), 0, &value);
  }
}

static void setValue(State& s, const Host& host, uint32_t port, float value) {
  const ParamInfo& p = infoFor(port);
  value = std::min(std::max(value, std::min(p.min, p.max)),
                   std::max(p.min, p.max));
  s.values[port] = value;
  if (s.presetIndex >= 0) {
    s.presetDirty = true;
  }
  s.dirty = true;
  sendValue(host, port, value);
}

// ---------------------------------------------------------------------------
// X11 UI instance
// ---------------------------------------------------------------------------
struct X11UI {
  Display* display = nullptr;
  Window parent = 0;
  Window window = 0;
  bool topLevel = false;
  Visual* visual = nullptr;
  int depth = 24;
  cairo_surface_t* windowSurface = nullptr;
  cairo_surface_t* imageSurface = nullptr;
  cairo_t* imageCr = nullptr;
  State state;
  Host host;
  int windowW = 0;
  int windowH = 0;
  double scale = 1.0;
  double offsetX = 0.0;
  double offsetY = 0.0;
  const LV2UI_Resize* hostResize = nullptr;
  bool resizeRequested = false;
  float scaleFactor = 1.0f;
  int configPollCounter = 0;
};

static void refreshSkin(X11UI* ui) {
  if (!ui) return;
  auto& skin = hearth::theme::global();
  applyTheme(skin.theme());
  loadThemeAssets(skin.bundlePath().c_str());
  showSkinToast(ui->state, skin.theme().name.c_str());
  ui->state.dirty = true;
}

static X11UI* get(LV2UI_Handle handle) {
  return static_cast<X11UI*>(handle);
}

static void applyPreset(X11UI* ui, int index) {
  if (!ui || index < 0 || index >= kPresetCount) {
    return;
  }
  const Preset& preset = kPresets[index];
  for (int i = 0; i < preset.count; ++i) {
    setValue(ui->state, ui->host, preset.values[i].port,
             preset.values[i].value);
  }
  ui->state.presetIndex = index;
  ui->state.presetDirty = false;
  ui->state.dirty = true;
}

static void cyclePreset(X11UI* ui, int direction) {
  if (!ui) {
    return;
  }
  int index = ui->state.presetIndex;
  if (index < 0) {
    index = (direction >= 0) ? 0 : kPresetCount - 1;
  } else {
    index = (index + direction + kPresetCount) % kPresetCount;
  }
  applyPreset(ui, index);
}

static void debugLog(const char* fmt, ...) {
  if (!std::getenv("EMBER_UI_DEBUG")) {
    return;
  }
  std::FILE* f = std::fopen("/tmp/ember-ui.log", "a");
  if (!f) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  std::vfprintf(f, fmt, args);
  va_end(args);
  std::fputc('\n', f);
  std::fclose(f);
}

static void updateScale(X11UI* ui) {
  if (ui->windowW <= 0 || ui->windowH <= 0) {
    return;
  }
  ui->scale = std::min(static_cast<double>(ui->windowW) / UI_W,
                       static_cast<double>(ui->windowH) / UI_H);
  ui->scale = std::max(ui->scale, 0.1);
  ui->offsetX = (ui->windowW - UI_W * ui->scale) * 0.5;
  ui->offsetY = (ui->windowH - UI_H * ui->scale) * 0.5;
}

static void ensureSurfaces(X11UI* ui) {
  if (!ui->imageSurface) {
    ui->imageSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, UI_W, UI_H);
    ui->imageCr = cairo_create(ui->imageSurface);
  }
}

static void blit(X11UI* ui) {
  ensureSurfaces(ui);
  render(ui->state, ui->imageCr);
  cairo_surface_flush(ui->imageSurface);

  if (!ui->windowSurface) {
    ui->windowSurface = cairo_xlib_surface_create(
        ui->display, ui->window, ui->visual, ui->windowW, ui->windowH);
  }
  cairo_t* cr = cairo_create(ui->windowSurface);
  cairo_set_source_rgba(cr, kBg.r, kBg.g, kBg.b, 1.0);
  cairo_paint(cr);

  cairo_save(cr);
  cairo_translate(cr, ui->offsetX, ui->offsetY);
  cairo_scale(cr, ui->scale, ui->scale);
  cairo_set_source_surface(cr, ui->imageSurface, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_destroy(cr);
  cairo_surface_flush(ui->windowSurface);
  XFlush(ui->display);
}

static void toLogical(const X11UI* ui, int x, int y, double& lx, double& ly) {
  lx = (x - ui->offsetX) / ui->scale;
  ly = (y - ui->offsetY) / ui->scale;
}

static void handleButtonPress(X11UI* ui, XButtonEvent& ev) {
  double x = 0.0;
  double y = 0.0;
  toLogical(ui, ev.x, ev.y, x, y);

  if (ev.button == Button1) {
    if (inRect(x, y, kSkinChip)) {
      const int direction = (ev.state & ShiftMask) ? -1 : 1;
      hearth::theme::global().cycle(direction);
      refreshSkin(ui);
      return;
    }
    if (inRect(x, y, kInfoButton)) {
      ui->state.showChain = !ui->state.showChain;
      ui->state.dirty = true;
      return;
    }
    if (inRect(x, y, kPresetPrev)) {
      cyclePreset(ui, -1);
      return;
    }
    if (inRect(x, y, kPresetNext)) {
      cyclePreset(ui, 1);
      return;
    }
    if (inRect(x, y, kPresetLcd)) {
      cyclePreset(ui, 1);
      return;
    }
  }

  if (ev.button == Button4 || ev.button == Button5) {
    const int idx = hitTest(x, y);
    if (idx >= 0 && kWidgets[idx].kind == Kind::Knob) {
      const Widget& w = kWidgets[idx];
      const ParamInfo& p = infoFor(w.port);
      float n = normOf(p, ui->state.values[w.port]);
      n += (ev.button == Button4) ? 0.02f : -0.02f;
      setValue(ui->state, ui->host, w.port, valueOf(p, n));
    }
    return;
  }
  if (ev.button != Button1) {
    return;
  }

  const int idx = hitTest(x, y);
  if (idx < 0) {
    return;
  }
  const Widget& w = kWidgets[idx];
  ui->state.active = idx;

  if (w.kind == Kind::Toggle) {
    const float v = ui->state.values[w.port] >= 0.5f ? 0.0f : 1.0f;
    setValue(ui->state, ui->host, w.port, v);
    ui->state.active = -1;
    return;
  }

  if (w.kind == Kind::Vu || w.kind == Kind::Meter) {
    ui->state.active = -1;
    return;
  }

  if (ui->state.lastClickWidget == idx &&
      ev.time - ui->state.lastClickTime < 400) {
    setValue(ui->state, ui->host, w.port, infoFor(w.port).def);
    ui->state.lastClickWidget = -1;
    ui->state.active = -1;
    return;
  }
  ui->state.lastClickWidget = idx;
  ui->state.lastClickTime = ev.time;
  ui->state.dragY = y;
  ui->state.dragValue = ui->state.values[w.port];
}

static void handleMotion(X11UI* ui, XMotionEvent& ev) {
  double x = 0.0;
  double y = 0.0;
  toLogical(ui, ev.x, ev.y, x, y);

  const bool skinHover = inRect(x, y, kSkinChip);
  if (skinHover != ui->state.skinHover) {
    ui->state.skinHover = skinHover;
    ui->state.dirty = true;
  }

  int presetHover = 0;
  if (inRect(x, y, kPresetPrev)) {
    presetHover = 1;
  } else if (inRect(x, y, kPresetNext)) {
    presetHover = 2;
  } else if (inRect(x, y, kPresetLcd)) {
    presetHover = 3;
  }
  if (skinHover) {
    presetHover = 0;
  }
  if (presetHover != ui->state.presetHover) {
    ui->state.presetHover = presetHover;
    ui->state.dirty = true;
  }

  const int infoHover = inRect(x, y, kInfoButton) ? 1 : 0;
  if (infoHover != ui->state.infoHover) {
    ui->state.infoHover = infoHover;
    ui->state.dirty = true;
  }

  const int hover = (presetHover || infoHover || skinHover) ? -1 : hitTest(x, y);
  if (hover != ui->state.hover) {
    ui->state.hover = hover;
    ui->state.dirty = true;
  }
  if (ui->state.active < 0) {
    return;
  }
  const Widget& w = kWidgets[ui->state.active];
  if (w.kind != Kind::Knob) {
    return;
  }

  const ParamInfo& p = infoFor(w.port);
  const double sensitivity = (ev.state & ShiftMask) ? 1500.0 : 260.0;
  const double dy = (ui->state.dragY - y) / sensitivity;
  const float startNorm = normOf(p, ui->state.dragValue);
  setValue(ui->state, ui->host, w.port, valueOf(p, startNorm + dy));
}

static void handleButtonRelease(X11UI* ui) {
  ui->state.active = -1;
}

static void processEvents(X11UI* ui) {
  while (XPending(ui->display)) {
    XEvent ev;
    XNextEvent(ui->display, &ev);
    switch (ev.type) {
      case Expose:
        if (ev.xexpose.count == 0) {
          ui->state.dirty = true;
        }
        break;
      case ConfigureNotify:
        if (ev.xconfigure.window == ui->window &&
            (ev.xconfigure.width != ui->windowW ||
             ev.xconfigure.height != ui->windowH)) {
          ui->windowW = std::max(ev.xconfigure.width, 160);
          ui->windowH = std::max(ev.xconfigure.height, 240);
          if (ui->windowSurface) {
            cairo_xlib_surface_set_size(ui->windowSurface, ui->windowW,
                                        ui->windowH);
          }
          updateScale(ui);
          ui->state.dirty = true;
        }
        break;
      case ButtonPress:
        handleButtonPress(ui, ev.xbutton);
        break;
      case MotionNotify:
        handleMotion(ui, ev.xmotion);
        break;
      case ButtonRelease:
        handleButtonRelease(ui);
        break;
      case LeaveNotify:
        if (ui->state.hover != -1 || ui->state.presetHover != 0 ||
            ui->state.infoHover != 0 || ui->state.skinHover) {
          ui->state.hover = -1;
          ui->state.presetHover = 0;
          ui->state.infoHover = 0;
          ui->state.skinHover = false;
          ui->state.dirty = true;
        }
        break;
      default:
        break;
    }
  }
}

static int idle(LV2UI_Handle handle) {
  X11UI* ui = get(handle);
  if (!ui) {
    return 1;
  }
  processEvents(ui);

  if (++ui->configPollCounter >= 30) {
    ui->configPollCounter = 0;
    if (hearth::theme::global().reloadIfConfigChanged()) {
      refreshSkin(ui);
    }
  }

  if (ui->state.skinToast[0] != '\0' &&
      nowSeconds() >= ui->state.skinToastUntil) {
    ui->state.skinToast[0] = '\0';
    ui->state.dirty = true;
  }

  const float prevGr = ui->state.meterGrSmooth;
  const float prevOut = ui->state.vuSmooth;
  ui->state.meterGrSmooth +=
      0.28f * (ui->state.meterGr - ui->state.meterGrSmooth);
  // A slower fall for the VU movement.
  if (ui->state.meterOut > ui->state.vuSmooth) {
    ui->state.vuSmooth += 0.55f * (ui->state.meterOut - ui->state.vuSmooth);
  } else {
    ui->state.vuSmooth += 0.10f * (ui->state.meterOut - ui->state.vuSmooth);
  }
  if (std::fabs(prevGr - ui->state.meterGrSmooth) > 0.05f ||
      std::fabs(prevOut - ui->state.vuSmooth) > 0.05f) {
    ui->state.dirty = true;
  }

  if (!ui->resizeRequested && ui->hostResize && ui->hostResize->ui_resize) {
    ui->resizeRequested = true;
    ui->hostResize->ui_resize(ui->hostResize->handle, ui->windowW, ui->windowH);
  }

  if (ui->state.dirty) {
    blit(ui);
    ui->state.dirty = false;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// LV2 UI entry points
// ---------------------------------------------------------------------------
static int ignoreXError(Display*, XErrorEvent*) { return 0; }

static bool isValidWindow(Display* display, Window window) {
  if (!window) {
    return false;
  }
  XErrorHandler old = XSetErrorHandler(ignoreXError);
  XWindowAttributes attributes;
  const Status status = XGetWindowAttributes(display, window, &attributes);
  XSync(display, False);
  XSetErrorHandler(old);
  return status != 0;
}

static float readScaleFactor(const LV2_Feature* const* features) {
  const LV2_URID_Map* map = nullptr;
  const LV2_Options_Option* options = nullptr;
  for (const LV2_Feature* const* f = features; f && *f; ++f) {
    if (!std::strcmp((*f)->URI, LV2_URID__map) && (*f)->data) {
      map = static_cast<const LV2_URID_Map*>(
          static_cast<const void*>((*f)->data));
    } else if (!std::strcmp((*f)->URI, LV2_OPTIONS__options) && (*f)->data) {
      options = static_cast<const LV2_Options_Option*>(
          static_cast<const void*>((*f)->data));
    }
  }
  if (!map || !options) {
    return 1.0f;
  }
  const LV2_URID key = map->map(map->handle, LV2_UI__scaleFactor);
  const LV2_URID tFloat = map->map(map->handle, LV2_ATOM__Float);
  const LV2_URID tDouble = map->map(map->handle, LV2_ATOM__Double);
  for (const LV2_Options_Option* o = options; o->key; ++o) {
    if (o->key != key || !o->value) {
      continue;
    }
    if (o->type == tFloat && o->size >= sizeof(float)) {
      return *static_cast<const float*>(o->value);
    }
    if (o->type == tDouble && o->size >= sizeof(double)) {
      return static_cast<float>(*static_cast<const double*>(o->value));
    }
  }
  return 1.0f;
}

static void chooseWindowSize(Display* display, float scaleFactor, int& width,
                             int& height) {
  const int screen = DefaultScreen(display);
  const double screenW = DisplayWidth(display, screen);
  const double screenH = DisplayHeight(display, screen);
  double sf = std::min(std::max(static_cast<double>(scaleFactor), 1.0), 2.5);
  double w = UI_W * sf;
  double h = UI_H * sf;
  const double maxW = screenW * 0.92;
  const double maxH = screenH * 0.86;
  const double fit = std::min(1.0, std::min(maxW / w, maxH / h));
  w *= fit;
  h *= fit;
  width = std::max(static_cast<int>(std::lround(w)), 320);
  height = std::max(static_cast<int>(std::lround(h)), 464);
}

static LV2UI_Handle uiInstantiate(const LV2UI_Descriptor*, const char*,
                                  const char* bundlePath,
                                  LV2UI_Write_Function write,
                                  LV2UI_Controller controller,
                                  LV2UI_Widget* widget,
                                  const LV2_Feature* const* features) {
  auto& skin = hearth::theme::global();
  skin.init(bundlePath ? bundlePath : "", "ember");
  applyTheme(skin.theme());
  loadThemeAssets(bundlePath);
  loadLogoFont(bundlePath);
  X11UI* ui = new X11UI();
  initState(ui->state);
  ui->host.write = write;
  ui->host.controller = controller;

  const void* parentData = nullptr;
  for (const LV2_Feature* const* f = features; f && *f; ++f) {
    if (!std::strcmp((*f)->URI, LV2_UI__parent) && (*f)->data) {
      parentData = (*f)->data;
    } else if (!std::strcmp((*f)->URI, LV2_UI__resize) && (*f)->data) {
      ui->hostResize = static_cast<const LV2UI_Resize*>(
          static_cast<const void*>((*f)->data));
    }
  }

  ui->display = XOpenDisplay(nullptr);
  if (!ui->display) {
    debugLog("instantiate: XOpenDisplay failed");
    delete ui;
    return nullptr;
  }

  ui->scaleFactor = readScaleFactor(features);
  debugLog("instantiate: features parentData=%p scale=%f", parentData,
           static_cast<double>(ui->scaleFactor));

  Window parent = 0;
  if (parentData) {
    const uintptr_t raw = reinterpret_cast<uintptr_t>(parentData);
    if (raw != 0 && raw <= 0xffffffffu &&
        isValidWindow(ui->display, static_cast<Window>(raw))) {
      parent = static_cast<Window>(raw);
    } else {
      parent = *static_cast<const Window*>(parentData);
    }
  }

  if (!isValidWindow(ui->display, parent)) {
    // Non-embedded fallback: show a top-level window rather than failing.
    parent = DefaultRootWindow(ui->display);
    ui->topLevel = true;
    debugLog("instantiate: no valid parent, using top-level window");
  }

  XWindowAttributes parentAttributes;
  if (!XGetWindowAttributes(ui->display, parent, &parentAttributes)) {
    debugLog("instantiate: XGetWindowAttributes(parent) failed");
    XCloseDisplay(ui->display);
    delete ui;
    return nullptr;
  }
  ui->visual = parentAttributes.visual;
  ui->depth = parentAttributes.depth;

  int width = 0;
  int height = 0;
  chooseWindowSize(ui->display, ui->scaleFactor, width, height);
  ui->windowW = width;
  ui->windowH = height;
  updateScale(ui);

  XSetWindowAttributes attributes;
  std::memset(&attributes, 0, sizeof(attributes));
  attributes.background_pixel = 0x0b0d10;
  attributes.border_pixel = 0;
  attributes.event_mask = ExposureMask | StructureNotifyMask |
                          ButtonPressMask | ButtonReleaseMask |
                          PointerMotionMask | LeaveWindowMask;
  attributes.colormap = parentAttributes.colormap
                            ? parentAttributes.colormap
                            : DefaultColormap(ui->display,
                                              DefaultScreen(ui->display));
  ui->parent = parent;
  ui->window = XCreateWindow(
      ui->display, parent, 0, 0, width, height, 0, parentAttributes.depth,
      InputOutput, parentAttributes.visual,
      CWBackPixel | CWBorderPixel | CWEventMask | CWColormap, &attributes);
  debugLog("instantiate: parent=0x%lx window=0x%lx size=%dx%d depth=%d",
           static_cast<unsigned long>(parent),
           static_cast<unsigned long>(ui->window), width, height,
           parentAttributes.depth);
  if (!ui->window) {
    XCloseDisplay(ui->display);
    delete ui;
    return nullptr;
  }

  if (ui->topLevel) {
    XStoreName(ui->display, ui->window, "Ember");
  }
  XMapRaised(ui->display, ui->window);
  XFlush(ui->display);

  ensureSurfaces(ui);
  blit(ui);
  *widget = reinterpret_cast<LV2UI_Widget>(static_cast<uintptr_t>(ui->window));

  if (ui->hostResize && ui->hostResize->ui_resize) {
    ui->resizeRequested = true;
    ui->hostResize->ui_resize(ui->hostResize->handle, width, height);
  }
  debugLog("instantiate: done");
  return ui;
}

static void uiCleanup(LV2UI_Handle handle) {
  X11UI* ui = get(handle);
  if (!ui) {
    return;
  }
  if (ui->imageCr) cairo_destroy(ui->imageCr);
  if (ui->imageSurface) cairo_surface_destroy(ui->imageSurface);
  if (ui->windowSurface) cairo_surface_destroy(ui->windowSurface);
  if (ui->display) {
    if (ui->window) XDestroyWindow(ui->display, ui->window);
    XCloseDisplay(ui->display);
  }
  delete ui;
}

static void uiPortEvent(LV2UI_Handle handle, uint32_t portIndex,
                        uint32_t bufferSize, uint32_t format,
                        const void* buffer) {
  X11UI* ui = get(handle);
  if (!ui || !buffer || bufferSize != sizeof(float) || format != 0) {
    return;
  }
  if (portIndex >= PORT_COUNT) {
    return;
  }
  const float value = *static_cast<const float*>(buffer);
  if (portIndex == PORT_METER_GR) {
    ui->state.meterGr = value;
  } else if (portIndex == PORT_METER_OUT) {
    ui->state.meterOut = value;
  } else {
    if (ui->state.presetIndex >= 0 &&
        std::fabs(value - ui->state.values[portIndex]) > 1.0e-6f) {
      ui->state.presetDirty = true;
    }
    ui->state.values[portIndex] = value;
    ui->state.dirty = true;
  }
}

static int uiResize(LV2UI_Handle handle, int width, int height) {
  X11UI* ui = get(handle);
  if (!ui) {
    return 1;
  }
  width = std::max(width, 160);
  height = std::max(height, 240);
  ui->windowW = width;
  ui->windowH = height;
  updateScale(ui);
  if (ui->window) {
    XResizeWindow(ui->display, ui->window, width, height);
  }
  if (ui->windowSurface) {
    cairo_xlib_surface_set_size(ui->windowSurface, width, height);
  }
  debugLog("resize: %dx%d", width, height);
  ui->state.dirty = true;
  return 0;
}

static const LV2UI_Idle_Interface kIdleInterface = {idle};
static const LV2UI_Resize kResizeInterface = {nullptr, uiResize};

static const void* uiExtensionData(const char* uri) {
  if (!std::strcmp(uri, LV2_UI__idleInterface)) {
    return &kIdleInterface;
  }
  if (!std::strcmp(uri, LV2_UI__resize)) {
    return &kResizeInterface;
  }
  return nullptr;
}

} // namespace ui
} // namespace ember

static const LV2UI_Descriptor kDescriptor = {
    "https://github.com/tylorismusic-commits/ember#ui",
    &ember::ui::uiInstantiate,
    &ember::ui::uiCleanup,
    &ember::ui::uiPortEvent,
    &ember::ui::uiExtensionData,
};

extern "C" LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
  return index == 0 ? &kDescriptor : nullptr;
}

// ---------------------------------------------------------------------------
// Standalone preview (build with -DEMBER_UI_PREVIEW)
// ---------------------------------------------------------------------------
#ifdef EMBER_UI_PREVIEW
int main(int argc, char** argv) {
  using namespace ember;
  using namespace ember::ui;
  auto& skin = hearth::theme::global();
  skin.init("assets/", "ember");
  applyTheme(skin.theme());
  loadThemeAssets("assets/");
  loadLogoFont("assets/fonts/");
  State state;
  initState(state);
  // Show the mix-bus preset in the documentation screenshot.
  state.values[PORT_AMOUNT] = 48.0f;
  state.values[PORT_ENABLE] = 1.0f;
  state.presetIndex = 2;
  state.presetDirty = false;
  state.meterGrSmooth = -2.5f;
  state.vuSmooth = -9.0f;

  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, UI_W, UI_H);
  cairo_t* cr = cairo_create(surface);
  render(state, cr);
  cairo_destroy(cr);
  const char* path = (argc > 1) ? argv[1] : "ember-ui.png";
  cairo_surface_write_to_png(surface, path);
  cairo_surface_destroy(surface);
  std::printf("wrote %s\n", path);
  return 0;
}
#endif
