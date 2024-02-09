/* Copyright (C) 2022-2024 Advanced Micro Devices, Inc. - All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// This file contains helper structures used by the AIE event trace config
// PS kernel.

#ifndef EVENT_CONFIGURATION_DOT_H
#define EVENT_CONFIGURATION_DOT_H

#include <memory>
#include <vector>

#include "xaiefal/xaiefal.hpp"
#include "xaiengine.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_util.h"
#include "xdp/profile/plugin/aie_trace/x86/aie_trace_kernel_config.h"

// This struct encapsulates all of the internal configuration information
// for a single AIE tile
struct EventConfiguration {
  XAie_Events coreTraceStartEvent = XAIE_EVENT_ACTIVE_CORE;
  XAie_Events coreTraceEndEvent = XAIE_EVENT_DISABLED_CORE;

  /*
   * This is needed because the cores are started/stopped during execution
   * to get around some hw bugs. We cannot restart tracemodules when that happens.
   * At the end, we need to use event generate register to create this event
   * to gracefully shut down trace modules.
   */
  XAie_Events traceFlushEndEvent = XAIE_EVENT_INSTR_EVENT_1_CORE;
  
  // Trace for memory and interface tiles is always on
  XAie_Events memTileTraceStartEvent = XAIE_EVENT_TRUE_MEM_TILE;
  XAie_Events memTileTraceEndEvent = XAIE_EVENT_NONE_MEM_TILE;
  XAie_Events interfaceTileTraceStartEvent = XAIE_EVENT_TRUE_PL;
  XAie_Events interfaceTileTraceEndEvent = XAIE_EVENT_USER_EVENT_1_PL;

  std::map<std::string, std::vector<XAie_Events>> coreEventSets;
  std::map<std::string, std::vector<XAie_Events>> memoryEventSets;
  std::map<std::string, std::vector<XAie_Events>> memoryTileEventSets;
  std::map<std::string, std::vector<XAie_Events>> interfaceTileEventSets;

  std::vector<XAie_Events> coreCounterStartEvents;
  std::vector<XAie_Events> coreCounterEndEvents;
  std::vector<uint32_t> coreCounterEventValues;
  std::vector<XAie_Events> memoryCounterStartEvents;
  std::vector<XAie_Events> memoryCounterEndEvents;
  std::vector<uint32_t> memoryCounterEventValues;

  std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> coreCounters;
  std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> memoryCounters;

  void initialize(const xdp::built_in::TraceInputConfiguration* params)
  {
    auto hwGen = params->hwGen;
    std::string counterScheme = "es2";

    // Pre-defined metric sets
    coreEventSets = aie::trace::getCoreEventSets(hwGen);
    memoryEventSets = aie::trace::getMemoryEventSets(hwGen);
    memoryTileEventSets = aie::trace::getMemoryTileEventSets(hwGen);
    interfaceTileEventSets = aie::trace::getInterfaceTileEventSets(hwGen);

    // Core/memory module counters
    coreCounterStartEvents = xdp::aie::trace::getCoreCounterStartEvents(hwGen, counterScheme);
    coreCounterEndEvents = xdp::aie::trace::getCoreCounterEndEvents(hwGen, counterScheme);
    coreCounterEventValues = xdp::aie::trace::getCoreCounterEventValues(hwGen, counterScheme);
    memoryCounterStartEvents = xdp::aie::trace::getMemoryCounterStartEvents(hwGen, counterScheme);
    memoryCounterEndEvents = xdp::aie::trace::getMemoryCounterEndEvents(hwGen, counterScheme);
    memoryCounterEventValues = xdp::aie::trace::getMemoryCounterEventValues(hwGen, counterScheme);
  }
};

#endif
