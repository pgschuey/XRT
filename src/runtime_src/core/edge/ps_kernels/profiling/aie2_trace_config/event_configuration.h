/* Copyright (C) 2022-2023 Advanced Micro Devices, Inc. - All rights reserved
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
#include "xdp/profile/plugin/aie_trace/x86/aie_trace_kernel_config.h"

constexpr uint32_t ES1_TRACE_COUNTER = 1020;
constexpr uint32_t ES2_TRACE_COUNTER = 0x3FF00;

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
  XAie_Events memTileTraceFlushEndEvent = XAIE_EVENT_USER_EVENT_1_MEM_TILE;

  // Memory tile trace is always on
  XAie_Events memTileTraceStartEvent = XAIE_EVENT_TRUE_MEM_TILE;
  XAie_Events memTileTraceEndEvent = XAIE_EVENT_NONE_MEM_TILE;

  std::map<xdp::built_in::MetricSet, std::vector<XAie_Events>> coreEventsBase;
  std::map<xdp::built_in::MetricSet, std::vector<XAie_Events>> memoryCrossEventsBase;
  std::map<xdp::built_in::MemTileMetricSet, std::vector<XAie_Events>> memTileEventSets;
  std::map<xdp::built_in::ShimTileMetricSet, std::vector<XAie_Events>> shimTileEventSets;

  std::vector<XAie_Events> coreCounterStartEvents;
  std::vector<XAie_Events> coreCounterEndEvents;
  std::vector<uint32_t> coreCounterEventValues;
  std::vector<XAie_Events> memoryCounterStartEvents;
  std::vector<XAie_Events> memoryCounterEndEvents;
  std::vector<uint32_t> memoryCounterEventValues;

  std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> mCoreCounters;
  std::vector<std::shared_ptr<xaiefal::XAiePerfCounter>> mMemoryCounters;

  void initialize(const xdp::built_in::TraceInputConfiguration* params)
  {
    coreEventsBase = {
        {xdp::built_in::MetricSet::FUNCTIONS, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::PARTIAL_STALLS, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::ALL_STALLS, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::ALL_DMA, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::ALL_STALLS_DMA, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::S2MM_CHANNELS_STALLS, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::MM2S_CHANNELS_STALLS, {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}}};

    // **** Memory Module Trace ****
    // NOTE 1: Core events listed here are broadcast by the resource manager
    // NOTE 2: These are supplemented with counter events (AIE1 only)
    memoryCrossEventsBase = {
        {xdp::built_in::MetricSet::FUNCTIONS, 
         {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE}},
        {xdp::built_in::MetricSet::PARTIAL_STALLS,
         {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE, 
          XAIE_EVENT_STREAM_STALL_CORE, XAIE_EVENT_CASCADE_STALL_CORE, 
          XAIE_EVENT_LOCK_STALL_CORE}},
        {xdp::built_in::MetricSet::ALL_STALLS,
         {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE, 
          XAIE_EVENT_MEMORY_STALL_CORE, XAIE_EVENT_STREAM_STALL_CORE, 
          XAIE_EVENT_CASCADE_STALL_CORE, XAIE_EVENT_LOCK_STALL_CORE}},
        {xdp::built_in::MetricSet::ALL_DMA,
         {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE, 
          XAIE_EVENT_PORT_RUNNING_0_CORE, XAIE_EVENT_PORT_RUNNING_1_CORE,
          XAIE_EVENT_PORT_RUNNING_2_CORE, XAIE_EVENT_PORT_RUNNING_3_CORE}},
        {xdp::built_in::MetricSet::ALL_STALLS_DMA,
         {XAIE_EVENT_INSTR_CALL_CORE, XAIE_EVENT_INSTR_RETURN_CORE, 
          XAIE_EVENT_GROUP_CORE_STALL_CORE, XAIE_EVENT_PORT_RUNNING_0_CORE, 
          XAIE_EVENT_PORT_RUNNING_1_CORE, XAIE_EVENT_PORT_RUNNING_2_CORE, 
          XAIE_EVENT_PORT_RUNNING_3_CORE}}
    };

    // Sets w/ DMA stall/backpressure events not supported on AIE1 
    if (params->hwGen > 1) {
      memoryCrossEventsBase[xdp::built_in::MetricSet::S2MM_CHANNELS_STALLS] =
         {XAIE_EVENT_DMA_S2MM_0_START_TASK_MEM,            XAIE_EVENT_DMA_S2MM_0_FINISHED_BD_MEM,
          XAIE_EVENT_DMA_S2MM_0_FINISHED_TASK_MEM,         XAIE_EVENT_DMA_S2MM_0_STALLED_LOCK_MEM,
          XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM,           XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM, 
          XAIE_EVENT_DMA_S2MM_0_MEMORY_BACKPRESSURE_MEM};
      memoryCrossEventsBase[xdp::built_in::MetricSet::MM2S_CHANNELS_STALLS] =
         {XAIE_EVENT_DMA_MM2S_0_START_TASK_MEM,            XAIE_EVENT_DMA_MM2S_0_FINISHED_BD_MEM,
          XAIE_EVENT_DMA_MM2S_0_FINISHED_TASK_MEM,         XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM, 
          XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM,           XAIE_EVENT_DMA_MM2S_0_STREAM_BACKPRESSURE_MEM,
          XAIE_EVENT_DMA_MM2S_0_MEMORY_STARVATION_MEM};
    }
    else {
      if (params->counterScheme == static_cast<uint8_t>(xdp::built_in::CounterScheme::ES1)) {
        // ES1 requires 2 performance counters to get around hardware bugs

        coreCounterStartEvents = {XAIE_EVENT_ACTIVE_CORE, XAIE_EVENT_ACTIVE_CORE};
        coreCounterEndEvents = {XAIE_EVENT_DISABLED_CORE, XAIE_EVENT_DISABLED_CORE};
        coreCounterEventValues = {ES1_TRACE_COUNTER, ES1_TRACE_COUNTER * ES1_TRACE_COUNTER};

        memoryCounterStartEvents = {XAIE_EVENT_TRUE_MEM, XAIE_EVENT_TRUE_MEM};
        memoryCounterEndEvents = {XAIE_EVENT_NONE_MEM, XAIE_EVENT_NONE_MEM};
        memoryCounterEventValues = {ES1_TRACE_COUNTER, ES1_TRACE_COUNTER * ES1_TRACE_COUNTER};

      } 
      else if (params->counterScheme == static_cast<uint8_t>(xdp::built_in::CounterScheme::ES2)) {
        // ES2 requires only 1 performance counter
        coreCounterStartEvents = {XAIE_EVENT_ACTIVE_CORE};
        coreCounterEndEvents = {XAIE_EVENT_DISABLED_CORE};
        coreCounterEventValues = {ES2_TRACE_COUNTER};

        memoryCounterStartEvents = {XAIE_EVENT_TRUE_MEM};
        memoryCounterEndEvents = {XAIE_EVENT_NONE_MEM};
        memoryCounterEventValues = {ES2_TRACE_COUNTER};
      }
    }

    // **** Interface Tile Trace ****
    shimTileEventSets = {
        {xdp::built_in::ShimTileMetricSet::INPUT_PORTS,
         {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL,
          XAIE_EVENT_PORT_RUNNING_2_PL, XAIE_EVENT_PORT_RUNNING_3_PL}},
        {xdp::built_in::ShimTileMetricSet::INPUT_PORTS_STALLS,
         {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
          XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}},
        {xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS,
         {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_RUNNING_1_PL,
          XAIE_EVENT_PORT_RUNNING_2_PL, XAIE_EVENT_PORT_RUNNING_3_PL}},
        {xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS_STALLS,
         {XAIE_EVENT_PORT_RUNNING_0_PL, XAIE_EVENT_PORT_STALLED_0_PL,
         XAIE_EVENT_PORT_RUNNING_1_PL, XAIE_EVENT_PORT_STALLED_1_PL}}
    };

    if (params->hwGen == 1) {
      shimTileEventSets[xdp::built_in::ShimTileMetricSet::INPUT_PORTS_DETAILS] = {
          XAIE_EVENT_DMA_MM2S_0_START_BD_PL, XAIE_EVENT_DMA_MM2S_0_FINISHED_BD_PL,
          XAIE_EVENT_DMA_MM2S_0_STALLED_LOCK_ACQUIRE_PL,
          XAIE_EVENT_DMA_MM2S_1_START_BD_PL, XAIE_EVENT_DMA_MM2S_1_FINISHED_BD_PL,
          XAIE_EVENT_DMA_MM2S_1_STALLED_LOCK_ACQUIRE_PL};
      shimTileEventSets[xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS_DETAILS] = {
          XAIE_EVENT_DMA_S2MM_0_START_BD_PL, XAIE_EVENT_DMA_S2MM_0_FINISHED_BD_PL,
          XAIE_EVENT_DMA_S2MM_0_STALLED_LOCK_ACQUIRE_PL,
          XAIE_EVENT_DMA_S2MM_1_START_BD_PL, XAIE_EVENT_DMA_S2MM_1_FINISHED_BD_PL,
          XAIE_EVENT_DMA_S2MM_1_STALLED_LOCK_ACQUIRE_PL};
    } else {
      shimTileEventSets[xdp::built_in::ShimTileMetricSet::INPUT_PORTS_DETAILS] = {
          XAIE_EVENT_DMA_MM2S_0_START_TASK_PL, XAIE_EVENT_DMA_MM2S_0_FINISHED_BD_PL,
          XAIE_EVENT_DMA_MM2S_0_FINISHED_TASK_PL, XAIE_EVENT_DMA_MM2S_0_STALLED_LOCK_PL,
          XAIE_EVENT_DMA_MM2S_0_STREAM_BACKPRESSURE_PL, XAIE_EVENT_DMA_MM2S_0_MEMORY_STARVATION_PL};
      shimTileEventSets[xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS_DETAILS] = {
          XAIE_EVENT_DMA_S2MM_0_START_TASK_PL, XAIE_EVENT_DMA_S2MM_0_FINISHED_BD_PL,
          XAIE_EVENT_DMA_S2MM_0_FINISHED_TASK_PL, XAIE_EVENT_DMA_S2MM_0_STALLED_LOCK_PL,
          XAIE_EVENT_DMA_S2MM_0_STREAM_STARVATION_PL, XAIE_EVENT_DMA_S2MM_0_MEMORY_BACKPRESSURE_PL};
    }
  }
};

#endif
