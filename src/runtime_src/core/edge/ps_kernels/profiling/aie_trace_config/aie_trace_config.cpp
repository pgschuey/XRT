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
#include "xdp/profile/device/tracedefs.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_config.h"
#include "xdp/profile/plugin/aie_trace/util/aie_trace_util.h"
#include "xdp/profile/plugin/aie_trace/x86/aie_trace_kernel_config.h"

// User private data structure container (context object) definition
class xrtHandles : public pscontext {
 public:
  XAie_DevInst* aieDevInst = nullptr;
  xaiefal::XAieDev* aieDev = nullptr;
  xclDeviceHandle handle = nullptr;
  std::vector<XAie_LocType> traceFlushLocs;
  std::vector<XAie_LocType> memTileTraceFlushLocs;
  std::vector<XAie_LocType> interfaceTileTraceFlushLocs;

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
  using Messages = xdp::built_in::Messages;
  using trace = xdp::aie::trace;

  /****************************************************************************
   * Add message to return array
   ***************************************************************************/
  void addMessage(xdp::built_in::MessageConfiguration* msgcfg, 
                  xdp::built_in::Messages ERROR_MSG,
                  std::vector<uint32_t>& paramsArray)
  {
    static int messageCounter = 0;

    if (messageCounter < xdp::built_in::MessageConfiguration::MAX_NUM_MESSAGES) {
      msgcfg->packets[messageCounter].messageCode = static_cast<uint8_t>(ERROR_MSG);
      std::copy(std::begin(paramsArray), std::end(paramsArray), 
                std::begin(msgcfg->packets[messageCounter].params));
      messageCounter++;
      msgcfg->numMessages = messageCounter;
    }
  }

