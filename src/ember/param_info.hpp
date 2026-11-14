// Ember — param_info.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ember {

enum class Unit {
  Unitless = 0,
  Db = 1,
  Hz = 2,
  Ms = 3,
  Percent = 4,
};

// Control ports, in ascending port index order, interleaved with the stereo
// audio ports as defined in the plugin RDF.
enum PortIndex : uint32_t {
  PORT_AUDIO_IN_L = 0,
  PORT_AUDIO_IN_R = 1,
  PORT_AUDIO_OUT_L = 2,
  PORT_AUDIO_OUT_R = 3,
  PORT_AMOUNT = 4,
  PORT_ENABLE = 5,
  PORT_METER_GR = 6,
  PORT_METER_OUT = 7,
  PORT_COUNT = 8
};

inline constexpr uint32_t kEmber_PortCount = PORT_COUNT;

enum ParamGroup : uint32_t {
  GROUP_FINISH = 0,
  GROUP_GLOBAL = 1,
};

struct ScalePoint {
  float value = 0.0f;
  const char* label = "";
};

struct ParamInfo {
  uint32_t index = 0;
  const char* symbol = "";
  const char* name = "";
  float min = 0.0f;
  float max = 1.0f;
  float def = 0.0f;
  Unit unit = Unit::Unitless;
  bool logarithmic = false;
  bool toggle = false;
  bool enumeration = false;
  int group = GROUP_FINISH;
  const ScalePoint* points = nullptr;
  int pointCount = 0;
};

inline constexpr ParamInfo kParams[] = {
    // Audio ports (indices 0-3) are handled in the LV2 wrapper; the table
    // below lists the control ports only.
    {4, "amount", "Amount", 0.0f, 100.0f, 0.0f, Unit::Percent, false, false,
     false, GROUP_FINISH, nullptr, 0},
    {5, "enable", "Enabled", 0.0f, 1.0f, 1.0f, Unit::Unitless, false, true,
     false, GROUP_GLOBAL, nullptr, 0},
};

inline constexpr int kParamInfoCount =
    static_cast<int>(sizeof(kParams) / sizeof(kParams[0]));

// The two meter ports are control outputs used by the host for display.
inline constexpr uint32_t kEmber_MeterGrPort = PORT_METER_GR;
inline constexpr uint32_t kEmber_MeterOutPort = PORT_METER_OUT;

} // namespace ember
