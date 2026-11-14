// Ember — lilv host integration test
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdio>

#include <lilv/lilv.h>

#include "inline_display.hpp"
#include "param_info.hpp"

using namespace ember;

static int g_failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    printf("FAIL: %s\n", what);
  }
}

int main(int argc, char** argv) {
  const char* bundleDir = (argc > 1) ? argv[1] : nullptr;
  if (!bundleDir) {
    printf("usage: %s <bundle-dir>\n", argv[0]);
    return 2;
  }

  LilvWorld* world = lilv_world_new();
  LilvNode* pathNode = lilv_new_string(world, bundleDir);
  lilv_world_set_option(world, LILV_OPTION_LV2_PATH, pathNode);
  lilv_node_free(pathNode);
  lilv_world_load_all(world);

  LilvNode* uri = lilv_new_uri(world, "https://github.com/tylorismusic-commits/ember");
  const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
  const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uri);
  lilv_node_free(uri);
  check(plugin != nullptr, "plugin found in bundle");
  if (!plugin) {
    lilv_world_free(world);
    return 1;
  }

  const uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
  check(numPorts == PORT_COUNT, "port count matches RDF");

  LilvUIs* uis = lilv_plugin_get_uis(plugin);
  check(lilv_uis_size(uis) >= 1, "at least one UI declared");
  lilv_uis_free(uis);

  LilvNode* presetClass =
      lilv_new_uri(world, "http://lv2plug.in/ns/ext/presets#Preset");
  LilvNodes* presets = lilv_plugin_get_related(plugin, presetClass);
  check(lilv_nodes_size(presets) >= 4, "factory presets found");
  lilv_nodes_free(presets);
  lilv_node_free(presetClass);

  LilvInstance* instance = lilv_plugin_instantiate(plugin, 48000.0, nullptr);
  check(instance != nullptr, "plugin instantiated");
  if (!instance) {
    lilv_world_free(world);
    return 1;
  }

  const uint32_t block = 256;
  float audioInL[block];
  float audioInR[block];
  float audioOutL[block];
  float audioOutR[block];
  float ctrl[PORT_COUNT] = {};

  for (uint32_t i = 0; i < PORT_COUNT; ++i) {
    if (i < 4) {
      void* ptr = (i == 0) ? static_cast<void*>(audioInL)
                  : (i == 1) ? static_cast<void*>(audioInR)
                  : (i == 2) ? static_cast<void*>(audioOutL)
                             : static_cast<void*>(audioOutR);
      lilv_instance_connect_port(instance, i, ptr);
    } else {
      lilv_instance_connect_port(instance, i, &ctrl[i]);
    }
  }

  ctrl[PORT_AMOUNT] = 0.0f;
  ctrl[PORT_ENABLE] = 1.0f;

  lilv_instance_activate(instance);

  // Amount 0 passes the signal through untouched.
  for (uint32_t b = 0; b < 20; ++b) {
    for (uint32_t i = 0; i < block; ++i) {
      const float x = std::sin(2.0f * 3.14159265358979f * 1000.0f *
                               (b * block + i) / 48000.0f) * 0.10f;
      audioInL[i] = x;
      audioInR[i] = x;
    }
    lilv_instance_run(instance, block);
  }
  float maxError = 0.0f;
  for (uint32_t i = 0; i < block; ++i) {
    maxError = std::max(maxError, std::fabs(audioOutL[i] - audioInL[i]));
  }
  check(maxError < 1.0e-6f, "host amount 0 is transparent");
  check(std::isfinite(ctrl[PORT_METER_OUT]), "output meter finite");

  // The inline display extension must be available for Ardour's mixer strip.
  {
    const auto* iface = static_cast<const InlineDisplayInterface*>(
        lilv_instance_get_extension_data(instance, kInlineDisplayInterfaceURI));
    check(iface && iface->render, "inline display extension available");
    if (iface && iface->render) {
      auto* surface = iface->render(lilv_instance_get_handle(instance), 320, 40);
      check(surface && surface->width == 320 && surface->height == 40,
            "inline display renders a surface");
    }
  }

  // Amount 80 must change the sound and engage the compressor.
  ctrl[PORT_AMOUNT] = 80.0f;
  for (uint32_t b = 0; b < 80; ++b) {
    for (uint32_t i = 0; i < block; ++i) {
      const float x = std::sin(2.0f * 3.14159265358979f * 1000.0f *
                               (b * block + i) / 48000.0f) * 0.5f;
      audioInL[i] = x;
      audioInR[i] = x;
    }
    lilv_instance_run(instance, block);
  }
  float difference = 0.0f;
  for (uint32_t i = 0; i < block; ++i) {
    difference = std::max(difference, std::fabs(audioOutL[i] - audioInL[i]));
  }
  check(difference > 0.01f, "host amount changes the sound");
  check(ctrl[PORT_METER_GR] < -0.5f, "host reports gain reduction");

  // Bypass must pass the dry signal through even at a high amount.
  ctrl[PORT_ENABLE] = 0.0f;
  for (uint32_t b = 0; b < 40; ++b) {
    for (uint32_t i = 0; i < block; ++i) {
      audioInL[i] = 0.05f;
      audioInR[i] = 0.05f;
    }
    lilv_instance_run(instance, block);
  }
  check(std::fabs(audioOutL[block - 1] - 0.05f) < 2.0e-4f,
        "host bypass passes dry signal");

  lilv_instance_deactivate(instance);
  lilv_instance_free(instance);
  lilv_world_free(world);

  if (g_failures == 0) {
    printf("ALL HOST TESTS PASSED\n");
  }
  return g_failures == 0 ? 0 : 1;
}
