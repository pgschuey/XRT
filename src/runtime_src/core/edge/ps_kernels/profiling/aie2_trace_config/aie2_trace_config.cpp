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

#include <cstring>
#include <vector>

#include "core/edge/include/pscontext.h"
#include "core/edge/user/shim.h"
#include "event_configuration.h"
#include "xaiefal/xaiefal.hpp"
#include "xaiengine.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_config.h"
#include "xdp/profile/plugin/aie_trace/x86/aie_trace_kernel_config.h"

// User private data structure container (context object) definition
class xrtHandles : public pscontext {
 public:
  XAie_DevInst* aieDevInst = nullptr;
  xaiefal::XAieDev* aieDev = nullptr;
  xclDeviceHandle handle = nullptr;
  std::vector<XAie_LocType> traceFlushLocs;
  std::vector<XAie_LocType> memTileTraceFlushLocs;

  xrtHandles() = default;
  ~xrtHandles()
  {
    // aieDevInst is not owned by xrtHandles, so don't delete here
    if (aieDev != nullptr)
      delete aieDev;
    // handle is not owned by xrtHandles, so don't close or delete here
  }
};

// Anonymous namespace for helper functions used in this file
namespace {
  /****************************************************************************
   * Configure requested tiles with trace metrics and settings
   ***************************************************************************/
  bool setMetricsSettings(XAie_DevInst* aieDevInst, xaiefal::XAieDev* aieDevice, 
                          EventConfiguration& config,
                          const xdp::built_in::TraceInputConfiguration* params,
                          xdp::built_in::TraceOutputConfiguration* tilecfg, 
                          xdp::built_in::MessageConfiguration* msgcfg,
                          std::vector<XAie_LocType>& traceFlushLocs, 
                          std::vector<XAie_LocType>& memTileTraceFlushLocs)
  {
    xaiefal::Logger::get().setLogLevel(xaiefal::LogLevel::DEBUG);

    // Keep track of number of events reserved per tile
    int numTileCoreTraceEvents[params->NUM_CORE_TRACE_EVENTS + 1] = {0};
    int numTileMemoryTraceEvents[params->NUM_MEMORY_TRACE_EVENTS + 1] = {0};
    int numTileMemTileTraceEvents[params->NUM_MEM_TILE_TRACE_EVENTS + 1] = {0};

    // Create ConfigMetrics Map
    std::map<xrt_core::edge::aie::tile_type, uint8_t> configMetrics;
    // Create Channel Map
    std::map<xrt_core::edge::aie::tile_type, uint8_t> configChannel0;
    std::map<xrt_core::edge::aie::tile_type, uint8_t> configChannel1;

    for (int i = 0; i < params->numTiles; i++) {
      auto tile = xrt_core::edge::aie::tile_type();
      tile.row = params->tiles[i].row;
      tile.col = params->tiles[i].col;
      configMetrics.insert({tile, params->tiles[i].metricSet});

      if (params->tiles[i].channel0 != -1)
        configChannel0.insert({tile, params->tiles[i].channel0});
      if (params->tiles[i].channel1 != -1)
        configChannel1.insert({tile, params->tiles[i].channel1});
    }

    bool useTraceFlush = false;
    if ((params->useUserControl) || (params->useGraphIterator) || (params->useDelay)) {
      if (params->useUserControl)
        config.coreTraceStartEvent = XAIE_EVENT_INSTR_EVENT_0_CORE;
      config.coreTraceEndEvent = config.traceFlushEndEvent;
      config.memTileTraceEndEvent = config.memTileTraceFlushEndEvent;
      useTraceFlush = true;

      std::vector<uint32_t> src = {0, 0, 0, 0};
      addMessage(msgcfg, Messages::ENABLE_TRACE_FLUSH, src);
    }

    int tile_idx = 0;

    // Iterate over all used/specified tiles
    // NOTE: rows are stored as absolute as requred by resource manager
    for (auto& tileMetric : configMetrics) {
      uint8_t metricSet = tileMetric.second;
      auto tile = tileMetric.first;
      auto col = tile.col;
      auto row = tile.row;
      auto type = getModuleType(row, params->offset);

      // NOTE: resource manager requires absolute row number
      xaiefal::XAieMod core;
      if (type == xdp::module_type::core)
        core = aieDevice->tile(col, row).core();

      auto& memory = aieDevice->tile(col, row).mem();
      auto loc = XAie_TileLoc(col, row);

      // Store location to flush at end of run
      if (useTraceFlush) {
        if (type == xdp::module_type::core)
          traceFlushLocs.push_back(loc);
        else if (type == xdp::module_type::mem_tile)
          memTileTraceFlushLocs.push_back(loc);
      }

      // AIE config object for this tile
      auto cfgTile = xdp::built_in::TileData(col, row);
      cfgTile.type = static_cast<uint8_t>(type);
      cfgTile.trace_metric_set = tileMetric.second;

      // Get vector of pre-defined metrics for this set
      // NOTE: these are local copies as we are adding tile/counter-specific events
      std::vector<XAie_Events> coreEvents;
      std::vector<XAie_Events> memoryCrossEvents;
      std::vector<XAie_Events> memoryEvents;

      if (type == xdp::module_type::core) {
        coreEvents = config.coreEventsBase[static_cast<xdp::built_in::MetricSet>(metricSet)];
        memoryCrossEvents = config.memoryCrossEventsBase[static_cast<xdp::built_in::MetricSet>(metricSet)];
      }
      if (type == xdp::module_type::mem_tile) {
        memoryEvents = config.memTileEventSets[static_cast<xdp::built_in::MemTileMetricSet>(metricSet)];
      }

      // Check Resource Availability
      // For now only counters are checked
      if (!tileHasFreeRsc(aieDevice, loc, config, params, msgcfg, type,
                          static_cast<xdp::built_in::MetricSet>(metricSet))) {
        std::cout << "Tile has no Free RSC block hit!" << std::endl;
        std::vector<uint32_t> src = {0, 0, 0, 0};
        addMessage(msgcfg, Messages::NO_RESOURCES, src);
        return 1;
      }

      int numCoreCounters = 0;
      int numMemoryCounters = 0;
      int numCoreTraceEvents = 0;
      int numMemoryTraceEvents = 0;

      //
      // 1. Reserve and start core module counters (as needed)
      //
      if (type == xdp::module_type::core) {
        XAie_ModuleType mod = XAIE_CORE_MOD;

        for (int i = 0; i < config.coreCounterStartEvents.size(); ++i) {
          auto perfCounter = core.perfCounter();
          if (perfCounter->initialize(mod, config.coreCounterStartEvents.at(i), mod,
                                      config.coreCounterEndEvents.at(i)) != XAIE_OK)
            break;
          if (perfCounter->reserve() != XAIE_OK)
            break;

          // NOTE: store events for later use in trace
          XAie_Events counterEvent;
          perfCounter->getCounterEvent(mod, counterEvent);
          int idx = static_cast<int>(counterEvent) - static_cast<int>(XAIE_EVENT_PERF_CNT_0_CORE);
          perfCounter->changeThreshold(config.coreCounterEventValues.at(i));

          // Set reset event based on counter number
          perfCounter->changeRstEvent(mod, counterEvent);
          coreEvents.push_back(counterEvent);

          // If no memory counters are used, then we need to broadcast the core counter
          if (config.memoryCounterStartEvents.empty())
            memoryCrossEvents.push_back(counterEvent);

          if (perfCounter->start() != XAIE_OK)
            break;

          // mCoreCounterTiles.push_back(tile);
          config.mCoreCounters.push_back(perfCounter);
          numCoreCounters++;

          // Update config file
          uint8_t phyEvent = 0;
          auto& cfg = cfgTile.core_trace_config.pc[idx];
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.coreCounterStartEvents[i], &phyEvent);
          cfg.start_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.coreCounterEndEvents[i], &phyEvent);
          cfg.stop_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, counterEvent, &phyEvent);
          cfg.reset_event = phyEvent;
          cfg.event_value = config.coreCounterEventValues[i];
        }
      }

      //
      // 2. Reserve and start memory module counters (as needed)
      //
      if (type == xdp::module_type::core) {
        XAie_ModuleType mod = XAIE_MEM_MOD;

        for (int i = 0; i < config.memoryCounterStartEvents.size(); ++i) {
          auto perfCounter = memory.perfCounter();
          if (perfCounter->initialize(mod, config.memoryCounterStartEvents.at(i), mod,
                                      config.memoryCounterEndEvents.at(i)) != XAIE_OK)
            break;
          if (perfCounter->reserve() != XAIE_OK)
            break;

          // Set reset event based on counter number
          XAie_Events counterEvent;
          perfCounter->getCounterEvent(mod, counterEvent);
          int idx = static_cast<int>(counterEvent) - static_cast<int>(XAIE_EVENT_PERF_CNT_0_MEM);
          perfCounter->changeThreshold(config.memoryCounterEventValues.at(i));

          perfCounter->changeRstEvent(mod, counterEvent);
          memoryEvents.push_back(counterEvent);

          if (perfCounter->start() != XAIE_OK)
            break;

          config.mMemoryCounters.push_back(perfCounter);
          numMemoryCounters++;

          // Update config file
          uint8_t phyEvent = 0;
          auto& cfg = cfgTile.memory_trace_config.pc[idx];
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.memoryCounterStartEvents[i], &phyEvent);
          cfg.start_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.memoryCounterEndEvents[i], &phyEvent);
          cfg.stop_event = phyEvent;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, counterEvent, &phyEvent);
          cfg.reset_event = phyEvent;
          cfg.event_value = config.memoryCounterEventValues[i];
        }
      }

      // Catch when counters cannot be reserved: report, release, and return
      if ((numCoreCounters < config.coreCounterStartEvents.size()) ||
          (numMemoryCounters < config.memoryCounterStartEvents.size())) {
        std::vector<uint32_t> src = {static_cast<uint32_t>(config.coreCounterStartEvents.size()),
                                     static_cast<uint32_t>(config.memoryCounterStartEvents.size()), col,
                                     static_cast<uint32_t>(row)};
        addMessage(msgcfg, Messages::COUNTERS_NOT_RESERVED, src);
        releaseCurrentTileCounters(config);
        return 1;
      }

      //
      // 3. Configure Core Tracing Events
      //
      if (type == xdp::module_type::core) {
        XAie_ModuleType mod = XAIE_CORE_MOD;
        uint8_t phyEvent = 0;
        auto coreTrace = core.traceControl();

        // Delay cycles and user control are not compatible with each other
        if (params->useGraphIterator) {
          if (!configStartIteration(core, config, params))
            break;
        } else if (params->useDelay) {
          if (!configStartDelay(core, config, params))
            break;
        }

        // Set overall start/end for trace capture
        // Wendy said this should be done first
        if (coreTrace->setCntrEvent(config.coreTraceStartEvent, config.coreTraceEndEvent) != XAIE_OK)
          break;

        auto ret = coreTrace->reserve();
        if (ret != XAIE_OK) {
          std::vector<uint32_t> src = {col, static_cast<uint32_t>(row), 0, 0};
          addMessage(msgcfg, Messages::CORE_MODULE_TRACE_NOT_RESERVED, src);
          releaseCurrentTileCounters(config);
          return 1;
        }

        for (int i = 0; i < coreEvents.size(); i++) {
          uint8_t slot;
          if (coreTrace->reserveTraceSlot(slot) != XAIE_OK)
            break;
          if (coreTrace->setTraceEvent(slot, coreEvents[i]) != XAIE_OK)
            break;
          numCoreTraceEvents++;

          // Update config file
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, coreEvents[i], &phyEvent);
          cfgTile.core_trace_config.traced_events[slot] = phyEvent;
        }
        // Update config file
        XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.coreTraceStartEvent, &phyEvent);
        cfgTile.core_trace_config.start_event = phyEvent;
        XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, config.coreTraceEndEvent, &phyEvent);
        cfgTile.core_trace_config.stop_event = phyEvent;

        coreEvents.clear();
        numTileCoreTraceEvents[numCoreTraceEvents]++;

        if (coreTrace->setMode(XAIE_TRACE_EVENT_PC) != XAIE_OK)
          break;
        XAie_Packet pkt = {0, 0};
        if (coreTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (coreTrace->start() != XAIE_OK)
          break;
      }

      //
      // 4. Configure Memory Tracing Events
      //
      // NOTE: this is applicable for memory modules in AIE tiles or MEM tiles
      uint32_t coreToMemBcMask = 0;
      if ((type == xdp::module_type::core) || (type == xdp::module_type::mem_tile)) {
        auto memoryTrace = memory.traceControl();
        // Set overall start/end for trace capture
        // Wendy said this should be done first
        auto traceStartEvent =
            (type == xdp::module_type::core) ? config.coreTraceStartEvent : config.memTileTraceStartEvent;
        auto traceEndEvent = (type == xdp::module_type::core) ? config.coreTraceEndEvent : config.memTileTraceEndEvent;
        if (memoryTrace->setCntrEvent(traceStartEvent, traceEndEvent) != XAIE_OK)
          break;

        auto ret = memoryTrace->reserve();
        if (ret != XAIE_OK) {
          std::vector<uint32_t> src = {col, static_cast<uint32_t>(row + 1), 0, 0};
          addMessage(msgcfg, Messages::MEMORY_MODULE_TRACE_NOT_RESERVED, src);
          releaseCurrentTileCounters(config);
          return 1;
        }

        // Specify Sel0/Sel1 for MEM tile events 21-44
        if (type == xdp::module_type::mem_tile) {
          auto memTileMetricSet = static_cast<xdp::built_in::MemTileMetricSet>(metricSet);
          auto iter0 = configChannel0.find(tile);
          auto iter1 = configChannel1.find(tile);
          uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
          uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;
          configEventSelections(aieDevInst, loc, XAIE_MEM_MOD, type, memTileMetricSet, channel0, channel1);

          // Record for runtime config file
          cfgTile.memory_tile_trace_config.port_trace_ids[0] = channel0;
          cfgTile.memory_tile_trace_config.port_trace_ids[1] = channel1;
          if ((memTileMetricSet == xdp::built_in::MemTileMetricSet::INPUT_CHANNELS) ||
              (memTileMetricSet == xdp::built_in::MemTileMetricSet::INPUT_CHANNELS_STALLS)) {
            cfgTile.memory_tile_trace_config.port_trace_is_master[0] = 1;
            cfgTile.memory_tile_trace_config.port_trace_is_master[1] = 1;
            cfgTile.memory_tile_trace_config.s2mm_channels[0] = channel0;
            if (channel0 != channel1)
              cfgTile.memory_tile_trace_config.s2mm_channels[1] = channel1;
          } else {
            cfgTile.memory_tile_trace_config.port_trace_is_master[0] = 0;
            cfgTile.memory_tile_trace_config.port_trace_is_master[1] = 0;
            cfgTile.memory_tile_trace_config.mm2s_channels[0] = channel0;
            if (channel0 != channel1)
              cfgTile.memory_tile_trace_config.mm2s_channels[1] = channel1;
          }
        }

        // Configure cross module events
        // NOTE: this is only applicable for memory modules, not MEM tiles
        for (int i = 0; i < memoryCrossEvents.size(); i++) {
          uint32_t bcBit = 0x1;
          auto TraceE = memory.traceEvent();
          TraceE->setEvent(XAIE_CORE_MOD, memoryCrossEvents[i]);
          if (TraceE->reserve() != XAIE_OK)
            break;

          int bcId = TraceE->getBc();
          coreToMemBcMask |= (bcBit << bcId);

          if (TraceE->start() != XAIE_OK)
            break;
          numMemoryTraceEvents++;

          // Update config file
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);

          uint8_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, memoryCrossEvents[i], &phyEvent);

          if (type == xdp::module_type::mem_tile) {
            cfgTile.memory_tile_trace_config.traced_events[S] = phyEvent;
          } else {
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile.memory_trace_config.traced_events[S] = aie::bcIdToEvent(bcId);
          }
        }

        // Configure memory trace events
        for (int i = 0; i < memoryEvents.size(); i++) {
          auto TraceE = memory.traceEvent();
          TraceE->setEvent(XAIE_MEM_MOD, memoryEvents[i]);
          if (TraceE->reserve() != XAIE_OK)
            break;
          if (TraceE->start() != XAIE_OK)
            break;
          numMemoryTraceEvents++;

          // Update config file
          // Get Trace slot
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);
          // Get Physical event

          uint8_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, memoryEvents[i], &phyEvent);
          // cfgTile.memory_trace_config.traced_events[S] = phyEvent;

          if (type == xdp::module_type::mem_tile)
            cfgTile.memory_tile_trace_config.traced_events[S] = phyEvent;
          else
            cfgTile.memory_trace_config.traced_events[S] = phyEvent;
        }

        // Update config file
        {
          // Add Memory module trace control events
          uint32_t bcBit = 0x1;
          auto bcId = memoryTrace->getStartBc();
          coreToMemBcMask |= (bcBit << bcId);
          uint8_t phyEvent = 0;

          if (type == xdp::module_type::mem_tile) {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceStartEvent, &phyEvent);
            cfgTile.memory_tile_trace_config.start_event = phyEvent;

          } else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceStartEvent, &phyEvent);
            cfgTile.memory_trace_config.start_event = aie::bcIdToEvent(bcId);
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;
          }

          bcBit = 0x1;
          bcId = memoryTrace->getStopBc();
          coreToMemBcMask |= (bcBit << bcId);
          if (type == xdp::module_type::mem_tile) {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceEndEvent, &phyEvent);
            cfgTile.memory_tile_trace_config.stop_event = aie::bcIdToEvent(bcId);
          } else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceEndEvent, &phyEvent);
            cfgTile.memory_trace_config.stop_event = aie::bcIdToEvent(bcId);
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;

            // Odd absolute rows change east mask end even row change west mask
            if (row % 2)
              cfgTile.core_trace_config.broadcast_mask_east = coreToMemBcMask;
            else
              cfgTile.core_trace_config.broadcast_mask_west = coreToMemBcMask;
          }
        }

        memoryEvents.clear();
        if (type == xdp::module_type::core)
          numTileMemoryTraceEvents[numMemoryTraceEvents]++;
        else
          numTileMemTileTraceEvents[numMemoryTraceEvents]++;

        if (memoryTrace->setMode(XAIE_TRACE_EVENT_TIME) != XAIE_OK)
          break;
        uint8_t packetType = (type == xdp::module_type::mem_tile) ? 3 : 1;
        XAie_Packet pkt = {0, packetType};

        if (memoryTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (memoryTrace->start() != XAIE_OK)
          break;

        // Update memory packet type in config file
        // NOTE: Use time packets for memory module (type 1)
        if (type == xdp::module_type::mem_tile)
          cfgTile.memory_tile_trace_config.packet_type = packetType;
        else
          cfgTile.memory_trace_config.packet_type = packetType;

        std::vector<uint32_t> src = {static_cast<uint32_t>(numCoreTraceEvents),
                                     static_cast<uint32_t>(numMemoryTraceEvents), col, row};
        addMessage(msgcfg, Messages::ALL_TRACE_EVENTS_RESERVED, src);
      }
      tilecfg->tiles[tile_idx] = cfgTile;
      tile_idx++;
    }  // For tiles

    // Report trace events reserved per tile
    {
      for (int n = 0; n <= params->NUM_CORE_TRACE_EVENTS; ++n) {
        if (numTileCoreTraceEvents[n] == 0)
          continue;
        if (n != params->NUM_CORE_TRACE_EVENTS)
          tilecfg->numTileCoreTraceEvents[n] = numTileCoreTraceEvents[n];
      }
    }
    {
      for (int n = 0; n <= params->NUM_MEMORY_TRACE_EVENTS; ++n) {
        if (numTileMemoryTraceEvents[n] == 0)
          continue;
        if (n != params->NUM_MEMORY_TRACE_EVENTS)
          tilecfg->numTileMemoryTraceEvents[n] = numTileMemoryTraceEvents[n];
      }
    }
    {
      for (int n = 0; n <= params->NUM_MEM_TILE_TRACE_EVENTS; ++n) {
        if (numTileMemTileTraceEvents[n] == 0)
          continue;
        if (n != params->NUM_MEM_TILE_TRACE_EVENTS)
          tilecfg->numTileMemTileTraceEvents[n] = numTileMemTileTraceEvents[n];
      }
    }

    return 0;
  }  // end setMetricsSettings

  /****************************************************************************
   * Flush trace modules by forcing end events
   *
   * Trace modules buffer partial packets. At end of run, this needs to be 
   * flushed using a custom end event. This applies to trace windowing and 
   * passive tiles like memory and interface.
   *
   ***************************************************************************/
  void flushTraceModules(XAie_DevInst* aieDevInst, EventConfiguration& config,
                         std::vector<XAie_LocType>& traceFlushLocs,
                         std::vector<XAie_LocType>& memTileTraceFlushLocs)
  {
    /*
     * Flush for trace windowing
     */

    if (traceFlushLocs.empty() && memTileTraceFlushLocs.empty())
      return;

    for (const auto& loc : traceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_CORE_MOD, config.traceFlushEndEvent);
    for (const auto& loc : memTileTraceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_CORE_MOD, config.memTileTraceFlushEndEvent);
    traceFlushLocs.clear();
    memTileTraceFlushLocs.clear();
  }

}  // namespace

