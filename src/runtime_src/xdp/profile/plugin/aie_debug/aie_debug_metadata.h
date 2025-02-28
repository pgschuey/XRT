/**
 * Copyright (C) 2022-2025 Advanced Micro Devices, Inc. - All rights reserved
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

#ifndef AIE_DEBUG_METADATA_H
#define AIE_DEBUG_METADATA_H

#include <boost/property_tree/ptree.hpp>
#include <memory>
#include <vector>
#include <set>

#include "core/common/device.h"
#include "core/common/message.h"
#include "core/include/xrt/xrt_hw_context.h"
#include "xdp/config.h"
#include "xdp/profile/database/static_info/aie_constructs.h"
#include "xdp/profile/database/static_info/aie_util.h"
#include "xdp/profile/database/static_info/filetypes/base_filetype_impl.h"
#include "xdp/profile/plugin/aie_profile/aie_profile_defs.h"
#include "xdp/profile/plugin/aie_debug/generations/aie1_registers.h"
#include "xdp/profile/plugin/aie_debug/generations/aie2_registers.h"
#include "xdp/profile/plugin/aie_debug/generations/aie2ps_registers.h"
#include "xdp/profile/plugin/vp_base/vp_base_plugin.h"

extern "C" {
#include <xaiengine.h>
#include <xaiengine/xaiegbl_params.h>
}
#define NUMBEROFMODULES 4
namespace xdp {

// Forward declarations
class BaseReadableTile;
class UsedRegisters;

class AieDebugMetadata {
  public:
    AieDebugMetadata(uint64_t deviceID, void* handle);

    void parseMetrics();

    module_type getModuleType(int mod) {return moduleTypes[mod];}
    uint64_t getDeviceID() {return deviceID;}
    void* getHandle() {return handle;}

    std::map<tile_type, std::string> getConfigMetrics(const int module) {
      return configMetrics[module];
    }
    std::vector<std::pair<tile_type, std::string>> getConfigMetricsVec(const int module) {
      return {configMetrics[module].begin(), configMetrics[module].end()};
    }

    std::map<module_type, std::vector<uint64_t>>& getRegisterValues() {
      return parsedRegValues;
    }

    bool aieMetadataEmpty() const {return (metadataReader == nullptr);}
    xdp::aie::driver_config getAIEConfigMetadata() {return metadataReader->getDriverConfig();}

    uint8_t getAIETileRowOffset() const {
      return (metadataReader == nullptr) ? 0 : metadataReader->getAIETileRowOffset();
    }
    int getHardwareGen() const {
      return (metadataReader == nullptr) ? 0 : metadataReader->getHardwareGeneration();
    }

    int getNumModules() {return NUM_MODULES;}
    xrt::hw_context getHwContext() {return hwContext;}
    void setHwContext(xrt::hw_context c) {
      hwContext = std::move(c);
    }

    std::string lookupRegisterName(uint64_t regVal, module_type mod);
    uint64_t lookupRegisterAddr(std::string regName, module_type mod);
    //std::map<uint64_t, uint32_t> lookupRegisterSizes();
    uint32_t lookupRegisterSizes(uint64_t regVal,int mod);

  private:
    std::vector<uint64_t> stringToRegList(std::string stringEntry, module_type t);
    std::vector<std::string> getSettingsVector(std::string settingsString);

  private:
    // Currently supporting Core, Memory, Interface Tiles, and Memory Tiles
    static constexpr int NUM_MODULES = 4;
    const std::vector<std::string> moduleNames =
      {"aie", "aie_memory", "interface_tile", "memory_tile"};
    const module_type moduleTypes[NUM_MODULES] =
      {module_type::core, module_type::dma, module_type::shim, module_type::mem_tile};

    void* handle;
    uint64_t deviceID;
    xrt::hw_context hwContext;
    std::vector<std::map<tile_type, std::string>> configMetrics;
    std::map<module_type, std::vector<uint64_t>> parsedRegValues;
    const aie::BaseFiletypeImpl* metadataReader = nullptr;

    // List of AIE HW generation-specific registers
    std::unique_ptr<UsedRegisters> usedRegisters;
};

/*****************************************************************
The BaseReadableTile is created to simplify the retrieving of value at
each tile. This class encapsulates all the data (row, col, list of registers
to read) pertaining to a particuar tile, for easy extraction of tile by tile data.
****************************************************************** */
class BaseReadableTile {
  public:
    //virtual void readValues(XAie_DevInst* aieDevInst, std::map<uint64_t, uint32_t>* lookupRegAddrToSizeMap)=0;
    virtual void readValues(XAie_DevInst* aieDevInst, std::shared_ptr<AieDebugMetadata> metadata)=0;

    void setTileOffset(uint64_t offset) {tileOffset = offset;}
    void addOffsetName(uint64_t rel, std::string name,module_type mod) {
      if (mod == module_type::core) {
        coreRelativeOffsets.push_back(rel);
        coreRegisterNames.push_back(name);
      }
      else if (mod == module_type::dma) {
        memoryRelativeOffsets.push_back(rel);
        memoryRegisterNames.push_back(name);
      }
      else if (mod == module_type::shim) {
        shimRelativeOffsets.push_back(rel);
        shimRegisterNames.push_back(name);
      }
      else if (mod == module_type::mem_tile) {
        memTileRelativeOffsets.push_back(rel);
        memTileRegisterNames.push_back(name);
      }
    }

    void printValues(uint32_t deviceID, VPDatabase* db) {
      int i = 0;
      std::vector<uint64_t>* addrVectors[] = {&coreRelativeOffsets, &memoryRelativeOffsets, &shimRelativeOffsets, &memTileRelativeOffsets};
      std::vector<xdp::aie::AieDebugValue>* valueVectors[] = {&coreValues, &memoryValues, &shimValues, &memTileValues};
      std::vector<std::string>* nameVectors[]={&coreRegisterNames,&memoryRegisterNames,&shimRegisterNames,&memTileRegisterNames};
      /*
      for (auto& offset : relativeOffsets) {
        db->getDynamicInfo().addAIEDebugSample(deviceID, col, row, values[i],
                                               offset, registerNames[i]);
        i++;
      }
      */
     for (int i = 0; i < NUMBEROFMODULES; ++i) {
        for (int j = 0; j < addrVectors[i]->size(); ++j) {
          db->getDynamicInfo().addAIEDebugSample(deviceID, col, row, (*valueVectors[i])[j],
                                               (*addrVectors[i])[j], (*nameVectors[i])[j]);
        }
     }
    }

  public:
    uint8_t col;
    uint8_t row;
    uint64_t tileOffset;
    //std::vector<uint32_t> values;
    //std::vector<xdp::aie::AieDebugValue> values;
    std::vector<xdp::aie::AieDebugValue> coreValues;
    std::vector<xdp::aie::AieDebugValue> memoryValues;
    std::vector<xdp::aie::AieDebugValue> shimValues;
    std::vector<xdp::aie::AieDebugValue> memTileValues;
    //std::vector<uint64_t> relativeOffsets;
    std::vector<uint64_t> coreRelativeOffsets;
    std::vector<uint64_t> memoryRelativeOffsets;
    std::vector<uint64_t> shimRelativeOffsets;
    std::vector<uint64_t> memTileRelativeOffsets;
    //std::vector<std::string> registerNames;
    std::vector<std::string> coreRegisterNames;
    std::vector<std::string> memoryRegisterNames;
    std::vector<std::string> shimRegisterNames;
    std::vector<std::string> memTileRegisterNames;
};

/*************************************************************************************
The class UsedRegisters is what gives us AIE hw generation specific data. The base class
has virtual functions which populate the correct registers and their addresses according
to the AIE hw generation in the derived classes. Thus we can dynamically populate the
correct registers and their addresses at runtime.
**************************************************************************************/
class UsedRegisters {
  public:
    UsedRegisters() {
      populateRegNameToValueMap();
      populateRegValueToNameMap();
      populateRegAddrToSizeMap();
    }
    virtual ~UsedRegisters() {
      core_addresses.clear();
      memory_addresses.clear();
      interface_addresses.clear();
      memory_tile_addresses.clear();
      regNameToValue.clear();
      coreRegValueToName.clear();
      memoryRegValueToName.clear();
      memTileRegValueToName.clear();
      shimRegValueToName.clear();
      //regAddrToSize.clear();
      coreRegAddrToSize.clear();
      memoryRegAddrToSize.clear();
      shimRegAddrToSize.clear();
      memTileRegAddrToSize.clear();

    }

    std::set<uint64_t> getCoreAddresses() {
      return core_addresses;
    }
    std::set<uint64_t> getMemoryAddresses() {
      return memory_addresses;
    }
    std::set<uint64_t> getInterfaceAddresses() {
      return interface_addresses;
    }
    std::set<uint64_t> getMemoryTileAddresses() {
      return memory_tile_addresses;
    }

    std::string getRegisterName(uint64_t regVal,module_type mod) {
      std::map<uint64_t,std::string>::iterator itr;
      if (mod == module_type::core) {
        itr = coreRegValueToName.find(regVal);
        if (itr != coreRegValueToName.end())
          return itr->second;
      }
      else if (mod == module_type::dma) {
        itr = memoryRegValueToName.find(regVal);
        if (itr != memoryRegValueToName.end())
          return itr->second;
      }
      else if (mod == module_type::shim) {
        itr = shimRegValueToName.find(regVal);
        if (itr != shimRegValueToName.end())
          return itr->second;
      }
      else if (mod == module_type::mem_tile) {
        itr = memTileRegValueToName.find(regVal);
        if (itr != memTileRegValueToName.end())
          return itr->second;
      }
      std::stringstream ss;
      ss << "0x" << std::hex << std::uppercase << regVal;
      return ss.str();
      }



    uint64_t getRegisterAddr(std::string regName) {
      auto itr=regNameToValue.find(regName);
      return (itr != regNameToValue.end()) ? itr->second : 0;
    }

    uint32_t getRegAddrToSize(uint64_t regVal,int mod) {
      std::map<uint64_t, uint32_t>::iterator itr;
     if (mod == 0) {
        itr = coreRegAddrToSize.find(regVal);
        if (itr != coreRegAddrToSize.end())
          return itr->second;
     }
     else if (mod == 1) {
        itr = memoryRegAddrToSize.find(regVal);
        if (itr != memoryRegAddrToSize.end())
          return itr->second;

     }
     else if (mod == 2) {
        itr = shimRegAddrToSize.find(regVal);
        if (itr != shimRegAddrToSize.end())
          return itr->second;
     }
     else if (mod == 3) {
        itr = memTileRegAddrToSize.find(regVal);
        if (itr != memTileRegAddrToSize.end())
          return itr->second;
     }
    return 32;
      //return regAddrToSize;
    }

    virtual void populateProfileRegisters() {};
    virtual void populateTraceRegisters() {};
    virtual void populateRegNameToValueMap() {};
    virtual void populateRegValueToNameMap() {};
    virtual void populateRegAddrToSizeMap() {};

    void populateAllRegisters() {
      populateProfileRegisters();
      populateTraceRegisters();
    }

  protected:
    std::set<uint64_t> core_addresses;
    std::set<uint64_t> memory_addresses;
    std::set<uint64_t> interface_addresses;
    std::set<uint64_t> memory_tile_addresses;
    std::map<std::string, uint64_t> regNameToValue;
    std::map<uint64_t, std::string> coreRegValueToName;
    std::map<uint64_t, std::string> memoryRegValueToName;
    std::map<uint64_t, std::string> shimRegValueToName;
    std::map<uint64_t, std::string> memTileRegValueToName;
    std::map<uint64_t, std::string> ucRegValueToName;
    std::map<uint64_t, std::string> npiRegValueToName;
    //std::map<uint64_t, uint32_t> regAddrToSize;
    std::map<uint64_t, uint32_t> coreRegAddrToSize;
    std::map<uint64_t, uint32_t> memoryRegAddrToSize;
    std::map<uint64_t, uint32_t> shimRegAddrToSize;
    std::map<uint64_t, uint32_t> memTileRegAddrToSize;
    std::map<uint64_t, uint32_t> ucRegAddrToSize ;
    std::map<uint64_t, uint32_t> npiRegAddrToSize;
};

/*************************************************************************************
 AIE1 Registers
 *************************************************************************************/
class AIE1UsedRegisters : public UsedRegisters {
public:
  AIE1UsedRegisters() {
    populateRegNameToValueMap();
    populateRegValueToNameMap();
    populateRegAddrToSizeMap();
  }
  virtual ~AIE1UsedRegisters() {}

  void populateProfileRegisters();

  void populateTraceRegisters() ;
void populateRegNameToValueMap();



void populateRegValueToNameMap() ;

void populateRegAddrToSizeMap() ;

};

/*************************************************************************************
 AIE2 Registers
 *************************************************************************************/
