// Ember — lv2.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#include <lv2/core/lv2.h>

#include "ember_strip.hpp"
#include "inline_display.hpp"
#include "param_info.hpp"
#include "params.hpp"

namespace ember {

class EmberInstance {
public:
  float* ports[PORT_COUNT] = {};
  float sampleRate = 48000.0f;
  EmberStrip strip;
  Params params;
  InlineDisplay display;
  const InlineDisplayQueueDraw* queueDraw = nullptr;
  uint32_t drawCounter = 0;
};

static const ParamInfo& infoFor(uint32_t index) noexcept {
  for (int i = 0; i < kParamInfoCount; ++i) {
    if (kParams[i].index == index) {
      return kParams[i];
    }
  }
  static ParamInfo fallback;
  fallback.index = index;
  return fallback;
}

static float ctrlValue(float* const* ports, uint32_t index, float fallback) noexcept {
  const ParamInfo& info = infoFor(index);
  float v = fallback;
  if (index < PORT_COUNT && ports[index]) {
    v = ports[index][0];
  }
  if (!std::isfinite(v)) {
    v = fallback;
  }
  const float lo = std::min(info.min, info.max);
  const float hi = std::max(info.min, info.max);
  if (hi > lo) {
    v = std::min(std::max(v, lo), hi);
  }
  return v;
}

static void readParams(EmberInstance* self) noexcept {
  self->params.amount = ctrlValue(self->ports, PORT_AMOUNT, 0.0f) / 100.0f;
  self->params.enabled = ctrlValue(self->ports, PORT_ENABLE, 1.0f) > 0.5f;
}

} // namespace ember

using namespace ember;

extern "C" LV2_Handle lv2_ember_instantiate(const LV2_Descriptor* descriptor,
                                          double rate, const char* bundle_path,
                                          const LV2_Feature* const* features) {
  (void)descriptor;
  (void)bundle_path;

  ember::EmberInstance* self = new (std::nothrow) ember::EmberInstance();
  if (!self) {
    return nullptr;
  }
  for (const LV2_Feature* const* f = features; f && *f; ++f) {
    if (!std::strcmp((*f)->URI, ember::kInlineDisplayQueueDrawURI) &&
        (*f)->data) {
      self->queueDraw = static_cast<const ember::InlineDisplayQueueDraw*>(
          static_cast<const void*>((*f)->data));
    }
  }
  self->sampleRate = std::max(8000.0f, static_cast<float>(rate));
  self->strip.prepare(self->sampleRate);

  ember::readParams(self);
  self->strip.setTargets(self->params);
  self->strip.reset();
  self->display.setGainReductionDb(0.0f);
  return self;
}

extern "C" void lv2_ember_connect_port(LV2_Handle handle, uint32_t port,
                                     void* data_location) {
  ember::EmberInstance* self = static_cast<ember::EmberInstance*>(handle);
  if (!self) {
    return;
  }
  if (port < ember::PORT_COUNT) {
    self->ports[port] = static_cast<float*>(data_location);
  }
}

extern "C" void lv2_ember_activate(LV2_Handle handle) {
  ember::EmberInstance* self = static_cast<ember::EmberInstance*>(handle);
  if (!self) {
    return;
  }
  ember::readParams(self);
  self->strip.setTargets(self->params);
  self->strip.reset();
  self->display.setGainReductionDb(0.0f);
}

extern "C" void lv2_ember_run(LV2_Handle handle, uint32_t sampleCount) {
  ember::EmberInstance* self = static_cast<ember::EmberInstance*>(handle);
  if (!self) {
    return;
  }

  const bool audioValid = self->ports[PORT_AUDIO_IN_L] && self->ports[PORT_AUDIO_IN_R] &&
                           self->ports[PORT_AUDIO_OUT_L] &&
                           self->ports[PORT_AUDIO_OUT_R];

  float grDb = 0.0f;
  float outDb = -60.0f;
  if (audioValid) {
    ember::readParams(self);
    self->strip.setTargets(self->params);
    self->strip.process(self->ports[PORT_AUDIO_IN_L], self->ports[PORT_AUDIO_IN_R],
                        self->ports[PORT_AUDIO_OUT_L], self->ports[PORT_AUDIO_OUT_R],
                        sampleCount, grDb, outDb);
  }

  float* meterGr = self->ports[PORT_METER_GR];
  float* meterOut = self->ports[PORT_METER_OUT];
  if (meterGr) {
    *meterGr = grDb;
  }
  if (meterOut) {
    *meterOut = outDb;
  }

  self->display.setGainReductionDb(std::max(0.0f, -grDb));
  if (self->queueDraw && self->queueDraw->queue_draw &&
      ((self->drawCounter++ & 31u) == 0u)) {
    self->queueDraw->queue_draw(self->queueDraw->handle);
  }
}

extern "C" void lv2_ember_deactivate(LV2_Handle handle) {
  ember::EmberInstance* self = static_cast<ember::EmberInstance*>(handle);
  if (!self) {
    return;
  }
  self->strip.reset();
  self->display.setGainReductionDb(0.0f);
}

extern "C" void lv2_ember_cleanup(LV2_Handle handle) {
  delete static_cast<ember::EmberInstance*>(handle);
}

static ember::InlineDisplaySurface* renderInlineDisplay(void* instance,
                                                        uint32_t width,
                                                        uint32_t height) {
  ember::EmberInstance* self =
      static_cast<ember::EmberInstance*>(instance);
  return self ? self->display.render(width, height) : nullptr;
}

static const ember::InlineDisplayInterface emberDisplayInterface = {
    &renderInlineDisplay};

extern "C" const void* lv2_ember_extension_data(const char* uri) {
  if (!std::strcmp(uri, ember::kInlineDisplayInterfaceURI)) {
    return &emberDisplayInterface;
  }
  return nullptr;
}

static const LV2_Descriptor ember_descriptor = {
    "https://github.com/tylorismusic-commits/ember",
    &lv2_ember_instantiate,
    &lv2_ember_connect_port,
    &lv2_ember_activate,
    &lv2_ember_run,
    &lv2_ember_deactivate,
    &lv2_ember_cleanup,
    &lv2_ember_extension_data,
};

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  if (index == 0) {
    return &ember_descriptor;
  }
  return nullptr;
}