#ifdef __cplusplus
extern "C" {
#endif

// The PS kernel initialization function
__attribute__((visibility("default"))) xrtHandles* aie2_trace_config_init(xclDeviceHandle handle,
                                                                          const xuid_t xclbin_uuid)
{
  xrtHandles* constructs = new xrtHandles;
  if (!constructs)
    return nullptr;

  constructs->handle = handle;
  return constructs;
}

// The main PS kernel functionality
__attribute__((visibility("default"))) int aie2_trace_config(uint8_t* input, uint8_t* output, uint8_t* messageOutput,
                                                             int iteration, xrtHandles* constructs)
{
  if (constructs == nullptr)
    return 0;

  auto drv = ZYNQ::shim::handleCheck(constructs->handle);
  if (!drv)
    return 0;

  auto aieArray = drv->getAieArray();
  if (!aieArray)
    return 0;

  constructs->aieDevInst = aieArray->getDevInst();
  if (!constructs->aieDevInst)
    return 0;

  if (constructs->aieDev == nullptr)
    constructs->aieDev = new xaiefal::XAieDev(constructs->aieDevInst, false);

  EventConfiguration config;

  if (iteration == 0) {
    xdp::built_in::TraceInputConfiguration* params = reinterpret_cast<xdp::built_in::TraceInputConfiguration*>(input);
    config.initialize(params);

    xdp::built_in::MessageConfiguration* messageStruct =
        reinterpret_cast<xdp::built_in::MessageConfiguration*>(messageOutput);

    // Using malloc/free instead of new/delete because the struct treats the
    // last element as a variable sized array
    std::size_t total_size =
        sizeof(xdp::built_in::TraceOutputConfiguration) + sizeof(xdp::built_in::TileData[params->numTiles - 1]);
    xdp::built_in::TraceOutputConfiguration* tilecfg = (xdp::built_in::TraceOutputConfiguration*)malloc(total_size);

    tilecfg->numTiles = params->numTiles;

    setMetricsSettings(constructs->aieDevInst, constructs->aieDev, config, params, tilecfg, messageStruct,
                       constructs->traceFlushLocs, constructs->memTileTraceFlushLocs);
    uint8_t* out = reinterpret_cast<uint8_t*>(tilecfg);
    std::memcpy(output, out, total_size);

    // Clean up
    free(tilecfg);

    // flush iteration
  } else if (iteration == 1) {
    flushTraceModules(constructs->aieDevInst, config, constructs->traceFlushLocs,
                      constructs->memTileTraceFlushLocs);
  }

  return 0;
}

// The final function for the PS kernel
__attribute__((visibility("default"))) int aie2_trace_config_fini(xrtHandles* handles)
{
  if (handles != nullptr)
    delete handles;
  return 0;
}

#ifdef __cplusplus
}
#endif