  /****************************************************************************
   * Check if given tile has free resources
   ***************************************************************************/
  bool tileHasFreeRsc(xaiefal::XAieDev* aieDevice, XAie_LocType& loc, EventConfiguration& config,
                      const xdp::built_in::TraceInputConfiguration* params, 
                      xdp::built_in::MessageConfiguration* msgcfg,
                      xdp::module_type type, const xdp::built_in::MetricSet metricSet)
  {
    auto stats = aieDevice->getRscStat(XAIEDEV_DEFAULT_GROUP_AVAIL);
    uint32_t available = 0;
    uint32_t required = 0;

    // Memory module perf counters
    if (params->hwGen == 1) {
      available = stats.getNumRsc(loc, XAIE_MEM_MOD, XAIE_PERFCNT_RSC);
      required = config.memoryCounterStartEvents.size();
      if (available < required) {
        std::vector<uint32_t> src = {available, required, 0, 0};
        addMessage(msgcfg, Messages::NO_MEM_MODULE_PCS, src);
        return false;
      }
    }

    // Memory module trace slots
    available = stats.getNumRsc(loc, XAIE_MEM_MOD, xaiefal::XAIE_TRACE_EVENTS_RSC);
    required = config.memoryCounterStartEvents.size() 
             + config.memoryEventSets[metricSet].size();
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_MEM_MODULE_TRACE_SLOTS, src);
      return false;
    }

    // Core resources only needed in AIE tiles
    if ((type == xdp::module_type::mem_tile) || (type == xdp::module_type::shim))
      return true;

    // Core module perf counters
    if (params->hwGen == 1) {
      available = stats.getNumRsc(loc, XAIE_CORE_MOD, XAIE_PERFCNT_RSC);
      required = config.coreCounterStartEvents.size();
      if (params->useDelay) {
        ++required;
        if (params->useOneDelayCounter)
          ++required;
      } else if (params->useGraphIterator)
        ++required;

      if (available < required) {
        std::vector<uint32_t> src = {available, required, 0, 0};
        addMessage(msgcfg, Messages::NO_CORE_MODULE_PCS, src);
        return false;
      }
    }

    // Core module trace slots
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, xaiefal::XAIE_TRACE_EVENTS_RSC);
    required = config.coreCounterStartEvents.size() + config.coreEventSets[metricSet].size();
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_CORE_MODULE_TRACE_SLOTS, src);
      return false;
    }

    // Core module broadcasts
    // NOTE: the addition of 2 is for start/end trace
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, XAIE_BCAST_CHANNEL_RSC);
    required = config.memoryEventSets[metricSet].size() + 2;
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_CORE_MODULE_BROADCAST_CHANNELS, src);
      return false;
    }

    return true;
  }

  /****************************************************************************
   * Release requested counters
   ***************************************************************************/
  void releaseCurrentTileCounters(EventConfiguration& config)
  {
    while (!config.coreCounters.empty()) {
      config.coreCounters.back()->stop();
      config.coreCounters.back()->release();
      config.coreCounters.pop_back();
    }

    while (!config.memoryCounters.empty()) {
      config.memoryCounters.back()->stop();
      config.memoryCounters.back()->release();
      config.memoryCounters.pop_back();
    }
  }

  std::string getMetricSetName(xdp::module_type type, uint8_t metricSetInt)
  {
    // AIE tiles
    if (type == xdp::module_type::core) {
      switch (static_cast<xdp::built_in::MetricSet>(metricSetInt)) {
      case xdp::built_in::MetricSet::FUNCTIONS:
        return "functions";
      case xdp::built_in::MetricSet::PARTIAL_STALLS:
        return "partial_stalls";
      case xdp::built_in::MetricSet::ALL_STALLS:
        return "all_stalls";
      case xdp::built_in::MetricSet::ALL_DMA:
        return "all_dma";
      case xdp::built_in::MetricSet::ALL_STALLS_DMA:
        return "all_stalls_dma";
      case xdp::built_in::MetricSet::S2MM_CHANNELS_STALLS:
        return "s2mm_channels_stalls";
      case xdp::built_in::MetricSet::MM2S_CHANNELS_STALLS: 
        return "mm2s_channels_stalls";
      default:
        return "unsupported";
      }
    }

    // Memory tiles
    if (type == xdp::module_type::mem_tile) {
      switch (static_cast<xdp::built_in::MemTileMetricSet>(metricSetInt)) {
      case xdp::built_in::MemTileMetricSet::INPUT_CHANNELS:
        return "input_channels";
      case xdp::built_in::MemTileMetricSet::INPUT_CHANNELS_STALLS:
        return "input_channels_stalls";
      case xdp::built_in::MemTileMetricSet::OUTPUT_CHANNELS:
        return "output_channels";
      case xdp::built_in::MemTileMetricSet::OUTPUT_CHANNELS_STALLS:
        return "output_channels_stalls";
      case xdp::built_in::MemTileMetricSet::MEMORY_CONFLICTS1:
        return "memory_conflicts1";
      case xdp::built_in::MemTileMetricSet::MEMORY_CONFLICTS2:
        return "memory_conflicts2";
      default:
        return "unsupported";
      }
    }

    // Interface tiles
    switch (static_cast<xdp::built_in::ShimTileMetricSet>(metricSetInt)) {
    case xdp::built_in::ShimTileMetricSet::INPUT_PORTS:
      return "input_ports";
    case xdp::built_in::ShimTileMetricSet::INPUT_PORTS_STALLS:
      return "input_ports_stalls";
    case xdp::built_in::ShimTileMetricSet::INPUT_PORTS_DETAILS:
      return "input_ports_details";
    case xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS:
      return "output_ports";
    case xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS_STALLS:
      return "output_ports_stalls";
    case xdp::built_in::ShimTileMetricSet::OUTPUT_PORTS_DETAILS:
      return "output_ports_details";
    default:
      return "unsupported";
    }
  }

  /****************************************************************************
   * Configure requested tiles with trace metrics and settings
   ***************************************************************************/
  bool setMetricsSettings(XAie_DevInst* aieDevInst, xaiefal::XAieDev* aieDevice, 
                          EventConfiguration& config,
                          const xdp::built_in::TraceInputConfiguration* params,
                          xdp::built_in::TraceOutputConfiguration* tilecfg, 
                          xdp::built_in::MessageConfiguration* msgcfg,
                          std::vector<XAie_LocType>& traceFlushLocs, 
                          std::vector<XAie_LocType>& memTileTraceFlushLocs,
                          std::vector<XAie_LocType>& interfaceTileTraceFlushLocs)
  {
    // Keep track of number of events reserved per tile
    int numTileCoreTraceEvents[xdp::NUM_TRACE_EVENTS + 1] = {0};
    int numTileMemoryTraceEvents[xdp::NUM_TRACE_EVENTS + 1] = {0};
    int numTileMemoryTileTraceEvents[xdp::NUM_TRACE_EVENTS + 1] = {0};
    int numTileInterfaceTileTraceEvents[xdp::NUM_TRACE_EVENTS + 1] = {0};

    // Create ConfigMetrics Map
    std::map<xdp::tile_type, uint8_t> configMetrics;
    // Create Channel Map
    std::map<xdp::tile_type, uint8_t> configChannel0;
    std::map<xdp::tile_type, uint8_t> configChannel1;

    for (int i = 0; i < params->numTiles; i++) {
      auto tile = xdp::tile_type();
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
      config.memTileTraceEndEvent = config.memTileTraceEndEvent;
      useTraceFlush = true;

      std::vector<uint32_t> src = {0, 0, 0, 0};
      addMessage(msgcfg, Messages::ENABLE_TRACE_FLUSH, src);
    }

    int tile_idx = 0;

    // Iterate over all used/specified tiles
    for (auto& tileMetric : configMetrics) {
      auto tile          = tileMetric.first;
      auto col           = tile.col;
      auto row           = tile.row;
      auto subtype       = tile.subtype;
      auto type          = xdp::aie::getModuleType(row, params->offset);
      auto typeInt       = static_cast<int>(type);
      auto& xaieTile     = aieDevice->tile(col, row);
      auto loc           = XAie_TileLoc(col, row);
      
      auto& metricSetInt = tileMetric.second;
      auto metricSet     = getMetricSetName(type, metricSetInt);

      std::string tileName = (type == xdp::module_type::mem_tile) ? "memory" 
                           : ((type == xdp::module_type::shim) ? "interface" : "AIE");
      tileName.append(" tile (" + std::to_string(col) + "," + std::to_string(row) + ")");

      xaiefal::XAieMod core;
      xaiefal::XAieMod memory;
      xaiefal::XAieMod shim;
      if (type == xdp::module_type::core)
        core = xaieTile.core();
      if (type == xdp::module_type::shim)
        shim = xaieTile.pl();
      else
        memory = xaieTile.mem();

      // Store location to flush at end of run
      if (useTraceFlush || (type == xdp::module_type::mem_tile) 
          || (type == xdp::module_type::shim)) {
        if (type == xdp::module_type::core)
          traceFlushLocs.push_back(loc);
        else if (type == xdp::module_type::mem_tile)
          memoryTileTraceFlushLocs.push_back(loc);
        else if (type == xdp::module_type::shim)
          interfaceTileTraceFlushLocs.push_back(loc);
      }

      // AIE config object for this tile
      auto cfgTile = xdp::built_in::TileData(col, row);
      cfgTile.type = static_cast<uint8_t>(type);
      cfgTile.trace_metric_set = metricSet;

      // Get vector of pre-defined metrics for this set
      // NOTE: these are local copies as we are adding tile/counter-specific events
      std::vector<XAie_Events> coreEvents;
      std::vector<XAie_Events> memoryEvents;
      std::vector<XAie_Events> interfaceEvents;
      if (type == xdp::module_type::core) {
        coreEvents = config.coreEventSets[metricSet];
        memoryEvents = config.memoryEventSets[metricSet];
      }
      else if (type == xdp::module_type::mem_tile) {
        memoryEvents = config.memoryTileEventSets[metricSet];
      }
      else if (type == xdp::module_type::shim) {
        interfaceEvents = config.interfaceTileEventSets[metricSet];
      }

      // Check resource availability
      if (!tileHasFreeRsc(aieDevice, loc, config, params, msgcfg, type,
                          static_cast<xdp::built_in::MetricSet>(metricSetInt))) {
        std::vector<uint32_t> src = {0, 0, 0, 0};
        addMessage(msgcfg, Messages::NO_RESOURCES, src);
        return 1;
      }

      int numCoreCounters = 0;
      int numMemoryCounters = 0;
      int numCoreTraceEvents = 0;
      int numMemoryTraceEvents = 0;
      int numInterfaceTraceEvents = 0;

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
            memoryEvents.push_back(counterEvent);

          if (perfCounter->start() != XAIE_OK)
            break;

          config.coreCounters.push_back(perfCounter);
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

          config.memoryCounters.push_back(perfCounter);
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
          if (!xdp::aie::trace::configStartIteration(core, config, params))
            break;
        } else if (params->useDelay) {
          if (!xdp::aie::trace::configStartDelay(core, config, params))
            break;
        }

        // Configure combo & group events (e.g., to monitor DMA channels)
        auto comboEvents = xdp::aie::trace::configComboEvents(aieDevInst, xaieTile, loc, mod, type, 
                                                              metricSet, cfgTile.core_trace_config);
        xdp::aie::trace::configGroupEvents(aieDevInst, loc, mod, type, metricSet);

        // Set overall start/end for trace capture
        if (coreTrace->setCntrEvent(config.coreTraceStartEvent, config.coreTraceEndEvent) != XAIE_OK)
          break;

        if (coreTrace->reserve() != XAIE_OK) {
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
      // NOTE: this is applicable for memory modules in AIE tiles or memory tiles
      uint32_t coreToMemBcMask = 0;
      if ((type == xdp::module_type::core) || (type == xdp::module_type::mem_tile)) {
        auto memoryTrace = memory.traceControl();
        auto traceStartEvent = (type == xdp::module_type::core) ? 
          config.coreTraceStartEvent : config.memTileTraceStartEvent;
        auto traceEndEvent = (type == xdp::module_type::core) ? 
          config.coreTraceEndEvent : config.memTileTraceEndEvent;

        xdp::aie_cfg_base& aieConfig = cfgTile.core_trace_config;
        if (type == xdp::module_type::mem_tile)
          aieConfig = cfgTile.memory_tile_trace_config;

        // Configure combo events for metric sets that include DMA events        
        auto comboEvents = xdp::aie::trace::configComboEvents(aieDevInst, xaieTile, loc, 
            XAIE_CORE_MOD, xdp::module_type::dma, metricSet, aieConfig);
        if (comboEvents.size() == 2) {
          traceStartEvent = comboEvents.at(0);
          traceEndEvent = comboEvents.at(1);
        }

        // Configure event ports on stream switch
        // NOTE: These are events from the core module stream switch
        //       outputted on the memory module trace stream. 
        streamPorts = xdp::aie::trace::configStreamSwitchPorts(aieDevInst, tile,
            xaieTile, loc, type, metricSet, 0, 0, memoryEvents, aieConfig);
          
        // Set overall start/end for trace capture
        if (memoryTrace->setCntrEvent(traceStartEvent, traceEndEvent) != XAIE_OK)
          break;

        if (memoryTrace->reserve() != XAIE_OK) {
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
          xdp::aie::trace::configEventSelections(aieDevInst, loc, XAIE_MEM_MOD, type, 
                                                 memTileMetricSet, channel0, channel1);

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

        // Configure memory trace events
        for (int i = 0; i < memoryEvents.size(); i++) {
          bool isCoreEvent = xdp::aie::trace::isCoreModuleEvent(memoryEvents[i]);
          XAie_ModuleType mod = isCoreEvent ? XAIE_CORE_MOD : XAIE_MEM_MOD;

          auto TraceE = memory.traceEvent();
          TraceE->setEvent(mod, memoryEvents[i]);
          if (TraceE->reserve() != XAIE_OK)
            break;
          if (TraceE->start() != XAIE_OK)
            break;
          numMemoryTraceEvents++;
          
          // Configure edge events (as needed)
          xdp::aie::trace::configEdgeEvents(aieDevInst, tile, type, metricSet, memoryEvents[i]);

          // Update config file
          // Get Trace slot
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);

          // Get physical event
          uint8_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, memoryEvents[i], &phyEvent);

          if (isCoreEvent) {
            auto bcId = TraceE->getBc();
            coreToMemBcMask |= (1 << bcId);
            
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;
            cfgTile.memory_trace_config.traced_events[S] = xdp::aie::bcIdToEvent(bcId);
          }
          else {
            cfgTile.memory_tile_trace_config.traced_events[S] = phyEvent;
          }
        }

        // Add trace control events to config file
        {
          uint8_t phyEvent = 0;

          // Start
          if (xdp::aie::trace::isCoreModuleEvent(traceStartEvent)) {
            auto bcId = memoryTrace->getStartBc();
            coreToMemBcMask |= (1 << bcId);

            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceStartEvent, &phyEvent);
            cfgTile.memory_trace_config.start_event = xdp::aie::bcIdToEvent(bcId);
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;
          }
          else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceStartEvent, &phyEvent);
            if (type == xdp::module_type::mem_tile)
              cfgTile.memory_tile_trace_config.start_event = phyEvent;
            else
              cfgTile.memory_trace_config.start_event = phyEvent;
          }

          // Stop
          if (xdp::aie::trace::isCoreModuleEvent(traceEndEvent)) {
            auto bcId = memoryTrace->getStopBc();
            coreToMemBcMask |= (1 << bcId);
          
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_CORE_MOD, traceEndEvent, &phyEvent);
            cfgTile.memory_trace_config.stop_event = xdp::aie::bcIdToEvent(bcId);
            cfgTile.core_trace_config.internal_events_broadcast[bcId] = phyEvent;

            // Use east broadcasting for AIE2+ or odd absolute rows of AIE1 checkerboard
            if ((row % 2) || (params->hwGen > 1))
              cfgTile.core_trace_config.broadcast_mask_east = coreToMemBcMask;
            else
              cfgTile.core_trace_config.broadcast_mask_west = coreToMemBcMask;
          }
          else {
            XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_MEM_MOD, traceEndEvent, &phyEvent);
            if (type == xdp::module_type::mem_tile)
              cfgTile.memory_tile_trace_config.stop_event = phyEvent;
            else
              cfgTile.memory_trace_config.stop_event = phyEvent;
          }
        }

        memoryEvents.clear();
        if (type == xdp::module_type::core)
          numTileMemoryTraceEvents[numMemoryTraceEvents]++;
        else
          numTileMemoryTileTraceEvents[numMemoryTraceEvents]++;

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

      //
      // 5. Configure Interface Tile Tracing Events
      //
      if (type == xdp::module_type::shim) {
        auto shimTrace = shim.traceControl();
        if (shimTrace->setCntrEvent(config.interfaceTileTraceStartEvent, 
            config.interfaceTileTraceEndEvent) != XAIE_OK)
          break;

        if (shimTrace->reserve() != XAIE_OK) {
          std::vector<uint32_t> src = {col, row, 0, 0};
          addMessage(msgcfg, Messages::INTERFACE_TRACE_NOT_RESERVED, src);
          releaseCurrentTileCounters(config);
          return 1;
        }

        // Specify channels for interface tile DMA events
        auto iter0 = configChannel0.find(tile);
        auto iter1 = configChannel1.find(tile);
        uint8_t channel0 = (iter0 == configChannel0.end()) ? 0 : iter0->second;
        uint8_t channel1 = (iter1 == configChannel1.end()) ? 1 : iter1->second;

        // Modify events as needed
        xdp::aie::trace::modifyEvents(type, subtype, metricSet, channel0, interfaceEvents);

        // Record for runtime config file
        if (type == xdp::module_type::shim) {
          if (xdp::aie::isInputSet(type, metricSet)) {
            cfgTile.interface_tile_trace_config.mm2s_channels[0] = channel0;
            if (channel0 != channel1)
              cfgTile.interface_tile_trace_config.mm2s_channels[1] = channel1;
          } 
          else {
            cfgTile.interface_tile_trace_config.s2mm_channels[0] = channel0;
            if (channel0 != channel1)
              cfgTile.interface_tile_trace_config.s2mm_channels[1] = channel1;
          }
        }

        streamPorts = xdp::aie::trace::configStreamSwitchPorts(aieDevInst, tile, xaieTile, loc, type,
          metricSet, channel0, channel1, interfaceEvents, cfgTile.interface_tile_trace_config);

        // Configure interface tile trace events
        for (int i = 0; i < interfaceEvents.size(); i++) {
          auto event = interfaceEvents.at(i);
          auto TraceE = shim.traceEvent();
          TraceE->setEvent(XAIE_PL_MOD, event);
          if (TraceE->reserve() != XAIE_OK)
            break;
          if (TraceE->start() != XAIE_OK)
            break;
          numInterfaceTraceEvents++;

          // Update config file
          // Get Trace slot
          uint32_t S = 0;
          XAie_LocType L;
          XAie_ModuleType M;
          TraceE->getRscId(L, M, S);
          // Get Physical event
          uint8_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, event, &phyEvent);
          cfgTile.interface_tile_trace_config.traced_events[S] = phyEvent;
        }

        // Update config file
        {
          // Add interface trace control events
          // Start
          uint8_t phyEvent = 0;
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, 
            config.interfaceTileTraceStartEvent, &phyEvent);
          cfgTile.interface_tile_trace_config.start_event = phyEvent;
          // Stop
          XAie_EventLogicalToPhysicalConv(aieDevInst, loc, XAIE_PL_MOD, 
            config.interfaceTileTraceEndEvent, &phyEvent);
          cfgTile.interface_tile_trace_config.stop_event = phyEvent;
        }

        // Record allocated trace events
        numTileInterfaceTileTraceEvents[numInterfaceTraceEvents]++;
        
        // Specify packet type and ID then start interface tile trace
        // NOTE: always use time packets
        if (shimTrace->setMode(XAIE_TRACE_EVENT_TIME) != XAIE_OK)
          break;
        uint8_t packetType = 4;
        XAie_Packet pkt = {0, packetType};
        if (shimTrace->setPkt(pkt) != XAIE_OK)
          break;
        if (shimTrace->start() != XAIE_OK)
          break;
        cfgTile.interface_tile_trace_config.packet_type = packetType;
      } // interface tiles

      tilecfg->tiles[tile_idx] = cfgTile;
      tile_idx++;
    }  // For tiles

    // Report trace events reserved per tile
    for (int n = 0; n < xdp::NUM_TRACE_EVENTS; ++n) {
      tilecfg->numTileCoreTraceEvents[n] = numTileCoreTraceEvents[n];
      tilecfg->numTileMemoryTraceEvents[n] = numTileMemoryTraceEvents[n];
      tilecfg->numTileMemoryTileTraceEvents[n] = numTileMemoryTileTraceEvents[n];
      tilecfg->numTileInterfaceTileTraceEvents[n] = numTileInterfaceTileTraceEvents[n];
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
                         std::vector<XAie_LocType>& memTileTraceFlushLocs,
                         std::vector<XAie_LocType>& interfaceTileTraceFlushLocs)
  {
    if (traceFlushLocs.empty() && memTileTraceFlushLocs.empty()
        && interfaceTileTraceFlushLocs.empty())
      return;

    for (const auto& loc : traceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_CORE_MOD, config.traceFlushEndEvent);
    for (const auto& loc : memTileTraceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_MEM_MOD, config.memTileTraceEndEvent);
    for (const auto& loc : interfaceTileTraceFlushLocs)
      XAie_EventGenerate(aieDevInst, loc, XAIE_PL_MOD, config.interfaceTileTraceEndEvent);

    traceFlushLocs.clear();
    memTileTraceFlushLocs.clear();
    interfaceTileTraceFlushLocs.clear();
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
                       constructs->traceFlushLocs, constructs->memTileTraceFlushLocs,
                       constructs->interfaceTileTraceFlushLocs);
    uint8_t* out = reinterpret_cast<uint8_t*>(tilecfg);
    std::memcpy(output, out, total_size);

    // Clean up
    free(tilecfg);

    // flush iteration
  } else if (iteration == 1) {
    flushTraceModules(constructs->aieDevInst, config, constructs->traceFlushLocs,
                      constructs->memTileTraceFlushLocs, constructs->interfaceTileTraceFlushLocs);
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
