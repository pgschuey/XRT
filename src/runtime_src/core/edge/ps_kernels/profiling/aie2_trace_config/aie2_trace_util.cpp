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
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/plugin/aie_trace/x86/aie_trace_kernel_config.h"

// Namespace for helper functions
namespace xdp::aie::trace::ps_kernel {
  using Messages = xdp::built_in::Messages;

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

    // Memory Module perf counters
    available = stats.getNumRsc(loc, XAIE_MEM_MOD, XAIE_PERFCNT_RSC);
    required = config.memoryCounterStartEvents.size();
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_MEM_MODULE_PCS, src);
      return false;
    }

    // Memory Module trace slots
    available = stats.getNumRsc(loc, XAIE_MEM_MOD, xaiefal::XAIE_TRACE_EVENTS_RSC);
    required = config.memoryCounterStartEvents.size() 
             + config.memoryCrossEventsBase[metricSet].size();
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_MEM_MODULE_TRACE_SLOTS, src);
      return false;
    }

    // Core resources not needed in MEM tiles
    if (type == xdp::module_type::mem_tile)
      return true;

    // Core Module perf counters
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

    // Core Module trace slots
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, xaiefal::XAIE_TRACE_EVENTS_RSC);
    required = config.coreCounterStartEvents.size() + config.coreEventsBase[metricSet].size();
    if (available < required) {
      std::vector<uint32_t> src = {available, required, 0, 0};
      addMessage(msgcfg, Messages::NO_CORE_MODULE_TRACE_SLOTS, src);
      return false;
    }

    // Core Module broadcasts. 2 events for starting/ending trace
    available = stats.getNumRsc(loc, XAIE_CORE_MOD, XAIE_BCAST_CHANNEL_RSC);
    required = config.memoryCrossEventsBase[metricSet].size() + 2;
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
    while (!config.mCoreCounters.empty()) {
      config.mCoreCounters.back()->stop();
      config.mCoreCounters.back()->release();
      config.mCoreCounters.pop_back();
    }

    while (!config.mMemoryCounters.empty()) {
      config.mMemoryCounters.back()->stop();
      config.mMemoryCounters.back()->release();
      config.mMemoryCounters.pop_back();
    }
  }

  /****************************************************************************
   * Check if metric set contains DMA events
   ***************************************************************************/
  bool isDmaSet(const xdp::built_in::MetricSet metricSet)
  {
    switch (metricSet) {
      case xdp::built_in::MetricSet::ALL_DMA:
      case xdp::built_in::MetricSet::ALL_STALLS_DMA:
      case xdp::built_in::MetricSet::S2MM_CHANNELS_STALLS:
      case xdp::built_in::MetricSet::MM2S_CHANNELS_STALLS:
        return true;
      default:
        return false;
    }
  }

  /****************************************************************************
   * Configure stream switch event ports for monitoring purposes
   ***************************************************************************/
  std::vector<std::shared_ptr<xaiefal::XAieStreamPortSelect>>
  configStreamSwitchPorts(XAie_DevInst* aieDevInst, const tile_type& tile,
                          xaiefal::XAieTile& xaieTile, const XAie_LocType loc,
                          const module_type type, const uint8_t channel0, const uint8_t channel1, 
                          std::vector<XAie_Events>& events, aie_cfg_base& config,
                          const xdp::built_in::MetricSet metricSet,
                          const xdp::built_in::MemTileMetricSet memTileMetricSet)
  {
    std::vector<std::shared_ptr<xaiefal::XAieStreamPortSelect>> streamPorts;
    std::map<uint8_t, std::shared_ptr<xaiefal::XAieStreamPortSelect>> switchPortMap;

    // Traverse all counters and request monitor ports as needed
    for (int i=0; i < events.size(); ++i) {
      // Ensure applicable event
      auto event = events.at(i);
      if (!xdp::aie::trace::isStreamSwitchPortEvent(event))
        continue;

      bool newPort = false;
      auto portnum = xdp::aie::trace::getPortNumberFromEvent(event);

      // New port needed: reserver, configure, and store
      if (switchPortMap.find(portnum) == switchPortMap.end()) {
        auto switchPortRsc = xaieTile.sswitchPort();
        if (switchPortRsc->reserve() != AieRC::XAIE_OK)
          continue;
        newPort = true;
        switchPortMap[portnum] = switchPortRsc;

        if (type == module_type::core) {
          // AIE Tiles - monitor DMA channels
          if (isDmaSet(metricSet)) {
            uint8_t channelNum = portnum % 2;
            auto slaveOrMaster = (portnum < 2) ? XAIE_STRMSW_SLAVE : XAIE_STRMSW_MASTER;
            //std::string typeName = (portnum < 2) ? "MM2S" : "S2MM";
            //std::string msg = "Configuring core module stream switch to monitor DMA " 
            //                + typeName + " channel " + std::to_string(channelNum);
            //xrt_core::message::send(severity_level::debug, "XRT", msg);
            switchPortRsc->setPortToSelect(slaveOrMaster, DMA, channelNum);

            config.port_trace_ids[portnum] = channelNum;
            config.port_trace_is_master[portnum] = (slaveOrMaster == XAIE_STRMSW_MASTER);
          }
        }
        else if (type == module_type::shim) {
          // Interface tiles (e.g., PLIO, GMIO)
          // Grab slave/master and stream ID
          auto slaveOrMaster = (tile.itr_mem_col == 0) ? XAIE_STRMSW_SLAVE : XAIE_STRMSW_MASTER;
          auto streamPortId  = static_cast<uint8_t>(tile.itr_mem_row);
          //std::string typeName = (tile.itr_mem_col == 0) ? "slave" : "master";
          //std::string msg = "Configuring interface tile stream switch to monitor " 
          //                + typeName + " stream port " + std::to_string(streamPortId);
          //xrt_core::message::send(severity_level::debug, "XRT", msg);
          switchPortRsc->setPortToSelect(slaveOrMaster, SOUTH, streamPortId);

          config.port_trace_ids[portnum] = streamPortId;
          config.port_trace_is_master[portnum] = (tile.is_master != 0);
        }
        else {
          // Memory tiles
          uint8_t channel = (portnum == 0) ? channel0 : channel1;
          auto slaveOrMaster = isInputSet(type, metricSet, memTileMetricSet) 
                              ? XAIE_STRMSW_MASTER : XAIE_STRMSW_SLAVE;
          //std::string typeName = (slaveOrMaster == XAIE_STRMSW_MASTER) ? "master" : "slave";
          //std::string msg = "Configuring memory tile stream switch to monitor " 
          //                + typeName + " stream port " + std::to_string(channel);
          //xrt_core::message::send(severity_level::debug, "XRT", msg);
          switchPortRsc->setPortToSelect(slaveOrMaster, DMA, channel);

          config.port_trace_ids[portnum] = channel;
          config.port_trace_is_master[portnum] = (slaveOrMaster == XAIE_STRMSW_MASTER);
        }
      }

      auto switchPortRsc = switchPortMap[portnum];

      // Event options:
      //   getSSIdleEvent, getSSRunningEvent, getSSStalledEvent, & getSSTlastEvent
      XAie_Events ssEvent;
      if (isPortRunningEvent(event))
        switchPortRsc->getSSRunningEvent(ssEvent);
      else
        switchPortRsc->getSSStalledEvent(ssEvent);
      events.at(i) = ssEvent;

      if (newPort) {
        switchPortRsc->start();
        streamPorts.push_back(switchPortRsc);
      }
    }

    switchPortMap.clear();
    return streamPorts;
  }

  /****************************************************************************
   * Configure combo events (AIE tiles only)
   ***************************************************************************/
  std::vector<XAie_Events>
  configComboEvents(XAie_DevInst* aieDevInst, xaiefal::XAieTile& xaieTile, 
                    const XAie_LocType loc, const XAie_ModuleType mod,
                    const module_type type, const xdp::built_in::MetricSet metricSet,
                    aie_cfg_base& config)
  {
    // Only needed for core/memory modules and metric sets that include DMA events
    if (!isDmaSet(metricSet) || ((type != module_type::core) && (type != module_type::dma)))
      return {};

    std::vector<XAie_Events> comboEvents;

    if (type == module_type::core) {
      auto comboEvent = xaieTile.core().comboEvent(4);
      comboEvents.push_back(XAIE_EVENT_COMBO_EVENT_2_CORE);

      // Combo2 = Port_Idle_0 OR Port_Idle_1 OR Port_Idle_2 OR Port_Idle_3
      std::vector<XAie_Events> events = {XAIE_EVENT_PORT_IDLE_0_CORE,
          XAIE_EVENT_PORT_IDLE_1_CORE, XAIE_EVENT_PORT_IDLE_2_CORE,
          XAIE_EVENT_PORT_IDLE_3_CORE};
      std::vector<XAie_EventComboOps> opts = {XAIE_EVENT_COMBO_E1_OR_E2, 
          XAIE_EVENT_COMBO_E1_OR_E2, XAIE_EVENT_COMBO_E1_OR_E2};

      // Capture in config class to report later
      for (int i=0; i < NUM_COMBO_EVENT_CONTROL; ++i)
        config.combo_event_control[i] = 2;
      for (int i=0; i < events.size(); ++i) {
        uint8_t phyEvent = 0;
        XAie_EventLogicalToPhysicalConv(aieDevInst, loc, mod, events.at(i), &phyEvent);
        config.combo_event_input[i] = phyEvent;
      }

      // Set events and trigger on OR of events
      comboEvent->setEvents(events, opts);
      return comboEvents;
    }

    // Since we're tracing DMA events, start trace right away.
    // Specify user event 0 as trace end so we can flush after run.
    comboEvents.push_back(XAIE_EVENT_TRUE_MEM);
    comboEvents.push_back(XAIE_EVENT_USER_EVENT_0_MEM);
    return comboEvents;
  }

  /****************************************************************************
   * Configure group events (core modules only)
   ***************************************************************************/
  void configGroupEvents(XAie_DevInst* aieDevInst, const XAie_LocType loc,
                         const XAie_ModuleType mod, const module_type type, 
                         const xdp::built_in::MetricSet metricSet)
  {
    // Only needed for core module and metric sets that include DMA events
    if (!isDmaSet(metricSet) || (type != module_type::core))
      return;

    // Set masks for group events
    XAie_EventGroupControl(aieDevInst, loc, mod, XAIE_EVENT_GROUP_CORE_PROGRAM_FLOW_CORE, 
                           GROUP_CORE_FUNCTIONS_MASK);
    XAie_EventGroupControl(aieDevInst, loc, mod, XAIE_EVENT_GROUP_CORE_STALL_CORE, 
                           GROUP_CORE_STALL_MASK);
    XAie_EventGroupControl(aieDevInst, loc, mod, XAIE_EVENT_GROUP_STREAM_SWITCH_CORE, 
                           GROUP_STREAM_SWITCH_RUNNING_MASK);
  }

  /****************************************************************************
   * Configure event selection (memory tiles only)
   ***************************************************************************/
  void configEventSelections(XAie_DevInst* aieDevInst, const XAie_LocType loc,
                             const xdp::module_type type, 
                             const xdp::built_in::MemTileMetricSet metricSet,
                             const uint8_t channel0, const uint8_t channel1)
  {
    if (type != xdp::module_type::mem_tile)
      return;

    XAie_DmaDirection dmaDir = ((metricSet == xdp::built_in::MemTileMetricSet::INPUT_CHANNELS) ||
                                (metricSet == xdp::built_in::MemTileMetricSet::INPUT_CHANNELS_STALLS))
                                   ? DMA_S2MM
                                   : DMA_MM2S;
    XAie_EventSelectDmaChannel(aieDevInst, loc, 0, dmaDir, channel0);
    XAie_EventSelectDmaChannel(aieDevInst, loc, 1, dmaDir, channel1);
  }

  /****************************************************************************
   * Configure edge detection events
   ***************************************************************************/
  void configEdgeEvents(XAie_DevInst* aieDevInst, const tile_type& tile, const module_type type,
                        const XAie_Events event, const uint8_t channel,
                        const xdp::built_in::MetricSet metricSet,
                        const xdp::built_in::MemTileMetricSet memTileMetricSet)
  {
    if ((event != XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM_TILE)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM_TILE)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_0_MEM)
        && (event != XAIE_EVENT_EDGE_DETECTION_EVENT_1_MEM))
      return;
    
    // Catch memory tiles
    if (type == module_type::mem_tile) {
      // Event is DMA_S2MM_Sel0_stream_starvation or DMA_MM2S_Sel0_stalled_lock
      uint16_t eventNum = isInputSet(type, metricSet, memTileMetricSet)
          ? EVENT_MEM_TILE_DMA_S2MM_SEL0_STREAM_STARVATION
          : EVENT_MEM_TILE_DMA_MM2S_SEL0_STALLED_LOCK;

      // Register Edge_Detection_event_control
      // 26    Event 1 triggered on falling edge
      // 25    Event 1 triggered on rising edge
      // 23:16 Input event for edge event 1
      // 10    Event 0 triggered on falling edge
      //  9    Event 0 triggered on rising edge
      //  7:0  Input event for edge event 0
      uint32_t edgeEventsValue = (1 << 26) + (eventNum << 16) + (1 << 9) + eventNum;

      auto tileOffset = _XAie_GetTileAddr(aieDevInst, tile.row, tile.col);
      XAie_Write32(aieDevInst, tileOffset + AIE_OFFSET_EDGE_CONTROL_MEM_TILE, 
                   edgeEventsValue);
      return;
    }

    // Below is AIE tile support
    
    // Event is DMA_MM2S_stalled_lock or DMA_S2MM_stream_starvation
    uint16_t eventNum = isInputSet(type, metricSet, memTileMetricSet)
        ? ((channel == 0) ? EVENT_MEM_DMA_MM2S_0_STALLED_LOCK
                          : EVENT_MEM_DMA_MM2S_1_STALLED_LOCK)
        : ((channel == 0) ? EVENT_MEM_DMA_S2MM_0_STREAM_STARVATION
                          : EVENT_MEM_DMA_S2MM_1_STREAM_STARVATION);

    // Register Edge_Detection_event_control
    // 26    Event 1 triggered on falling edge
    // 25    Event 1 triggered on rising edge
    // 23:16 Input event for edge event 1
    // 10    Event 0 triggered on falling edge
    //  9    Event 0 triggered on rising edge
    //  7:0  Input event for edge event 0
    uint32_t edgeEventsValue = (1 << 26) + (eventNum << 16) + (1 << 9) + eventNum;

    xrt_core::message::send(severity_level::debug, "XRT", 
        "Configuring AIE tile edge events to detect rise and fall of event " 
        + std::to_string(eventNum));

    auto tileOffset = _XAie_GetTileAddr(aieDevInst, tile.row, tile.col);
    XAie_Write32(aieDevInst, tileOffset + AIE_OFFSET_EDGE_CONTROL_MEM, 
                 edgeEventsValue);
  }

  /****************************************************************************
   * Configure trace start on graph iteration
   ***************************************************************************/
  bool configStartIteration(xaiefal::XAieMod& core, EventConfiguration& config,
                            const xdp::built_in::TraceInputConfiguration* params)
  {
    XAie_ModuleType mod = XAIE_CORE_MOD;
    // Count up by 1 for every iteration
    auto pc = core.perfCounter();
    if (pc->initialize(mod, XAIE_EVENT_INSTR_EVENT_0_CORE, mod, 
                       XAIE_EVENT_INSTR_EVENT_0_CORE) != XAIE_OK)
      return false;
    if (pc->reserve() != XAIE_OK)
      return false;
    pc->changeThreshold(params->iterationCount);
    XAie_Events counterEvent;
    pc->getCounterEvent(mod, counterEvent);
    // Reset when done counting
    pc->changeRstEvent(mod, counterEvent);
    if (pc->start() != XAIE_OK)
      return false;

    config.coreTraceStartEvent = counterEvent;

    return true;
  }

  /****************************************************************************
   * Configure delay for trace start event
   ***************************************************************************/
  bool configStartDelay(xaiefal::XAieMod& core, EventConfiguration& config,
                        const xdp::built_in::TraceInputConfiguration* params)
  {
    if (!params->useDelay)
      return false;

    // This algorithm daisy chains counters to get an effective 64 bit delay
    // counterLow -> counterHigh -> trace start
    uint32_t delayCyclesHigh = 0;
    uint32_t delayCyclesLow = 0;
    XAie_ModuleType mod = XAIE_CORE_MOD;

    if (!params->useOneDelayCounter) {
      // ceil(x/y) where x and y are  positive integers
      delayCyclesHigh = static_cast<uint32_t>(1 + ((params->delayCycles - 1) / std::numeric_limits<uint32_t>::max()));
      delayCyclesLow = static_cast<uint32_t>(params->delayCycles / delayCyclesHigh);
    } else {
      delayCyclesLow = static_cast<uint32_t>(params->delayCycles);
    }

    // Configure lower 32 bits
    auto pc = core.perfCounter();
    if (pc->initialize(mod, XAIE_EVENT_ACTIVE_CORE, mod, XAIE_EVENT_DISABLED_CORE) != XAIE_OK)
      return false;
    if (pc->reserve() != XAIE_OK)
      return false;
    pc->changeThreshold(delayCyclesLow);
    XAie_Events counterEvent;
    pc->getCounterEvent(mod, counterEvent);
    // Reset when done counting
    pc->changeRstEvent(mod, counterEvent);
    if (pc->start() != XAIE_OK)
      return false;

    // Configure upper 32 bits if necessary
    // Use previous counter to start a new counter
    if (!params->useOneDelayCounter && delayCyclesHigh) {
      auto pc = core.perfCounter();
      // Count by 1 when previous counter generates event
      if (pc->initialize(mod, counterEvent, mod, counterEvent) != XAIE_OK)
        return false;
      if (pc->reserve() != XAIE_OK)
        return false;
      pc->changeThreshold(delayCyclesHigh);
      pc->getCounterEvent(mod, counterEvent);
      // Reset when done counting
      pc->changeRstEvent(mod, counterEvent);
      if (pc->start() != XAIE_OK)
        return false;
    }

    config.coreTraceStartEvent = counterEvent;

    return true;
  }

}  // namespace