class AIE2UsedRegisters : public UsedRegisters {
public:
  AIE2UsedRegisters() {
    populateRegNameToValueMap();
    populateRegValueToNameMap();
    populateRegAddrToSizeMap();
  }
 // ~AIE2UsedRegisters() = default;
  virtual ~AIE2UsedRegisters() {}

  void populateProfileRegisters() ;

  void populateTraceRegisters() ;

void populateRegNameToValueMap() ;

  /*
  void populateRegValueToNameMap() {
    regValueToName=  {
      {0x000940a0, "mem_event_broadcast_b_block_west_set"},
      {0x0001d220, "shim_dma_s2mm_status_0"},
      {0x0001d224, "shim_dma_s2mm_status_1"},
      {0x000a061c, "mem_dma_s2mm_3_start_queue"},
      {0x000c0230, "mem_lock35_value"},
      {0x000a0614, "mem_dma_s2mm_2_start_queue"},
      {0x000340f4, "shim_timer_trig_event_high_value"},
      {0x00034068, "shim_event_broadcast_a_block_west_value"},
      {0x0001d10c, "mm_dma_bd8_3"},
      {0x000b0228, "mem_stream_switch_slave_dma_2_slot2"},
      {0x0001d104, "mm_dma_bd8_1"},
      {0x000140e0, "shim_lock14_value"},
      {0x0001d100, "mm_dma_bd8_0"},
      {0x0003f2d4, "cm_stream_switch_slave_west_2_slot1"},
      {0x0003f2d0, "cm_stream_switch_slave_west_2_slot0"},
      {0x0003f2dc, "cm_stream_switch_slave_west_2_slot3"},
      {0x0003f2d8, "cm_stream_switch_slave_west_2_slot2"},
      {0x000c0000, "mem_lock0_value"},
      {0x000c0130, "mem_lock19_value"},
      {0x000a0254, "mem_dma_bd18_5"},
      {0x000a0250, "mem_dma_bd18_4"},
      {0x000a025c, "mem_dma_bd18_7"},
      {0x000a0258, "mem_dma_bd18_6"},
      {0x000a0244, "mem_dma_bd18_1"},
      {0x000a0240, "mem_dma_bd18_0"},
      {0x000a024c, "mem_dma_bd18_3"},
      {0x000a0248, "mem_dma_bd18_2"},
      {0x000c02c0, "mem_lock44_value"},
      {0x0001de0c, "mm_dma_s2mm_1_start_queue"},
      {0x00034508, "cm_event_group_core_stall_enable"},
      {0x00094214, "mem_event_status5"},
      {0x00094200, "mem_event_status0"},
      {0x00094204, "mem_event_status1"},
      {0x00094208, "mem_event_status2"},
      {0x0009420c, "mem_event_status3"},
      {0x0003ff38, "cm_stream_switch_adaptive_clock_gate_abort_period"},
      {0x00094500, "mem_event_group_0_enable"},
      {0x0003f104, "cm_stream_switch_slave_config_dma_0"},
      {0x000c01c0, "mem_lock28_value"},
      {0x000a0624, "mem_dma_s2mm_4_start_queue"},
      {0x00014514, "mm_event_group_error_enable"},
      {0x0001f0b0, "mm_lock11_value"},
      {0x000c00c0, "mem_lock12_value"},
      {0x0001d200, "shim_dma_s2mm_0_ctrl"},
      {0x00014074, "mm_event_broadcast_block_north_clr"},
      {0x0001d084, "mm_dma_bd4_1"},
      {0x000340f0, "cm_timer_trig_event_low_value"},
      {0x00031080, "shim_performance_counter0_event_value"},
      {0x000b02d0, "mem_stream_switch_slave_north_0_slot0"},
      {0x000b02d4, "mem_stream_switch_slave_north_0_slot1"},
      {0x000b02d8, "mem_stream_switch_slave_north_0_slot2"},
      {0x000b02dc, "mem_stream_switch_slave_north_0_slot3"},
      {0x0003f278, "cm_stream_switch_slave_south_2_slot2"},
      {0x0003f27c, "cm_stream_switch_slave_south_2_slot3"},
      {0x0003f270, "cm_stream_switch_slave_south_2_slot0"},
      {0x0003f274, "cm_stream_switch_slave_south_2_slot1"},
      {0x0001f050, "mm_lock5_value"},
      {0x00060010, "cm_module_reset_control"},
      {0x000940f8, "mem_timer_low"},
      {0x0001d1f0, "mm_dma_bd15_4"},
      {0x0001d1f4, "mm_dma_bd15_5"},
      {0x000a063c, "mem_dma_mm2s_1_start_queue"},
      {0x0001d1e0, "mm_dma_bd15_0"},
      {0x0001d1e4, "mm_dma_bd15_1"},
      {0x0001d1e8, "mm_dma_bd15_2"},
      {0x0001d1ec, "mm_dma_bd15_3"},
      {0x00094400, "mem_combo_event_inputs"},
      {0x0003f100, "cm_stream_switch_slave_config_aie_core0"},
      {0x000c0200, "mem_lock32_value"},
      {0x000340d8, "shim_trace_status"},
      {0x00014070, "mm_event_broadcast_block_north_set"},
      {0x0001d160, "mm_dma_bd11_0"},
      {0x0001d164, "mm_dma_bd11_1"},
      {0x0001d168, "mm_dma_bd11_2"},
      {0x0001d16c, "mm_dma_bd11_3"},
      {0x0001d170, "mm_dma_bd11_4"},
      {0x0001d174, "mm_dma_bd11_5"},
      {0x0001d1b0, "mm_dma_bd13_4"},
      {0x0001d1b4, "mm_dma_bd13_5"},
      {0x0001d1a8, "mm_dma_bd13_2"},
      {0x0001d1ac, "mm_dma_bd13_3"},
      {0x0001d1a0, "mm_dma_bd13_0"},
      {0x0001d1a4, "mm_dma_bd13_1"},
      {0x0009422c, "mem_reserved3"},
      {0x00094228, "mem_reserved2"},
      {0x00094224, "mem_reserved1"},
      {0x00094220, "mem_reserved0"},
      {0x000140e0, "mm_trace_event0"},
      {0x000140e4, "mm_trace_event1"},
      {0x000a0230, "mem_dma_bd17_4"},
      {0x000a0234, "mem_dma_bd17_5"},
      {0x000a0228, "mem_dma_bd17_2"},
      {0x000a022c, "mem_dma_bd17_3"},
      {0x000a0220, "mem_dma_bd17_0"},
      {0x000a0224, "mem_dma_bd17_1"},
      {0x000a01e0, "mem_dma_bd15_0"},
      {0x000a01e4, "mem_dma_bd15_1"},
      {0x000a01e8, "mem_dma_bd15_2"},
      {0x000a01ec, "mem_dma_bd15_3"},
      {0x000a01f0, "mem_dma_bd15_4"},
      {0x000a01f4, "mem_dma_bd15_5"},
      {0x000a01f8, "mem_dma_bd15_6"},
      {0x000a01fc, "mem_dma_bd15_7"},
      {0x000a06dc, "mem_dma_s2mm_fot_count_fifo_pop_5"},
      {0x000a06d8, "mem_dma_s2mm_fot_count_fifo_pop_4"},
      {0x000a06cc, "mem_dma_s2mm_fot_count_fifo_pop_1"},
      {0x0001f0d0, "mm_lock13_value"},
      {0x000a06d4, "mem_dma_s2mm_fot_count_fifo_pop_3"},
      {0x000a06d0, "mem_dma_s2mm_fot_count_fifo_pop_2"},
      {0x000b0800, "mem_stream_switch_deterministic_merge_arb0_slave0_1"},
      {0x0003f234, "cm_stream_switch_slave_tile_ctrl_slot1"},
      {0x0003f238, "cm_stream_switch_slave_tile_ctrl_slot2"},
      {0x0003f23c, "cm_stream_switch_slave_tile_ctrl_slot3"},
      {0x0003f14c, "cm_stream_switch_slave_config_east_0"},
      {0x0003f150, "cm_stream_switch_slave_config_east_1"},
      {0x0003f154, "cm_stream_switch_slave_config_east_2"},
      {0x0003f158, "cm_stream_switch_slave_config_east_3"},
      {0x000a01a8, "mem_dma_bd13_2"},
      {0x000a01ac, "mem_dma_bd13_3"},
      {0x000a01a0, "mem_dma_bd13_0"},
      {0x000a01a4, "mem_dma_bd13_1"},
      {0x000a01b8, "mem_dma_bd13_6"},
      {0x000a01bc, "mem_dma_bd13_7"},
      {0x000a01b0, "mem_dma_bd13_4"},
      {0x000a01b4, "mem_dma_bd13_5"},
      {0x000a0170, "mem_dma_bd11_4"},
      {0x000a0174, "mem_dma_bd11_5"},
      {0x000a0178, "mem_dma_bd11_6"},
      {0x000a017c, "mem_dma_bd11_7"},
      {0x000a0160, "mem_dma_bd11_0"},
      {0x000a0164, "mem_dma_bd11_1"},
      {0x000a0168, "mem_dma_bd11_2"},
      {0x000a016c, "mem_dma_bd11_3"},
      {0x000940fc, "mem_timer_high"},
      {0x0003f374, "cm_stream_switch_slave_aie_trace_slot1"},
      {0x0003f370, "cm_stream_switch_slave_aie_trace_slot0"},
      {0x0003f37c, "cm_stream_switch_slave_aie_trace_slot3"},
      {0x0003f378, "cm_stream_switch_slave_aie_trace_slot2"},
      {0x00031524, "cm_performance_counter1"},
      {0x00031520, "cm_performance_counter0"},
      {0x0003152c, "cm_performance_counter3"},
      {0x00031528, "cm_performance_counter2"},
      {0x000b0018, "mem_stream_switch_master_config_tile_ctrl"},
      {0x000940c8, "mem_event_broadcast_b_block_east_value"},
      {0x0003f2bc, "cm_stream_switch_slave_west_0_slot3"},
      {0x0003f2b8, "cm_stream_switch_slave_west_0_slot2"},
      {0x0003f2b4, "cm_stream_switch_slave_west_0_slot1"},
      {0x0003f2b0, "cm_stream_switch_slave_west_0_slot0"},
      {0x000940d4, "mem_trace_control1"},
      {0x000940d0, "mem_trace_control0"},
      {0x0001f030, "mm_lock3_value"},
      {0x00014028, "mm_event_broadcast6"},
      {0x0001402c, "mm_event_broadcast7"},
      {0x00034000, "cm_timer_control"},
      {0x00014018, "mm_event_broadcast2"},
      {0x00014040, "shim_lock4_value"},
      {0x0001401c, "mm_event_broadcast3"},
      {0x0001d0cc, "mm_dma_bd6_3"},
      {0x00014014, "mm_event_broadcast1"},
      {0x00014090, "shim_lock9_value"},
      {0x0003ff30, "cm_tile_control_packet_handler_status"},
      {0x00034030, "cm_event_broadcast8"},
      {0x00034034, "cm_event_broadcast9"},
      {0x0003f368, "cm_stream_switch_slave_east_3_slot2"},
      {0x00034024, "cm_event_broadcast5"},
      {0x0003f360, "cm_stream_switch_slave_east_3_slot0"},
      {0x0003f364, "cm_stream_switch_slave_east_3_slot1"},
      {0x00034010, "cm_event_broadcast0"},
      {0x00034014, "cm_event_broadcast1"},
      {0x00034018, "cm_event_broadcast2"},
      {0x0003401c, "cm_event_broadcast3"},
      {0x00091084, "mem_performance_counter1_event_value"},
      {0x000b0268, "mem_stream_switch_slave_tile_ctrl_slot2"},
      {0x000b026c, "mem_stream_switch_slave_tile_ctrl_slot3"},
      {0x0001d158, "shim_dma_bd10_6"},
      {0x0003f02c, "cm_stream_switch_master_config_west2"},
      {0x0003f030, "cm_stream_switch_master_config_west3"},
      {0x0003f024, "cm_stream_switch_master_config_west0"},
      {0x0003f028, "cm_stream_switch_master_config_west1"},
      {0x0003f144, "cm_stream_switch_slave_config_north_2"},
      {0x000a05c0, "mem_dma_bd46_0"},
      {0x000a05c4, "mem_dma_bd46_1"},
      {0x000a05c8, "mem_dma_bd46_2"},
      {0x000a05cc, "mem_dma_bd46_3"},
      {0x000a05d0, "mem_dma_bd46_4"},
      {0x000a05d4, "mem_dma_bd46_5"},
      {0x000a05d8, "mem_dma_bd46_6"},
      {0x000a05dc, "mem_dma_bd46_7"},
      {0x00014100, "mm_watchpoint0"},
      {0x00014104, "mm_watchpoint1"},
      {0x00094068, "mem_event_broadcast_a_block_west_value"},
      {0x0001de18, "mm_dma_mm2s_1_ctrl"},
      {0x000140f0, "mm_timer_trig_event_low_value"},
      {0x000b0808, "mem_stream_switch_deterministic_merge_arb0_ctrl"},
      {0x0003f28c, "cm_stream_switch_slave_south_3_slot3"},
      {0x0003f288, "cm_stream_switch_slave_south_3_slot2"},
      {0x000b0030, "mem_stream_switch_master_config_north1"},
      {0x000b002c, "mem_stream_switch_master_config_north0"},
      {0x000b0040, "mem_stream_switch_master_config_north5"},
      {0x000b003c, "mem_stream_switch_master_config_north4"},
      {0x000140d4, "mm_trace_control1"},
      {0x000140d0, "mm_trace_control0"},
      {0x00034078, "shim_event_broadcast_a_block_north_value"},
      {0x00034054, "shim_event_broadcast_a_block_south_clr"},
      {0x00094074, "mem_event_broadcast_a_block_north_clr"},
      {0x00014064, "mm_event_broadcast_block_west_clr"},
      {0x000c02e0, "mem_lock46_value"},
      {0x0001d114, "mm_dma_bd8_5"},
      {0x0001d110, "mm_dma_bd8_4"},
      {0x000b022c, "mem_stream_switch_slave_dma_2_slot3"},
      {0x0001d108, "mm_dma_bd8_2"},
      {0x000b0224, "mem_stream_switch_slave_dma_2_slot1"},
      {0x000b0220, "mem_stream_switch_slave_dma_2_slot0"},
      {0x0001de00, "mm_dma_s2mm_0_ctrl"},
      {0x0003158c, "cm_performance_counter3_event_value"},
      {0x00091088, "mem_performance_counter2_event_value"},
      {0x0003f108, "cm_stream_switch_slave_config_dma_1"},
      {0x000a052c, "mem_dma_bd41_3"},
      {0x000a0528, "mem_dma_bd41_2"},
      {0x000a0524, "mem_dma_bd41_1"},
      {0x000a0520, "mem_dma_bd41_0"},
      {0x000a053c, "mem_dma_bd41_7"},
      {0x000a0538, "mem_dma_bd41_6"},
      {0x000a0534, "mem_dma_bd41_5"},
      {0x000a0530, "mem_dma_bd41_4"},
      {0x000a0574, "mem_dma_bd43_5"},
      {0x000a0570, "mem_dma_bd43_4"},
      {0x000a057c, "mem_dma_bd43_7"},
      {0x000a0578, "mem_dma_bd43_6"},
      {0x000a0564, "mem_dma_bd43_1"},
      {0x000a0560, "mem_dma_bd43_0"},
      {0x000a056c, "mem_dma_bd43_3"},
      {0x000a0568, "mem_dma_bd43_2"},
      {0x000c0350, "mem_lock53_value"},
      {0x000140b0, "shim_lock11_value"},
      {0x000b0244, "mem_stream_switch_slave_dma_4_slot1"},
      {0x000b0240, "mem_stream_switch_slave_dma_4_slot0"},
      {0x000b024c, "mem_stream_switch_slave_dma_4_slot3"},
      {0x000b0248, "mem_stream_switch_slave_dma_4_slot2"},
      {0x000a05bc, "mem_dma_bd45_7"},
      {0x000a05b8, "mem_dma_bd45_6"},
      {0x000a05b4, "mem_dma_bd45_5"},
      {0x000a05b0, "mem_dma_bd45_4"},
      {0x000a05ac, "mem_dma_bd45_3"},
      {0x000a05a8, "mem_dma_bd45_2"},
      {0x000a05a4, "mem_dma_bd45_1"},
      {0x000a05a0, "mem_dma_bd45_0"},
      {0x00014050, "shim_lock5_value"},
      {0x00092110, "mem_ecc_scrubbing_event"},
      {0x00034084, "shim_event_broadcast_a_block_east_clr"},
      {0x0003f290, "cm_stream_switch_slave_south_4_slot0"},
      {0x0003f294, "cm_stream_switch_slave_south_4_slot1"},
      {0x0003f298, "cm_stream_switch_slave_south_4_slot2"},
      {0x0003f29c, "cm_stream_switch_slave_south_4_slot3"},
      {0x00094520, "mem_event_group_user_event_enable"},
      {0x00014000, "shim_lock0_value"},
      {0x00092000, "mem_checkbit_error_generation"},
      {0x00011000, "mm_performance_control0"},
      {0x00011008, "mm_performance_control1"},
      {0x0003f134, "cm_stream_switch_slave_config_west_2"},
      {0x0003f138, "cm_stream_switch_slave_config_west_3"},
      {0x0003f12c, "cm_stream_switch_slave_config_west_0"},
      {0x0003f130, "cm_stream_switch_slave_config_west_1"},
      {0x00014518, "mm_event_group_broadcast_enable"},
      {0x00034070, "shim_event_broadcast_a_block_north_set"},
      {0x00094210, "mem_event_status4"},
      {0x0001d1c4, "shim_dma_bd14_1"},
      {0x000a010c, "mem_dma_bd8_3"},
      {0x000a0108, "mem_dma_bd8_2"},
      {0x000a0104, "mem_dma_bd8_1"},
      {0x000a0100, "mem_dma_bd8_0"},
      {0x000a011c, "mem_dma_bd8_7"},
      {0x000a0118, "mem_dma_bd8_6"},
      {0x000a0114, "mem_dma_bd8_5"},
      {0x000a0110, "mem_dma_bd8_4"},
      {0x000a00c4, "mem_dma_bd6_1"},
      {0x000a00c0, "mem_dma_bd6_0"},
      {0x000a00cc, "mem_dma_bd6_3"},
      {0x000a00c8, "mem_dma_bd6_2"},
      {0x000a00d4, "mem_dma_bd6_5"},
      {0x000a00d0, "mem_dma_bd6_4"},
      {0x000a00dc, "mem_dma_bd6_7"},
      {0x000a00d8, "mem_dma_bd6_6"},
      {0x000940b0, "mem_event_broadcast_b_block_north_set"},
      {0x00096040, "mem_cssd_trigger"},
      {0x0001d0f0, "mm_dma_bd7_4"},
      {0x0001d0f4, "mm_dma_bd7_5"},
      {0x0001d0e0, "mm_dma_bd7_0"},
      {0x0001d0e4, "mm_dma_bd7_1"},
      {0x0001d0e8, "mm_dma_bd7_2"},
      {0x0001d0ec, "mm_dma_bd7_3"},
      {0x000b02f8, "mem_stream_switch_slave_north_2_slot2"},
      {0x000b02fc, "mem_stream_switch_slave_north_2_slot3"},
      {0x000b02f0, "mem_stream_switch_slave_north_2_slot0"},
      {0x000b02f4, "mem_stream_switch_slave_north_2_slot1"},
      {0x0001d0a8, "mm_dma_bd5_2"},
      {0x0001d0ac, "mm_dma_bd5_3"},
      {0x0001d0a0, "mm_dma_bd5_0"},
      {0x0001d0a4, "mm_dma_bd5_1"},
      {0x0001d0b0, "mm_dma_bd5_4"},
      {0x0001d0b4, "mm_dma_bd5_5"},
      {0x00094518, "mem_event_group_error_enable"},
      {0x0001f020, "mm_lock2_value"},
      {0x00034060, "shim_event_broadcast_a_block_west_set"},
      {0x000a0028, "mem_dma_bd1_2"},
      {0x000a002c, "mem_dma_bd1_3"},
      {0x000a0020, "mem_dma_bd1_0"},
      {0x000a0024, "mem_dma_bd1_1"},
      {0x000a0038, "mem_dma_bd1_6"},
      {0x000a003c, "mem_dma_bd1_7"},
      {0x000a0030, "mem_dma_bd1_4"},
      {0x000a0034, "mem_dma_bd1_5"},
      {0x000340d0, "shim_trace_control0"},
      {0x000340d4, "shim_trace_control1"},
      {0x000b0f00, "mem_stream_switch_event_port_selection_0"},
      {0x000b0f04, "mem_stream_switch_event_port_selection_1"},
      {0x0001d068, "mm_dma_bd3_2"},
      {0x0001d06c, "mm_dma_bd3_3"},
      {0x0001d070, "mm_dma_bd3_4"},
      {0x0001d074, "mm_dma_bd3_5"},
      {0x00034074, "cm_event_broadcast_block_north_clr"},
      {0x0001d030, "mm_dma_bd1_4"},
      {0x0001d034, "mm_dma_bd1_5"},
      {0x0001d028, "mm_dma_bd1_2"},
      {0x0001d02c, "mm_dma_bd1_3"},
      {0x00032008, "cm_enable_events"},
      {0x0001d024, "mm_dma_bd1_1"},
      {0x000c0250, "mem_lock37_value"},
      {0x000b0110, "mem_stream_switch_slave_config_dma_4"},
      {0x000b0114, "mem_stream_switch_slave_config_dma_5"},
      {0x000b0100, "mem_stream_switch_slave_config_dma_0"},
      {0x000b0104, "mem_stream_switch_slave_config_dma_1"},
      {0x000b0108, "mem_stream_switch_slave_config_dma_2"},
      {0x00094508, "mem_event_group_dma_enable"},
      {0x00094028, "mem_event_broadcast6"},
      {0x0009402c, "mem_event_broadcast7"},
      {0x00094020, "mem_event_broadcast4"},
      {0x00094024, "mem_event_broadcast5"},
      {0x00094018, "mem_event_broadcast2"},
      {0x0009401c, "mem_event_broadcast3"},
      {0x00094010, "mem_event_broadcast0"},
      {0x00094014, "mem_event_broadcast1"},
      {0x000b0020, "mem_stream_switch_master_config_south1"},
      {0x000b001c, "mem_stream_switch_master_config_south0"},
      {0x000b0028, "mem_stream_switch_master_config_south3"},
      {0x000b0024, "mem_stream_switch_master_config_south2"},
      {0x000a0060, "mem_dma_bd3_0"},
      {0x000a0064, "mem_dma_bd3_1"},
      {0x000a0068, "mem_dma_bd3_2"},
      {0x000a006c, "mem_dma_bd3_3"},
      {0x00032004, "cm_core_status"},
      {0x0001f120, "mm_locks_overflow"},
      {0x000c0390, "mem_lock57_value"},
      {0x00034080, "cm_event_broadcast_block_east_set"},
      {0x0001df14, "mm_dma_mm2s_status_1"},
      {0x0001df10, "mm_dma_mm2s_status_0"},
      {0x000c0020, "mem_lock2_value"},
      {0x0003f240, "cm_stream_switch_slave_fifo_0_slot0"},
      {0x0001f080, "mm_lock8_value"},
      {0x00034518, "cm_event_group_stream_switch_enable"},
      {0x0001f000, "mm_lock0_value"},
      {0x000a0404, "mem_dma_bd32_1"},
      {0x000a0400, "mem_dma_bd32_0"},
      {0x000140d8, "mm_trace_status"},
      {0x000a0408, "mem_dma_bd32_2"},
      {0x000a0414, "mem_dma_bd32_5"},
      {0x000a0410, "mem_dma_bd32_4"},
      {0x000a041c, "mem_dma_bd32_7"},
      {0x000a0418, "mem_dma_bd32_6"},
      {0x000c0040, "mem_lock4_value"},
      {0x0003f204, "cm_stream_switch_slave_aie_core0_slot1"},
      {0x0003f200, "cm_stream_switch_slave_aie_core0_slot0"},
      {0x0003f20c, "cm_stream_switch_slave_aie_core0_slot3"},
      {0x0003f208, "cm_stream_switch_slave_aie_core0_slot2"},
      {0x0001451c, "mm_event_group_user_event_enable"},
      {0x00014000, "mm_timer_control"},
      {0x000a04dc, "mem_dma_bd38_7"},
      {0x000a04d8, "mem_dma_bd38_6"},
      {0x000a04d4, "mem_dma_bd38_5"},
      {0x000a04d0, "mem_dma_bd38_4"},
      {0x000a04cc, "mem_dma_bd38_3"},
      {0x000a04c8, "mem_dma_bd38_2"},
      {0x000a04c4, "mem_dma_bd38_1"},
      {0x000a04c0, "mem_dma_bd38_0"},
      {0x0003f2e8, "cm_stream_switch_slave_west_3_slot2"},
      {0x0003f2ec, "cm_stream_switch_slave_west_3_slot3"},
      {0x00014404, "mm_combo_event_control"},
      {0x0003f314, "cm_stream_switch_slave_north_2_slot1"},
      {0x0003f318, "cm_stream_switch_slave_north_2_slot2"},
      {0x0003f31c, "cm_stream_switch_slave_north_2_slot3"},
      {0x00031020, "shim_performance_counter0"},
      {0x00031024, "shim_performance_counter1"},
      {0x00014508, "mm_event_group_dma_enable"},
      {0x00014030, "shim_lock3_value"},
      {0x000340f4, "cm_timer_trig_event_high_value"},
      {0x000c0190, "mem_lock25_value"},
      {0x000d0000, "mem_lock_request"},
      {0x0001f128, "mm_locks_underflow"},
      {0x00034088, "cm_event_broadcast_block_east_value"},
      {0x00011084, "mm_performance_counter1_event_value"},
      {0x00034058, "cm_event_broadcast_block_south_value"},
      {0x0003f340, "cm_stream_switch_slave_east_1_slot0"},
      {0x0003f344, "cm_stream_switch_slave_east_1_slot1"},
      {0x0003f348, "cm_stream_switch_slave_east_1_slot2"},
      {0x0003f34c, "cm_stream_switch_slave_east_1_slot3"},
      {0x000c02a0, "mem_lock42_value"},
      {0x000a0468, "mem_dma_bd35_2"},
      {0x000a046c, "mem_dma_bd35_3"},
      {0x000a0460, "mem_dma_bd35_0"},
      {0x000140f4, "mm_timer_trig_event_high_value"},
      {0x000a0478, "mem_dma_bd35_6"},
      {0x0003ff20, "cm_stream_switch_parity_injection"},
      {0x000a0470, "mem_dma_bd35_4"},
      {0x000a0474, "mem_dma_bd35_5"},
      {0x000a04b0, "mem_dma_bd37_4"},
      {0x000a04b4, "mem_dma_bd37_5"},
      {0x000a04b8, "mem_dma_bd37_6"},
      {0x000a04bc, "mem_dma_bd37_7"},
      {0x000a04a0, "mem_dma_bd37_0"},
      {0x000a04a4, "mem_dma_bd37_1"},
      {0x000a04a8, "mem_dma_bd37_2"},
      {0x000a04ac, "mem_dma_bd37_3"},
      {0x000a03f8, "mem_dma_bd31_6"},
      {0x000a03fc, "mem_dma_bd31_7"},
      {0x000a03f0, "mem_dma_bd31_4"},
      {0x000a03f4, "mem_dma_bd31_5"},
      {0x000a03e8, "mem_dma_bd31_2"},
      {0x000a03ec, "mem_dma_bd31_3"},
      {0x000a03e0, "mem_dma_bd31_0"},
      {0x000a03e4, "mem_dma_bd31_1"},
      {0x000a0608, "mem_dma_s2mm_1_ctrl"},
      {0x0003f038, "cm_stream_switch_master_config_north1"},
      {0x0003f034, "cm_stream_switch_master_config_north0"},
      {0x0003f040, "cm_stream_switch_master_config_north3"},
      {0x0003f03c, "cm_stream_switch_master_config_north2"},
      {0x0003f048, "cm_stream_switch_master_config_north5"},
      {0x0003f044, "cm_stream_switch_master_config_north4"},
      {0x0001d044, "shim_dma_bd2_1"},
      {0x000fff00, "mem_module_clock_control"},
      {0x0001de10, "mm_dma_mm2s_0_ctrl"},
      {0x000a0674, "mem_dma_s2mm_status_5"},
      {0x000a0670, "mem_dma_s2mm_status_4"},
      {0x000a066c, "mem_dma_s2mm_status_3"},
      {0x000a0668, "mem_dma_s2mm_status_2"},
      {0x000a0664, "mem_dma_s2mm_status_1"},
      {0x000a0660, "mem_dma_s2mm_status_0"},
      {0x00014060, "mm_event_broadcast_block_west_set"},
      {0x00060000, "cm_module_clock_control"},
      {0x000b0f10, "mem_stream_switch_parity_status"},
      {0x00094408, "mem_edge_detection_event_control"},
      {0x00094034, "mem_event_broadcast9"},
      {0x0003f128, "cm_stream_switch_slave_config_south_5"},
      {0x0003f380, "cm_stream_switch_slave_mem_trace_slot0"},
      {0x0003f2a4, "cm_stream_switch_slave_south_5_slot1"},
      {0x0003f2a0, "cm_stream_switch_slave_south_5_slot0"},
      {0x0003f2ac, "cm_stream_switch_slave_south_5_slot3"},
      {0x0003f2a8, "cm_stream_switch_slave_south_5_slot2"},
      {0x000340d4, "cm_trace_control1"},
      {0x000340d0, "cm_trace_control0"},
      {0x0003f118, "cm_stream_switch_slave_config_south_1"},
      {0x000c0310, "mem_lock49_value"},
      {0x000c02f0, "mem_lock47_value"},
      {0x0001d188, "shim_dma_bd12_2"},
      {0x0001d18c, "shim_dma_bd12_3"},
      {0x0001d180, "shim_dma_bd12_0"},
      {0x0001d184, "shim_dma_bd12_1"},
      {0x0001d198, "shim_dma_bd12_6"},
      {0x0001d19c, "shim_dma_bd12_7"},
      {0x0001d190, "shim_dma_bd12_4"},
      {0x0001d194, "shim_dma_bd12_5"},
      {0x0001d150, "shim_dma_bd10_4"},
      {0x0001d154, "shim_dma_bd10_5"},
      {0x000b0260, "mem_stream_switch_slave_tile_ctrl_slot0"},
      {0x000b0264, "mem_stream_switch_slave_tile_ctrl_slot1"},
      {0x0001d140, "shim_dma_bd10_0"},
      {0x0001d144, "shim_dma_bd10_1"},
      {0x0001d148, "shim_dma_bd10_2"},
      {0x0001d14c, "shim_dma_bd10_3"},
      {0x000c00d0, "mem_lock13_value"},
      {0x0003f2cc, "cm_stream_switch_slave_west_1_slot3"},
      {0x000b025c, "mem_stream_switch_slave_dma_5_slot3"},
      {0x00014088, "mm_event_broadcast_block_east_value"},
      {0x00014054, "mm_event_broadcast_block_south_clr"},
      {0x000a0300, "mem_dma_bd24_0"},
      {0x000a0304, "mem_dma_bd24_1"},
      {0x000a0308, "mem_dma_bd24_2"},
      {0x000a030c, "mem_dma_bd24_3"},
      {0x000940a4, "mem_event_broadcast_b_block_west_clr"},
      {0x000b0f20, "mem_stream_switch_parity_injection"},
      {0x000a0610, "mem_dma_s2mm_2_ctrl"},
      {0x0001d064, "mm_dma_bd3_1"},
      {0x000c01e0, "mem_lock30_value"},
      {0x00034504, "cm_event_group_pc_enable"},
      {0x000a02d8, "mem_dma_bd22_6"},
      {0x0001d0cc, "shim_dma_bd6_3"},
      {0x000a02d0, "mem_dma_bd22_4"},
      {0x0001d0c4, "shim_dma_bd6_1"},
      {0x000c03c0, "mem_lock60_value"},
      {0x0001d0d8, "shim_dma_bd6_6"},
      {0x0001df04, "mm_dma_s2mm_status_1"},
      {0x0001df00, "mm_dma_s2mm_status_0"},
      {0x0001d0dc, "shim_dma_bd6_7"},
      {0x0001d0d0, "shim_dma_bd6_4"},
      {0x0001d0d4, "shim_dma_bd6_5"},
      {0x000c00a0, "mem_lock10_value"},
      {0x000a062c, "mem_dma_s2mm_5_start_queue"},
      {0x0001d020, "mm_dma_bd1_0"},
      {0x000a0380, "mem_dma_bd28_0"},
      {0x000a0384, "mem_dma_bd28_1"},
      {0x000a0388, "mem_dma_bd28_2"},
      {0x000a038c, "mem_dma_bd28_3"},
      {0x000a0390, "mem_dma_bd28_4"},
      {0x000a0394, "mem_dma_bd28_5"},
      {0x000a0398, "mem_dma_bd28_6"},
      {0x000a039c, "mem_dma_bd28_7"},
      {0x00034514, "cm_event_group_errors1_enable"},
      {0x000940e0, "mem_trace_event0"},
      {0x000940e4, "mem_trace_event1"},
      {0x000a0288, "mem_dma_bd20_2"},
      {0x000a028c, "mem_dma_bd20_3"},
      {0x000a0290, "mem_dma_bd20_4"},
      {0x000a0294, "mem_dma_bd20_5"},
      {0x000a0298, "mem_dma_bd20_6"},
      {0x000a029c, "mem_dma_bd20_7"},
      {0x000c03a0, "mem_lock58_value"},
      {0x00094084, "mem_event_broadcast_a_block_east_clr"},
      {0x000a060c, "mem_dma_s2mm_1_start_queue"},
      {0x0001d1e4, "shim_dma_bd15_1"},
      {0x0001d1e0, "shim_dma_bd15_0"},
      {0x0001d1ec, "shim_dma_bd15_3"},
      {0x0001d1e8, "shim_dma_bd15_2"},
      {0x0001d1f4, "shim_dma_bd15_5"},
      {0x0001d1f0, "shim_dma_bd15_4"},
      {0x00014084, "mm_event_broadcast_block_east_clr"},
      {0x0001d1f8, "shim_dma_bd15_6"},
      {0x0001d170, "shim_dma_bd11_4"},
      {0x000940a8, "mem_event_broadcast_b_block_west_value"},
      {0x00094030, "mem_event_broadcast8"},
      {0x000b010c, "mem_stream_switch_slave_config_dma_3"},
      {0x00014510, "mm_event_group_memory_conflict_enable"},
      {0x000c0260, "mem_lock38_value"},
      {0x00034034, "shim_event_broadcast_a_9"},
      {0x00034030, "shim_event_broadcast_a_8"},
      {0x0003402c, "shim_event_broadcast_a_7"},
      {0x00034028, "shim_event_broadcast_a_6"},
      {0x00034024, "shim_event_broadcast_a_5"},
      {0x00034020, "shim_event_broadcast_a_4"},
      {0x0003401c, "shim_event_broadcast_a_3"},
      {0x00034018, "shim_event_broadcast_a_2"},
      {0x00034014, "shim_event_broadcast_a_1"},
      {0x00034010, "shim_event_broadcast_a_0"},
      {0x000a0620, "mem_dma_s2mm_4_ctrl"},
      {0x0001d0b4, "shim_dma_bd5_5"},
      {0x0001d0b0, "shim_dma_bd5_4"},
      {0x0001d0bc, "shim_dma_bd5_7"},
      {0x0001d0b8, "shim_dma_bd5_6"},
      {0x0001d0a4, "shim_dma_bd5_1"},
      {0x0001d0a0, "shim_dma_bd5_0"},
      {0x0001d0ac, "shim_dma_bd5_3"},
      {0x0001d0a8, "shim_dma_bd5_2"},
      {0x0003200c, "cm_reset_event"},
      {0x000140f0, "shim_lock15_value"},
      {0x0001d07c, "shim_dma_bd3_7"},
      {0x0001d078, "shim_dma_bd3_6"},
      {0x0001d074, "shim_dma_bd3_5"},
      {0x0001d070, "shim_dma_bd3_4"},
      {0x0001d06c, "shim_dma_bd3_3"},
      {0x0001d068, "shim_dma_bd3_2"},
      {0x0001d064, "shim_dma_bd3_1"},
      {0x0001d060, "shim_dma_bd3_0"},
      {0x000c0180, "mem_lock24_value"},
      {0x000a0070, "mem_dma_bd3_4"},
      {0x000a0074, "mem_dma_bd3_5"},
      {0x000a0078, "mem_dma_bd3_6"},
      {0x000a007c, "mem_dma_bd3_7"},
      {0x00012124, "mm_parity_failing_address"},
      {0x000b0000, "mem_stream_switch_master_config_dma0"},
      {0x000b0004, "mem_stream_switch_master_config_dma1"},
      {0x000b0008, "mem_stream_switch_master_config_dma2"},
      {0x000b000c, "mem_stream_switch_master_config_dma3"},
      {0x000b0010, "mem_stream_switch_master_config_dma4"},
      {0x000b0014, "mem_stream_switch_master_config_dma5"},
      {0x000a036c, "mem_dma_bd27_3"},
      {0x000a0368, "mem_dma_bd27_2"},
      {0x000a0364, "mem_dma_bd27_1"},
      {0x000a0360, "mem_dma_bd27_0"},
      {0x000a037c, "mem_dma_bd27_7"},
      {0x000a0378, "mem_dma_bd27_6"},
      {0x000a0374, "mem_dma_bd27_5"},
      {0x000a0370, "mem_dma_bd27_4"},
      {0x000a0334, "mem_dma_bd25_5"},
      {0x000a0330, "mem_dma_bd25_4"},
      {0x000a033c, "mem_dma_bd25_7"},
      {0x000a0338, "mem_dma_bd25_6"},
      {0x000a0324, "mem_dma_bd25_1"},
      {0x000a0320, "mem_dma_bd25_0"},
      {0x000a032c, "mem_dma_bd25_3"},
      {0x000a0328, "mem_dma_bd25_2"},
      {0x0001d0ec, "shim_dma_bd7_3"},
      {0x0001d0e8, "shim_dma_bd7_2"},
      {0x000a02f4, "mem_dma_bd23_5"},
      {0x0001d0e0, "shim_dma_bd7_0"},
      {0x0001d0fc, "shim_dma_bd7_7"},
      {0x0001d0f8, "shim_dma_bd7_6"},
      {0x0001d0f4, "shim_dma_bd7_5"},
      {0x0001d0f0, "shim_dma_bd7_4"},
      {0x000b0204, "mem_stream_switch_slave_dma_0_slot1"},
      {0x000b0200, "mem_stream_switch_slave_dma_0_slot0"},
      {0x000b020c, "mem_stream_switch_slave_dma_0_slot3"},
      {0x000b0208, "mem_stream_switch_slave_dma_0_slot2"},
      {0x000a0638, "mem_dma_mm2s_1_ctrl"},
      {0x00034404, "cm_combo_event_control"},
      {0x000b0144, "mem_stream_switch_slave_config_trace"},
      {0x000a0280, "mem_dma_bd20_0"},
      {0x000a0284, "mem_dma_bd20_1"},
      {0x00032018, "cm_debug_control2"},
      {0x00032014, "cm_debug_control1"},
      {0x00032010, "cm_debug_control0"},
      {0x0001d100, "shim_dma_bd8_0"},
      {0x00014010, "mm_event_broadcast0"},
      {0x0001d108, "shim_dma_bd8_2"},
      {0x0001d10c, "shim_dma_bd8_3"},
      {0x0001d110, "shim_dma_bd8_4"},
      {0x0001d114, "shim_dma_bd8_5"},
      {0x0001d118, "shim_dma_bd8_6"},
      {0x0001d11c, "shim_dma_bd8_7"},
      {0x000c02b0, "mem_lock43_value"},
      {0x000340f8, "shim_timer_low"},
      {0x0001de08, "mm_dma_s2mm_1_ctrl"},
      {0x0009451c, "mem_event_group_broadcast_enable"},
      {0x0003f308, "cm_stream_switch_slave_north_1_slot2"},
      {0x0003f304, "cm_stream_switch_slave_north_1_slot1"},
      {0x0003f300, "cm_stream_switch_slave_north_1_slot0"},
      {0x0001d000, "shim_dma_bd0_0"},
      {0x0001d004, "shim_dma_bd0_1"},
      {0x0001d008, "shim_dma_bd0_2"},
      {0x0001d00c, "shim_dma_bd0_3"},
      {0x0001d010, "shim_dma_bd0_4"},
      {0x0001d014, "shim_dma_bd0_5"},
      {0x0001d018, "shim_dma_bd0_6"},
      {0x0001d01c, "shim_dma_bd0_7"},
      {0x000940b4, "mem_event_broadcast_b_block_north_clr"},
      {0x000b0210, "mem_stream_switch_slave_dma_1_slot0"},
      {0x000b0214, "mem_stream_switch_slave_dma_1_slot1"},
      {0x000b0218, "mem_stream_switch_slave_dma_1_slot2"},
      {0x000b021c, "mem_stream_switch_slave_dma_1_slot3"},
      {0x00034408, "cm_edge_detection_event_control"},
      {0x000c0070, "mem_lock7_value"},
      {0x0003201c, "cm_debug_status"},
      {0x000b030c, "mem_stream_switch_slave_north_3_slot3"},
      {0x000b0308, "mem_stream_switch_slave_north_3_slot2"},
      {0x000b0304, "mem_stream_switch_slave_north_3_slot1"},
      {0x000b0300, "mem_stream_switch_slave_north_3_slot0"},
      {0x0003202c, "cm_pc_event3"},
      {0x00032028, "cm_pc_event2"},
      {0x00032024, "cm_pc_event1"},
      {0x00032020, "cm_pc_event0"},
      {0x0003f000, "cm_stream_switch_master_config_aie_core0"},
      {0x000a0238, "mem_dma_bd17_6"},
      {0x000a023c, "mem_dma_bd17_7"},
      {0x0001f118, "mm_locks_event_selection_6"},
      {0x0001f11c, "mm_locks_event_selection_7"},
      {0x000a06c4, "mem_dma_s2mm_current_write_count_5"},
      {0x0001f114, "mm_locks_event_selection_5"},
      {0x0001f108, "mm_locks_event_selection_2"},
      {0x000a06b8, "mem_dma_s2mm_current_write_count_2"},
      {0x0001f100, "mm_locks_event_selection_0"},
      {0x0001f104, "mm_locks_event_selection_1"},
      {0x000a02f8, "mem_dma_bd23_6"},
      {0x00040000, "mm_lock_request"},
      {0x000940f0, "mem_timer_trig_event_low_value"},
      {0x0009404c, "mem_event_broadcast15"},
      {0x00094038, "mem_event_broadcast10"},
      {0x0009403c, "mem_event_broadcast11"},
      {0x00094040, "mem_event_broadcast12"},
      {0x00094044, "mem_event_broadcast13"},
      {0x0003f324, "cm_stream_switch_slave_north_3_slot1"},
      {0x0003f320, "cm_stream_switch_slave_north_3_slot0"},
      {0x0003f32c, "cm_stream_switch_slave_north_3_slot3"},
      {0x0003f328, "cm_stream_switch_slave_north_3_slot2"},
      {0x0003ff10, "cm_stream_switch_parity_status"},
      {0x00036030, "cm_tile_control"},
      {0x00031580, "cm_performance_counter0_event_value"},
      {0x000b02a4, "mem_stream_switch_slave_south_3_slot1"},
      {0x000b02a0, "mem_stream_switch_slave_south_3_slot0"},
      {0x000b02ac, "mem_stream_switch_slave_south_3_slot3"},
      {0x000b02a8, "mem_stream_switch_slave_south_3_slot2"},
      {0x000c0160, "mem_lock22_value"},
      {0x000340f8, "cm_timer_low"},
      {0x00094080, "mem_event_broadcast_a_block_east_set"},
      {0x00014504, "mm_event_group_watchpoint_enable"},
      {0x000c0080, "mem_lock8_value"},
      {0x000c0210, "mem_lock33_value"},
      {0x000b012c, "mem_stream_switch_slave_config_south_4"},
      {0x000a064c, "mem_dma_mm2s_3_start_queue"},
      {0x00094098, "mem_event_broadcast_b_block_south_value"},
      {0x000140d0, "shim_lock13_value"},
      {0x000940b8, "mem_event_broadcast_b_block_north_value"},
      {0x0003f04c, "cm_stream_switch_master_config_east0"},
      {0x0003f050, "cm_stream_switch_master_config_east1"},
      {0x0003f054, "cm_stream_switch_master_config_east2"},
      {0x0003f058, "cm_stream_switch_master_config_east3"},
      {0x0001d1fc, "shim_dma_bd15_7"},
      {0x000a06c8, "mem_dma_s2mm_fot_count_fifo_pop_0"},
      {0x0003f230, "cm_stream_switch_slave_tile_ctrl_slot0"},
      {0x000c0320, "mem_lock50_value"},
      {0x00034504, "shim_event_group_dma_enable"},
      {0x0003450c, "cm_event_group_core_program_flow_enable"},
      {0x00014050, "mm_event_broadcast_block_south_set"},
      {0x0001d1d4, "mm_dma_bd14_5"},
      {0x0001d1d0, "mm_dma_bd14_4"},
      {0x0001d1c4, "mm_dma_bd14_1"},
      {0x0001d1c0, "mm_dma_bd14_0"},
      {0x0001d1cc, "mm_dma_bd14_3"},
      {0x0001d1c8, "mm_dma_bd14_2"},
      {0x00034044, "cm_event_broadcast13"},
      {0x00016000, "mm_spare_reg"},
      {0x0003f148, "cm_stream_switch_slave_config_north_3"},
      {0x0003f00c, "cm_stream_switch_master_config_tile_ctrl"},
      {0x0003f140, "cm_stream_switch_slave_config_north_1"},
      {0x0003f13c, "cm_stream_switch_slave_config_north_0"},
      {0x00094504, "mem_event_group_watchpoint_enable"},
      {0x00096030, "mem_tile_control"},
      {0x0001d144, "mm_dma_bd10_1"},
      {0x0001d140, "mm_dma_bd10_0"},
      {0x0001d14c, "mm_dma_bd10_3"},
      {0x0001d148, "mm_dma_bd10_2"},
      {0x0001d154, "mm_dma_bd10_5"},
      {0x0001d150, "mm_dma_bd10_4"},
      {0x0001d194, "mm_dma_bd12_5"},
      {0x0001d190, "mm_dma_bd12_4"},
      {0x0001d18c, "mm_dma_bd12_3"},
      {0x0001d188, "mm_dma_bd12_2"},
      {0x0001d184, "mm_dma_bd12_1"},
      {0x0001d180, "mm_dma_bd12_0"},
      {0x000a01c4, "mem_dma_bd14_1"},
      {0x000a01c0, "mem_dma_bd14_0"},
      {0x000a01cc, "mem_dma_bd14_3"},
      {0x000a01c8, "mem_dma_bd14_2"},
      {0x000a01d4, "mem_dma_bd14_5"},
      {0x000a01d0, "mem_dma_bd14_4"},
      {0x000a01dc, "mem_dma_bd14_7"},
      {0x000a01d8, "mem_dma_bd14_6"},
      {0x00034058, "shim_event_broadcast_a_block_south_value"},
      {0x00032030, "cm_error_halt_control"},
      {0x00094070, "mem_event_broadcast_a_block_north_set"},
      {0x00096000, "mem_spare_reg"},
      {0x000a018c, "mem_dma_bd12_3"},
      {0x000a0188, "mem_dma_bd12_2"},
      {0x000a0184, "mem_dma_bd12_1"},
      {0x000a0180, "mem_dma_bd12_0"},
      {0x000a019c, "mem_dma_bd12_7"},
      {0x000a0198, "mem_dma_bd12_6"},
      {0x000a0194, "mem_dma_bd12_5"},
      {0x000a0190, "mem_dma_bd12_4"},
      {0x00031008, "shim_performance_control1"},
      {0x00031000, "shim_performance_control0"},
      {0x0003ff00, "cm_stream_switch_event_port_selection_0"},
      {0x0003ff04, "cm_stream_switch_event_port_selection_1"},
      {0x000a0154, "mem_dma_bd10_5"},
      {0x000a0150, "mem_dma_bd10_4"},
      {0x000a015c, "mem_dma_bd10_7"},
      {0x000a0158, "mem_dma_bd10_6"},
      {0x000a0144, "mem_dma_bd10_1"},
      {0x000a0140, "mem_dma_bd10_0"},
      {0x000a014c, "mem_dma_bd10_3"},
      {0x000a0148, "mem_dma_bd10_2"},
      {0x000a021c, "mem_dma_bd16_7"},
      {0x000a0218, "mem_dma_bd16_6"},
      {0x000a0214, "mem_dma_bd16_5"},
      {0x000a0210, "mem_dma_bd16_4"},
      {0x000a020c, "mem_dma_bd16_3"},
      {0x000a0208, "mem_dma_bd16_2"},
      {0x000a0204, "mem_dma_bd16_1"},
      {0x000a0200, "mem_dma_bd16_0"},
      {0x0001d060, "mm_dma_bd3_0"},
      {0x00094514, "mem_event_group_memory_conflict_enable"},
      {0x0003f334, "cm_stream_switch_slave_east_0_slot1"},
      {0x0003f330, "cm_stream_switch_slave_east_0_slot0"},
      {0x0003f33c, "cm_stream_switch_slave_east_0_slot3"},
      {0x0003f338, "cm_stream_switch_slave_east_0_slot2"},
      {0x00034080, "shim_event_broadcast_a_block_east_set"},
      {0x00014200, "mm_event_status0"},
      {0x00014204, "mm_event_status1"},
      {0x00014208, "mm_event_status2"},
      {0x0001420c, "mm_event_status3"},
      {0x00034074, "shim_event_broadcast_a_block_north_clr"},
      {0x0001f040, "mm_lock4_value"},
      {0x000a0630, "mem_dma_mm2s_0_ctrl"},
      {0x00031588, "cm_performance_counter2_event_value"},
      {0x000340f0, "shim_timer_trig_event_low_value"},
      {0x0003ff34, "cm_stream_switch_adaptive_clock_gate_status"},
      {0x000b0804, "mem_stream_switch_deterministic_merge_arb0_slave2_3"},
      {0x000a00b0, "mem_dma_bd5_4"},
      {0x00094000, "mem_timer_control"},
      {0x000c0420, "mem_locks_overflow_0"},
      {0x000c0424, "mem_locks_overflow_1"},
      {0x000a0270, "mem_dma_bd19_4"},
      {0x000a0274, "mem_dma_bd19_5"},
      {0x000a0278, "mem_dma_bd19_6"},
      {0x000a027c, "mem_dma_bd19_7"},
      {0x000a0260, "mem_dma_bd19_0"},
      {0x000a0264, "mem_dma_bd19_1"},
      {0x000a0268, "mem_dma_bd19_2"},
      {0x000a026c, "mem_dma_bd19_3"},
      {0x000b0128, "mem_stream_switch_slave_config_south_3"},
      {0x000b0124, "mem_stream_switch_slave_config_south_2"},
      {0x000b0120, "mem_stream_switch_slave_config_south_1"},
      {0x000b011c, "mem_stream_switch_slave_config_south_0"},
      {0x000b0130, "mem_stream_switch_slave_config_south_5"},
      {0x0003f244, "cm_stream_switch_slave_fifo_0_slot1"},
      {0x0003f248, "cm_stream_switch_slave_fifo_0_slot2"},
      {0x0001450c, "mm_event_group_lock_enable"},
      {0x0003f24c, "cm_stream_switch_slave_fifo_0_slot3"},
      {0x000c0030, "mem_lock3_value"},
      {0x000a0654, "mem_dma_mm2s_4_start_queue"},
      {0x000a0600, "mem_dma_s2mm_0_ctrl"},
      {0x00094510, "mem_event_group_stream_switch_enable"},
      {0x000c0370, "mem_lock55_value"},
      {0x000a0694, "mem_dma_mm2s_status_5"},
      {0x000a0690, "mem_dma_mm2s_status_4"},
      {0x000a0684, "mem_dma_mm2s_status_1"},
      {0x000a0680, "mem_dma_mm2s_status_0"},
      {0x000a068c, "mem_dma_mm2s_status_3"},
      {0x000a0688, "mem_dma_mm2s_status_2"},
      {0x00094058, "mem_event_broadcast_a_block_south_value"},
      {0x000940c0, "mem_event_broadcast_b_block_east_set"},
      {0x000a0648, "mem_dma_mm2s_3_ctrl"},
      {0x00034060, "cm_event_broadcast_block_west_set"},
      {0x000b0f38, "mem_stream_switch_adaptive_clock_gate_abort_period"},
      {0x00034084, "cm_event_broadcast_block_east_clr"},
      {0x0009108c, "mem_performance_counter3_event_value"},
      {0x00014020, "shim_lock2_value"},
      {0x00011024, "mm_performance_counter1"},
      {0x00011020, "mm_performance_counter0"},
      {0x000a065c, "mem_dma_mm2s_5_start_queue"},
      {0x00014068, "mm_event_broadcast_block_west_value"},
      {0x000b027c, "mem_stream_switch_slave_south_0_slot3"},
      {0x000b0270, "mem_stream_switch_slave_south_0_slot0"},
      {0x000b0274, "mem_stream_switch_slave_south_0_slot1"},
      {0x00032000, "cm_core_control"},
      {0x000c0010, "mem_lock1_value"},
      {0x000a0508, "mem_dma_bd40_2"},
      {0x000a050c, "mem_dma_bd40_3"},
      {0x000a0500, "mem_dma_bd40_0"},
      {0x000a0504, "mem_dma_bd40_1"},
      {0x000a0518, "mem_dma_bd40_6"},
      {0x000a051c, "mem_dma_bd40_7"},
      {0x000a0510, "mem_dma_bd40_4"},
      {0x000a0514, "mem_dma_bd40_5"},
      {0x000a0550, "mem_dma_bd42_4"},
      {0x000a0554, "mem_dma_bd42_5"},
      {0x000a0558, "mem_dma_bd42_6"},
      {0x000a055c, "mem_dma_bd42_7"},
      {0x000a0540, "mem_dma_bd42_0"},
      {0x000a0544, "mem_dma_bd42_1"},
      {0x000a0548, "mem_dma_bd42_2"},
      {0x000a054c, "mem_dma_bd42_3"},
      {0x000a0598, "mem_dma_bd44_6"},
      {0x000a059c, "mem_dma_bd44_7"},
      {0x000a0590, "mem_dma_bd44_4"},
      {0x000a0594, "mem_dma_bd44_5"},
      {0x000a0588, "mem_dma_bd44_2"},
      {0x000a058c, "mem_dma_bd44_3"},
      {0x000a0580, "mem_dma_bd44_0"},
      {0x000a0584, "mem_dma_bd44_1"},
      {0x0003f810, "cm_stream_switch_deterministic_merge_arb1_slave0_1"},
      {0x00094088, "mem_event_broadcast_a_block_east_value"},
      {0x00034054, "cm_event_broadcast_block_south_clr"},
      {0x000c01a0, "mem_lock26_value"},
      {0x00014070, "shim_lock7_value"},
      {0x000b02b8, "mem_stream_switch_slave_south_4_slot2"},
      {0x000b02bc, "mem_stream_switch_slave_south_4_slot3"},
      {0x000b02b0, "mem_stream_switch_slave_south_4_slot0"},
      {0x000b02b4, "mem_stream_switch_slave_south_4_slot1"},
      {0x0003420c, "shim_event_status3"},
      {0x00034208, "shim_event_status2"},
      {0x00034204, "shim_event_status1"},
      {0x00034200, "shim_event_status0"},
      {0x000c0110, "mem_lock17_value"},
      {0x00096048, "mem_memory_control"},
      {0x0003f228, "cm_stream_switch_slave_dma_1_slot2"},
      {0x000a0604, "mem_dma_s2mm_0_start_queue"},
      {0x0003f22c, "cm_stream_switch_slave_dma_1_slot3"},
      {0x0003f220, "cm_stream_switch_slave_dma_1_slot0"},
      {0x0001df24, "mm_dma_s2mm_fot_count_fifo_pop_1"},
      {0x0001df20, "mm_dma_s2mm_fot_count_fifo_pop_0"},
      {0x0003f224, "cm_stream_switch_slave_dma_1_slot1"},
      {0x000940d8, "mem_trace_status"},
      {0x00012110, "mm_ecc_scrubbing_event"},
      {0x0003f020, "cm_stream_switch_master_config_south3"},
      {0x0003f01c, "cm_stream_switch_master_config_south2"},
      {0x0003f018, "cm_stream_switch_master_config_south1"},
      {0x0003f014, "cm_stream_switch_master_config_south0"},
      {0x00031084, "shim_performance_counter1_event_value"},
      {0x00014010, "shim_lock1_value"},
      {0x000340e4, "shim_trace_event1"},
      {0x000340e0, "shim_trace_event0"},
      {0x0001421c, "mm_reserved3"},
      {0x000a040c, "mem_dma_bd32_3"},
      {0x00014214, "mm_reserved1"},
      {0x00014210, "mm_reserved0"},
      {0x000c042c, "mem_locks_underflow_1"},
      {0x000c0428, "mem_locks_underflow_0"},
      {0x000b028c, "mem_stream_switch_slave_south_1_slot3"},
      {0x000c0140, "mem_lock20_value"},
      {0x000b0288, "mem_stream_switch_slave_south_1_slot2"},
      {0x000b0284, "mem_stream_switch_slave_south_1_slot1"},
      {0x000b0280, "mem_stream_switch_slave_south_1_slot0"},
      {0x00014038, "mm_event_broadcast10"},
      {0x0001403c, "mm_event_broadcast11"},
      {0x00014040, "mm_event_broadcast12"},
      {0x00014044, "mm_event_broadcast13"},
      {0x00014048, "mm_event_broadcast14"},
      {0x0001404c, "mm_event_broadcast15"},
      {0x0003f160, "cm_stream_switch_slave_config_mem_trace"},
      {0x000c0418, "mem_locks_event_selection_6"},
      {0x000c041c, "mem_locks_event_selection_7"},
      {0x000c0410, "mem_locks_event_selection_4"},
      {0x000c0414, "mem_locks_event_selection_5"},
      {0x000c0408, "mem_locks_event_selection_2"},
      {0x000c040c, "mem_locks_event_selection_3"},
      {0x000c0400, "mem_locks_event_selection_0"},
      {0x000c0404, "mem_locks_event_selection_1"},
      {0x00094060, "mem_event_broadcast_a_block_west_set"},
      {0x0003f2e0, "cm_stream_switch_slave_west_3_slot0"},
      {0x000c0340, "mem_lock52_value"},
      {0x000c01d0, "mem_lock29_value"},
      {0x000a02fc, "mem_dma_bd23_7"},
      {0x000c03e0, "mem_lock62_value"},
      {0x0001d0e4, "shim_dma_bd7_1"},
      {0x000c01b0, "mem_lock27_value"},
      {0x0003f2e4, "cm_stream_switch_slave_west_3_slot1"},
      {0x000a02f0, "mem_dma_bd23_4"},
      {0x000a02ec, "mem_dma_bd23_3"},
      {0x000a02e8, "mem_dma_bd23_2"},
      {0x000a02e4, "mem_dma_bd23_1"},
      {0x000a02e0, "mem_dma_bd23_0"},
      {0x0003451c, "cm_event_group_broadcast_enable"},
      {0x000a0650, "mem_dma_mm2s_4_ctrl"},
      {0x00094050, "mem_event_broadcast_a_block_south_set"},
      {0x00011080, "mm_performance_counter0_event_value"},
      {0x00032038, "cm_core_processor_bus"},
      {0x0001d08c, "mm_dma_bd4_3"},
      {0x0001d088, "mm_dma_bd4_2"},
      {0x00034068, "cm_event_broadcast_block_west_value"},
      {0x0001d080, "mm_dma_bd4_0"},
      {0x0001d094, "mm_dma_bd4_5"},
      {0x0001d090, "mm_dma_bd4_4"},
      {0x00014408, "mm_edge_detection_event_control"},
      {0x00012120, "mm_ecc_failing_address"},
      {0x0001d044, "mm_dma_bd2_1"},
      {0x0001d040, "mm_dma_bd2_0"},
      {0x0001d04c, "mm_dma_bd2_3"},
      {0x0001d048, "mm_dma_bd2_2"},
      {0x0001d054, "mm_dma_bd2_5"},
      {0x0001d050, "mm_dma_bd2_4"},
      {0x0001d014, "mm_dma_bd0_5"},
      {0x0001d010, "mm_dma_bd0_4"},
      {0x0001d00c, "mm_dma_bd0_3"},
      {0x0001d008, "mm_dma_bd0_2"},
      {0x0001d004, "mm_dma_bd0_1"},
      {0x0001d000, "mm_dma_bd0_0"},
      {0x00014030, "mm_event_broadcast8"},
      {0x00014034, "mm_event_broadcast9"},
      {0x0001d0d4, "mm_dma_bd6_5"},
      {0x0001d0d0, "mm_dma_bd6_4"},
      {0x00014020, "mm_event_broadcast4"},
      {0x00014024, "mm_event_broadcast5"},
      {0x0001d0c4, "mm_dma_bd6_1"},
      {0x0001d0c0, "mm_dma_bd6_0"},
      {0x000a0464, "mem_dma_bd35_1"},
      {0x0001d0c8, "mm_dma_bd6_2"},
      {0x000a05e4, "mem_dma_bd47_1"},
      {0x000a05e0, "mem_dma_bd47_0"},
      {0x000a05ec, "mem_dma_bd47_3"},
      {0x000a05e8, "mem_dma_bd47_2"},
      {0x000a05f4, "mem_dma_bd47_5"},
      {0x000a05f0, "mem_dma_bd47_4"},
      {0x000a05fc, "mem_dma_bd47_7"},
      {0x000a05f8, "mem_dma_bd47_6"},
      {0x0001f010, "mm_lock1_value"},
      {0x000b0814, "mem_stream_switch_deterministic_merge_arb1_slave2_3"},
      {0x00031500, "cm_performance_control0"},
      {0x00031504, "cm_performance_control1"},
      {0x00031508, "cm_performance_control2"},
      {0x000b0810, "mem_stream_switch_deterministic_merge_arb1_slave0_1"},
      {0x00034044, "shim_event_broadcast_a_13"},
      {0x00034040, "shim_event_broadcast_a_12"},
      {0x0003403c, "shim_event_broadcast_a_11"},
      {0x00034038, "shim_event_broadcast_a_10"},
      {0x0003404c, "shim_event_broadcast_a_15"},
      {0x00034048, "shim_event_broadcast_a_14"},
      {0x000a0618, "mem_dma_s2mm_3_ctrl"},
      {0x00094054, "mem_event_broadcast_a_block_south_clr"},
      {0x00014078, "mm_event_broadcast_block_north_value"},
      {0x00014080, "mm_event_broadcast_block_east_set"},
      {0x000b0278, "mem_stream_switch_slave_south_0_slot2"},
      {0x000a0094, "mem_dma_bd4_5"},
      {0x000c0380, "mem_lock56_value"},
      {0x0001d104, "shim_dma_bd8_1"},
      {0x0001f060, "mm_lock6_value"},
      {0x0001d130, "mm_dma_bd9_4"},
      {0x0001d134, "mm_dma_bd9_5"},
      {0x0001d128, "mm_dma_bd9_2"},
      {0x0001d12c, "mm_dma_bd9_3"},
      {0x0001d120, "mm_dma_bd9_0"},
      {0x0001d124, "mm_dma_bd9_1"},
      {0x000c02d0, "mem_lock45_value"},
      {0x0003ff04, "shim_stream_switch_event_port_selection_1"},
      {0x0003ff00, "shim_stream_switch_event_port_selection_0"},
      {0x0003f2c8, "cm_stream_switch_slave_west_1_slot2"},
      {0x0009450c, "mem_event_group_lock_enable"},
      {0x0003f2c0, "cm_stream_switch_slave_west_1_slot0"},
      {0x0003f2c4, "cm_stream_switch_slave_west_1_slot1"},
      {0x0003f110, "cm_stream_switch_slave_config_fifo_0"},
      {0x000c0290, "mem_lock41_value"},
      {0x0001f090, "mm_lock9_value"},
      {0x00036060, "cm_accumulator_control"},
      {0x0003f250, "cm_stream_switch_slave_south_0_slot0"},
      {0x0003f254, "cm_stream_switch_slave_south_0_slot1"},
      {0x0003f258, "cm_stream_switch_slave_south_0_slot2"},
      {0x0003f25c, "cm_stream_switch_slave_south_0_slot3"},
      {0x000140c0, "shim_lock12_value"},
      {0x000c03b0, "mem_lock59_value"},
      {0x0003f310, "cm_stream_switch_slave_north_2_slot0"},
      {0x0001f0a0, "mm_lock10_value"},
      {0x0003f21c, "cm_stream_switch_slave_dma_0_slot3"},
      {0x0003f218, "cm_stream_switch_slave_dma_0_slot2"},
      {0x0003f214, "cm_stream_switch_slave_dma_0_slot1"},
      {0x0003f210, "cm_stream_switch_slave_dma_0_slot0"},
      {0x0003f30c, "cm_stream_switch_slave_north_1_slot3"},
      {0x00034208, "cm_event_status2"},
      {0x0003420c, "cm_event_status3"},
      {0x00034200, "cm_event_status0"},
      {0x00034204, "cm_event_status1"},
      {0x00034048, "cm_event_broadcast14"},
      {0x0003404c, "cm_event_broadcast15"},
      {0x00034040, "cm_event_broadcast12"},
      {0x00034500, "cm_event_group_0_enable"},
      {0x00034038, "cm_event_broadcast10"},
      {0x0003403c, "cm_event_broadcast11"},
      {0x000c0060, "mem_lock6_value"},
      {0x00034088, "shim_event_broadcast_a_block_east_value"},
      {0x0001de1c, "mm_dma_mm2s_1_start_queue"},
      {0x000a00e0, "mem_dma_bd7_0"},
      {0x000a00e4, "mem_dma_bd7_1"},
      {0x000a00e8, "mem_dma_bd7_2"},
      {0x000a00ec, "mem_dma_bd7_3"},
      {0x000a00f0, "mem_dma_bd7_4"},
      {0x000a00f4, "mem_dma_bd7_5"},
      {0x000a00f8, "mem_dma_bd7_6"},
      {0x000a00fc, "mem_dma_bd7_7"},
      {0x000a044c, "mem_dma_bd34_3"},
      {0x000a0448, "mem_dma_bd34_2"},
      {0x000a0444, "mem_dma_bd34_1"},
      {0x000a0440, "mem_dma_bd34_0"},
      {0x000a045c, "mem_dma_bd34_7"},
      {0x000a0458, "mem_dma_bd34_6"},
      {0x000a0454, "mem_dma_bd34_5"},
      {0x000a0450, "mem_dma_bd34_4"},
      {0x000a00b8, "mem_dma_bd5_6"},
      {0x000a00bc, "mem_dma_bd5_7"},
      {0x00094064, "mem_event_broadcast_a_block_west_clr"},
      {0x000a00b4, "mem_dma_bd5_5"},
      {0x000a00a8, "mem_dma_bd5_2"},
      {0x000a00ac, "mem_dma_bd5_3"},
      {0x000a00a0, "mem_dma_bd5_0"},
      {0x000a00a4, "mem_dma_bd5_1"},
      {0x000a0494, "mem_dma_bd36_5"},
      {0x000a0490, "mem_dma_bd36_4"},
      {0x000a049c, "mem_dma_bd36_7"},
      {0x000a0498, "mem_dma_bd36_6"},
      {0x000a0484, "mem_dma_bd36_1"},
      {0x000a0480, "mem_dma_bd36_0"},
      {0x000a048c, "mem_dma_bd36_3"},
      {0x000a0488, "mem_dma_bd36_2"},
      {0x000a03dc, "mem_dma_bd30_7"},
      {0x000a03d8, "mem_dma_bd30_6"},
      {0x000a03d4, "mem_dma_bd30_5"},
      {0x000a03d0, "mem_dma_bd30_4"},
      {0x000a03cc, "mem_dma_bd30_3"},
      {0x000a03c8, "mem_dma_bd30_2"},
      {0x000a03c4, "mem_dma_bd30_1"},
      {0x000a03c0, "mem_dma_bd30_0"},
      {0x000b0298, "mem_stream_switch_slave_south_2_slot2"},
      {0x00014008, "mm_event_generate"},
      {0x00031584, "cm_performance_counter1_event_value"},
      {0x000c00f0, "mem_lock15_value"},
      {0x00034050, "cm_event_broadcast_block_south_set"},
      {0x000a0128, "mem_dma_bd9_2"},
      {0x000a012c, "mem_dma_bd9_3"},
      {0x000a0120, "mem_dma_bd9_0"},
      {0x000a0124, "mem_dma_bd9_1"},
      {0x000a0138, "mem_dma_bd9_6"},
      {0x000a013c, "mem_dma_bd9_7"},
      {0x000a0130, "mem_dma_bd9_4"},
      {0x000a0134, "mem_dma_bd9_5"},
      {0x000c0170, "mem_lock23_value"},
      {0x000a000c, "mem_dma_bd0_3"},
      {0x000a0008, "mem_dma_bd0_2"},
      {0x000a0004, "mem_dma_bd0_1"},
      {0x000a0000, "mem_dma_bd0_0"},
      {0x000a001c, "mem_dma_bd0_7"},
      {0x000a0018, "mem_dma_bd0_6"},
      {0x000a0014, "mem_dma_bd0_5"},
      {0x000a0010, "mem_dma_bd0_4"},
      {0x000b0f34, "mem_stream_switch_adaptive_clock_gate_status"},
      {0x00034020, "cm_event_broadcast4"},
      {0x0003f36c, "cm_stream_switch_slave_east_3_slot3"},
      {0x00034028, "cm_event_broadcast6"},
      {0x0003402c, "cm_event_broadcast7"},
      {0x000140a0, "shim_lock10_value"},
      {0x000a009c, "mem_dma_bd4_7"},
      {0x000a0098, "mem_dma_bd4_6"},
      {0x00034510, "cm_event_group_errors0_enable"},
      {0x000a0090, "mem_dma_bd4_4"},
      {0x000a008c, "mem_dma_bd4_3"},
      {0x000a0088, "mem_dma_bd4_2"},
      {0x000a0084, "mem_dma_bd4_1"},
      {0x000a0080, "mem_dma_bd4_0"},
      {0x000a0054, "mem_dma_bd2_5"},
      {0x000a0050, "mem_dma_bd2_4"},
      {0x000a005c, "mem_dma_bd2_7"},
      {0x000a0058, "mem_dma_bd2_6"},
      {0x000a0044, "mem_dma_bd2_1"},
      {0x000a0040, "mem_dma_bd2_0"},
      {0x000a004c, "mem_dma_bd2_3"},
      {0x000a0048, "mem_dma_bd2_2"},
      {0x0003f10c, "cm_stream_switch_slave_config_tile_ctrl"},
      {0x000b0290, "mem_stream_switch_slave_south_2_slot0"},
      {0x000b0294, "mem_stream_switch_slave_south_2_slot1"},
      {0x0003f804, "cm_stream_switch_deterministic_merge_arb0_slave2_3"},
      {0x000b029c, "mem_stream_switch_slave_south_2_slot3"},
      {0x00094108, "mem_watchpoint2"},
      {0x0009410c, "mem_watchpoint3"},
      {0x00094100, "mem_watchpoint0"},
      {0x00094104, "mem_watchpoint1"},
      {0x0001de14, "mm_dma_mm2s_0_start_queue"},
      {0x0003f800, "cm_stream_switch_deterministic_merge_arb0_slave0_1"},
      {0x00014058, "mm_event_broadcast_block_south_value"},
      {0x000c0220, "mem_lock34_value"},
      {0x00094078, "mem_event_broadcast_a_block_north_value"},
      {0x0003f010, "cm_stream_switch_master_config_fifo0"},
      {0x00094094, "mem_event_broadcast_b_block_south_clr"},
      {0x000340fc, "cm_timer_high"},
      {0x000c0240, "mem_lock36_value"},
      {0x00016010, "mm_memory_control"},
      {0x00032034, "cm_error_halt_event"},
      {0x000a04f8, "mem_dma_bd39_6"},
      {0x00014400, "mm_combo_event_inputs"},
      {0x000a04f0, "mem_dma_bd39_4"},
      {0x000a04f4, "mem_dma_bd39_5"},
      {0x000a04e8, "mem_dma_bd39_2"},
      {0x000a04ec, "mem_dma_bd39_3"},
      {0x000a04e0, "mem_dma_bd39_0"},
      {0x000a04e4, "mem_dma_bd39_1"},
      {0x0009102c, "mem_performance_counter3"},
      {0x00091028, "mem_performance_counter2"},
      {0x00091024, "mem_performance_counter1"},
      {0x00091020, "mem_performance_counter0"},
      {0x0003f814, "cm_stream_switch_deterministic_merge_arb1_slave2_3"},
      {0x000c01f0, "mem_lock31_value"},
      {0x000c0150, "mem_lock21_value"},
      {0x0003f35c, "cm_stream_switch_slave_east_2_slot3"},
      {0x0003f358, "cm_stream_switch_slave_east_2_slot2"},
      {0x0003f354, "cm_stream_switch_slave_east_2_slot1"},
      {0x0003f350, "cm_stream_switch_slave_east_2_slot0"},
      {0x000a0420, "mem_dma_bd33_0"},
      {0x000a0424, "mem_dma_bd33_1"},
      {0x000a0428, "mem_dma_bd33_2"},
      {0x000a042c, "mem_dma_bd33_3"},
      {0x000a0430, "mem_dma_bd33_4"},
      {0x000a0434, "mem_dma_bd33_5"},
      {0x000a0438, "mem_dma_bd33_6"},
      {0x000a043c, "mem_dma_bd33_7"},
      {0x000b0f30, "mem_tile_control_packet_handler_status"},
      {0x0003f264, "cm_stream_switch_slave_south_1_slot1"},
      {0x0003f260, "cm_stream_switch_slave_south_1_slot0"},
      {0x0003f26c, "cm_stream_switch_slave_south_1_slot3"},
      {0x0003f268, "cm_stream_switch_slave_south_1_slot2"},
      {0x00034078, "cm_event_broadcast_block_north_value"},
      {0x00034400, "cm_combo_event_inputs"},
      {0x0001f110, "mm_locks_event_selection_4"},
      {0x000a06c0, "mem_dma_s2mm_current_write_count_4"},
      {0x000a06bc, "mem_dma_s2mm_current_write_count_3"},
      {0x0003f15c, "cm_stream_switch_slave_config_aie_trace"},
      {0x0001f10c, "mm_locks_event_selection_3"},
      {0x10, "shim_lock_step_size"},
      {0x000a06b4, "mem_dma_s2mm_current_write_count_1"},
      {0x000a06b0, "mem_dma_s2mm_current_write_count_0"},
      {0x00034070, "cm_event_broadcast_block_north_set"},
      {0x00034008, "shim_event_generate"},
      {0x000c0100, "mem_lock16_value"},
      {0x000b0818, "mem_stream_switch_deterministic_merge_arb1_ctrl"},
      {0x000140fc, "mm_timer_high"},
      {0x000c0360, "mem_lock54_value"},
      {0x8, "shim_dma_s2mm_step_size"},
      {0x00014500, "mm_event_group_0_enable"},
      {0x0001df1c, "mm_dma_s2mm_current_write_count_1"},
      {0x0001df18, "mm_dma_s2mm_current_write_count_0"},
      {0x000a0628, "mem_dma_s2mm_5_ctrl"},
      {0x0001d204, "shim_dma_s2mm_0_task_queue"},
      {0x000a047c, "mem_dma_bd35_7"},
      {0x000940c4, "mem_event_broadcast_b_block_east_clr"},
      {0x000340e0, "cm_trace_event0"},
      {0x000340e4, "cm_trace_event1"},
      {0x000c0050, "mem_lock5_value"},
      {0x00094048, "mem_event_broadcast14"},
      {0x000b0238, "mem_stream_switch_slave_dma_3_slot2"},
      {0x000b023c, "mem_stream_switch_slave_dma_3_slot3"},
      {0x000b0230, "mem_stream_switch_slave_dma_3_slot0"},
      {0x000b0234, "mem_stream_switch_slave_dma_3_slot1"},
      {0x000c03d0, "mem_lock61_value"},
      {0x000c03f0, "mem_lock63_value"},
      {0x0003f2f8, "cm_stream_switch_slave_north_0_slot2"},
      {0x0003f2fc, "cm_stream_switch_slave_north_0_slot3"},
      {0x0003f2f0, "cm_stream_switch_slave_north_0_slot0"},
      {0x0003f2f4, "cm_stream_switch_slave_north_0_slot1"},
      {0x0001d1c0, "shim_dma_bd14_0"},
      {0x0003f818, "cm_stream_switch_deterministic_merge_arb1_ctrl"},
      {0x0001d1c8, "shim_dma_bd14_2"},
      {0x0001d1cc, "shim_dma_bd14_3"},
      {0x0001d1d0, "shim_dma_bd14_4"},
      {0x0001d1d4, "shim_dma_bd14_5"},
      {0x0001d1d8, "shim_dma_bd14_6"},
      {0x0001d1dc, "shim_dma_bd14_7"},
      {0x000b0314, "mem_stream_switch_slave_trace_slot1"},
      {0x000b0310, "mem_stream_switch_slave_trace_slot0"},
      {0x000b031c, "mem_stream_switch_slave_trace_slot3"},
      {0x000b0318, "mem_stream_switch_slave_trace_slot2"},
      {0x0001d15c, "shim_dma_bd10_7"},
      {0x000c0120, "mem_lock18_value"},
      {0x000a04fc, "mem_dma_bd39_7"},
      {0x00094008, "mem_event_generate"},
      {0x0001f0f0, "mm_lock15_value"},
      {0x00014080, "shim_lock8_value"},
      {0x0003f004, "cm_stream_switch_master_config_dma0"},
      {0x0003f008, "cm_stream_switch_master_config_dma1"},
      {0x0001d090, "shim_dma_bd4_4"},
      {0x0001d094, "shim_dma_bd4_5"},
      {0x0001d098, "shim_dma_bd4_6"},
      {0x0001d09c, "shim_dma_bd4_7"},
      {0x0001d080, "shim_dma_bd4_0"},
      {0x0001d084, "shim_dma_bd4_1"},
      {0x0001d088, "shim_dma_bd4_2"},
      {0x0001d08c, "shim_dma_bd4_3"},
      {0x0001d058, "shim_dma_bd2_6"},
      {0x0001d05c, "shim_dma_bd2_7"},
      {0x0001d050, "shim_dma_bd2_4"},
      {0x0001d054, "shim_dma_bd2_5"},
      {0x0001d048, "shim_dma_bd2_2"},
      {0x0001d04c, "shim_dma_bd2_3"},
      {0x0001d040, "shim_dma_bd2_0"},
      {0x000b0118, "mem_stream_switch_slave_config_tile_ctrl"},
      {0x00014218, "mm_reserved2"},
      {0x00014060, "shim_lock6_value"},
      {0x0003f384, "cm_stream_switch_slave_mem_trace_slot1"},
      {0x0003f124, "cm_stream_switch_slave_config_south_4"},
      {0x0003f38c, "cm_stream_switch_slave_mem_trace_slot3"},
      {0x0003f388, "cm_stream_switch_slave_mem_trace_slot2"},
      {0x000a0644, "mem_dma_mm2s_2_start_queue"},
      {0x0003f114, "cm_stream_switch_slave_config_south_0"},
      {0x0003f120, "cm_stream_switch_slave_config_south_3"},
      {0x0003f11c, "cm_stream_switch_slave_config_south_2"},
      {0x000a0348, "mem_dma_bd26_2"},
      {0x000a034c, "mem_dma_bd26_3"},
      {0x000a0340, "mem_dma_bd26_0"},
      {0x000a0344, "mem_dma_bd26_1"},
      {0x000a0358, "mem_dma_bd26_6"},
      {0x000a035c, "mem_dma_bd26_7"},
      {0x000a0350, "mem_dma_bd26_4"},
      {0x000a0354, "mem_dma_bd26_5"},
      {0x000a0310, "mem_dma_bd24_4"},
      {0x000a0314, "mem_dma_bd24_5"},
      {0x000a0318, "mem_dma_bd24_6"},
      {0x000a031c, "mem_dma_bd24_7"},
      {0x000b02e4, "mem_stream_switch_slave_north_1_slot1"},
      {0x000b02e0, "mem_stream_switch_slave_north_1_slot0"},
      {0x000b02ec, "mem_stream_switch_slave_north_1_slot3"},
      {0x000b02e8, "mem_stream_switch_slave_north_1_slot2"},
      {0x0001d0c8, "shim_dma_bd6_2"},
      {0x000a02dc, "mem_dma_bd22_7"},
      {0x0001d0c0, "shim_dma_bd6_0"},
      {0x000a02d4, "mem_dma_bd22_5"},
      {0x000a02c8, "mem_dma_bd22_2"},
      {0x000a02cc, "mem_dma_bd22_3"},
      {0x000a02c0, "mem_dma_bd22_0"},
      {0x000a02c4, "mem_dma_bd22_1"},
      {0x0001f0e0, "mm_lock14_value"},
      {0x0001d1ac, "shim_dma_bd13_3"},
      {0x0001d1a8, "shim_dma_bd13_2"},
      {0x0001d1a4, "shim_dma_bd13_1"},
      {0x0001d1a0, "shim_dma_bd13_0"},
      {0x0001d1bc, "shim_dma_bd13_7"},
      {0x0001d1b8, "shim_dma_bd13_6"},
      {0x0001d1b4, "shim_dma_bd13_5"},
      {0x0001d1b0, "shim_dma_bd13_4"},
      {0x000c0270, "mem_lock39_value"},
      {0x0001d174, "shim_dma_bd11_5"},
      {0x000c0280, "mem_lock40_value"},
      {0x0001d17c, "shim_dma_bd11_7"},
      {0x0001d178, "shim_dma_bd11_6"},
      {0x0001d164, "shim_dma_bd11_1"},
      {0x0001d160, "shim_dma_bd11_0"},
      {0x0001d16c, "shim_dma_bd11_3"},
      {0x0001d168, "shim_dma_bd11_2"},
      {0x0001d124, "shim_dma_bd9_1"},
      {0x0001d120, "shim_dma_bd9_0"},
      {0x0001d12c, "shim_dma_bd9_3"},
      {0x0001d128, "shim_dma_bd9_2"},
      {0x0001d134, "shim_dma_bd9_5"},
      {0x0001d130, "shim_dma_bd9_4"},
      {0x0001d13c, "shim_dma_bd9_7"},
      {0x0001d138, "shim_dma_bd9_6"},
      {0x00034050, "shim_event_broadcast_a_block_south_set"},
      {0x00034064, "cm_event_broadcast_block_west_clr"},
      {0x0001d024, "shim_dma_bd1_1"},
      {0x0001d020, "shim_dma_bd1_0"},
      {0x0001d02c, "shim_dma_bd1_3"},
      {0x0001d028, "shim_dma_bd1_2"},
      {0x0001d034, "shim_dma_bd1_5"},
      {0x0001d030, "shim_dma_bd1_4"},
      {0x0001d03c, "shim_dma_bd1_7"},
      {0x0001d038, "shim_dma_bd1_6"},
      {0x0001f070, "mm_lock7_value"},
      {0x0001f0c0, "mm_lock12_value"},
      {0x000b0250, "mem_stream_switch_slave_dma_5_slot0"},
      {0x000b0254, "mem_stream_switch_slave_dma_5_slot1"},
      {0x000b0258, "mem_stream_switch_slave_dma_5_slot2"},
      {0x000c0330, "mem_lock51_value"},
      {0x0003f808, "cm_stream_switch_deterministic_merge_arb0_ctrl"},
      {0x00091008, "mem_performance_control2"},
      {0x00091000, "mem_performance_control0"},
      {0x00091004, "mem_performance_control1"},
      {0x00094090, "mem_event_broadcast_b_block_south_set"},
      {0x000c0300, "mem_lock48_value"},
      {0x000340d8, "cm_trace_status"},
      {0x00091080, "mem_performance_counter0_event_value"},
      {0x000140f8, "mm_timer_low"},
      {0x000c00e0, "mem_lock14_value"},
      {0x000940f4, "mem_timer_trig_event_high_value"},
      {0x000b0038, "mem_stream_switch_master_config_north3"},
      {0x000b0034, "mem_stream_switch_master_config_north2"},
      {0x0003f284, "cm_stream_switch_slave_south_3_slot1"},
      {0x0003f280, "cm_stream_switch_slave_south_3_slot0"},
      {0x000a0640, "mem_dma_mm2s_2_ctrl"},
      {0x00034008, "cm_event_generate"},
      {0x000c00b0, "mem_lock11_value"},
      {0x000c0090, "mem_lock9_value"},
      {0x00092120, "mem_ecc_failing_address"},
      {0x00012000, "mm_checkbit_error_generation"},
      {0x000a03a4, "mem_dma_bd29_1"},
      {0x000a03a0, "mem_dma_bd29_0"},
      {0x000a03ac, "mem_dma_bd29_3"},
      {0x000a03a8, "mem_dma_bd29_2"},
      {0x000a03b4, "mem_dma_bd29_5"},
      {0x000a03b0, "mem_dma_bd29_4"},
      {0x000a03bc, "mem_dma_bd29_7"},
      {0x000a03b8, "mem_dma_bd29_6"},
      {0x000fff10, "mem_module_reset_control"},
      {0x000a02a4, "mem_dma_bd21_1"},
      {0x000a02a0, "mem_dma_bd21_0"},
      {0x000a02ac, "mem_dma_bd21_3"},
      {0x000a02a8, "mem_dma_bd21_2"},
      {0x000a02b4, "mem_dma_bd21_5"},
      {0x000a02b0, "mem_dma_bd21_4"},
      {0x000a02bc, "mem_dma_bd21_7"},
      {0x000a02b8, "mem_dma_bd21_6"},
      {0x00036070, "cm_memory_control"},
      {0x000b02cc, "mem_stream_switch_slave_south_5_slot3"},
      {0x000b02c8, "mem_stream_switch_slave_south_5_slot2"},
      {0x000b02c4, "mem_stream_switch_slave_south_5_slot1"},
      {0x000b02c0, "mem_stream_switch_slave_south_5_slot0"},
      {0x00094404, "mem_combo_event_control"},
      {0x20, "shim_dma_bd_step_size"},
      {0x00032110, "cm_ecc_scrubbing_event"},
      {0x00034064, "shim_event_broadcast_a_block_west_clr"},
      {0x000a0658, "mem_dma_mm2s_5_ctrl"},
      {0x000b0138, "mem_stream_switch_slave_config_north_1"},
      {0x000b0134, "mem_stream_switch_slave_config_north_0"},
      {0x000b0140, "mem_stream_switch_slave_config_north_3"},
      {0x000b013c, "mem_stream_switch_slave_config_north_2"},
      {0x00031100, "cm_program_counter"},
      {0x0001d228, "shim_dma_mm2s_status_0"},
      {0x0001d22c, "shim_dma_mm2s_status_1"},
      {0x000a0634, "mem_dma_mm2s_0_start_queue"},
      {0x0001de04, "mm_dma_s2mm_0_start_queue"},
      {0x000340fc, "shim_timer_high"},
      {0x000a06a0, "mem_dma_event_channel_selection"},
      {0x00034520, "cm_event_group_user_event_enable"}
    };
  }
  */
void populateRegValueToNameMap() ;

void populateRegAddrToSizeMap() ;


};

/*************************************************************************************
 AIE2ps Registers
 *************************************************************************************/
class AIE2psUsedRegisters : public UsedRegisters {
public:
  AIE2psUsedRegisters() {
    populateRegNameToValueMap();
    populateRegValueToNameMap();
    populateRegAddrToSizeMap();
  }
  //~AIE2psUsedRegisters() = default;
  virtual ~AIE2psUsedRegisters() {}

  void populateProfileRegisters();

  void populateTraceRegisters();

void populateRegNameToValueMap() ;

void populateRegValueToNameMap();
void populateRegAddrToSizeMap() ;

};

} // end XDP namespace

#endif
