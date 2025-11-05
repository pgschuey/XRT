/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. - All rights reserved
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

#define XDP_PLUGIN_SOURCE

#include "aie_halt_npu3.h"
#include "xdp/profile/plugin/vp_base/utility.h"
#include "xdp/profile/device/common/npu3/npu3_transaction.h"
#include "xdp/profile/plugin/aie_base/generations/npu3_registers.h"

#include "core/common/api/hw_context_int.h"
#include "core/common/device.h"
#include "core/common/message.h"

#include "core/include/xrt/experimental/xrt_elf.h"
#include "core/include/xrt/experimental/xrt_ext.h"
#include "core/include/xrt/experimental/xrt_module.h"
#include "core/include/xrt/xrt_kernel.h"

extern "C" {
  #include <xaiengine.h>
  #include <xaiengine/xaiemlgbl_params.h>
}

#ifdef _WIN32
# pragma warning ( disable : 4244 )
#endif

namespace xdp {

  AIEHaltNPU3Impl::AIEHaltNPU3Impl(VPDatabase* dB)
    : AIEHaltImpl(dB)
  {
  }

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4702)
#endif
  void AIEHaltNPU3Impl::updateDevice(void* hwCtxImpl)
  {
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
              "In AIEHaltNPU3Impl::updateDevice");

    xrt::hw_context hwContext = xrt_core::hw_context_int::create_hw_context_from_implementation(hwCtxImpl);

    // Two ways to run:
    //   1. Use pre-created elf
    //   2. Have plugin create elf and tranx 
    std::string inputCtrlCode = xrt_core::config::get_aie_halt_settings_control_code();
    if (!inputCtrlCode.empty()) {
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
          "Using AIE Halt control code " + inputCtrlCode);

      xrt::elf haltElf;
      try {
        haltElf = xrt::elf(inputCtrlCode);
      } catch (...) {
        std::string msg = "Failed to load " + inputCtrlCode + ". Cannot configure AIE to halt.";
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg);
        return;
      }

      xrt::module mod{haltElf};
      xrt::kernel krnl;
      try {
        krnl = xrt::ext::kernel{hwContext, mod, "XDP_KERNEL:{IPUV1CNN}"};
      } catch (...) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT",
                  "XDP_KERNEL not found in HW Context. Cannot configure AIE to halt.");
        return;
      }

      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                "In AIEHaltNPU3Impl New Kernel Object for XDP_KERNEL created for running control code Elf");      

      xrt::run rn{krnl};
      rn.start();
      xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
                "In AIEHaltNPU3Impl run start, going to wait");  

      rn.wait2();
      xrt_core::message::send(severity_level::info, "XRT", "Successfully scheduled AIE Halt.");
      return;
    }
    
    // Create and submit control code to halt all cores
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
        "Creating AIE Halt control code");

    // Configure driver
    boost::property_tree::ptree aieMetadata;
    try {
      auto device = xrt_core::hw_context_int::get_core_device(hwContext);
      xrt::xclbin xrtXclbin = device.get()->get_xclbin(device.get()->get_xclbin_uuid());
      auto data = xrt_core::xclbin_int::get_axlf_section(xrtXclbin, AIE_METADATA);

      if (!data.first || !data.second) {
        xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", "Empty AIE Metadata in xclbin");
        return;
      }

      std::stringstream ss;
      ss.write(data.first,data.second);

      boost::property_tree::read_json(ss, aieMetadata);
    } catch (const std::exception& e) {
      std::string msg("AIE Metadata could not be read/processed from xclbin: ");
      msg += e.what();
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", msg);
      return;
    }

    xdp::aie::driver_config meta_config = xdp::aie::getDriverConfig(aieMetadata, "aie_metadata.driver_config");

    XAie_Config cfg {
      meta_config.hw_gen,
      meta_config.base_address,
      meta_config.column_shift,
      meta_config.row_shift,
      meta_config.num_rows,
      meta_config.num_columns,
      meta_config.shim_row,
      0,
      1,
      meta_config.mem_row_start,
      meta_config.mem_num_rows,
      meta_config.aie_tile_row_start,
      meta_config.aie_tile_num_rows,
      {0} // PartProp
    };
    XAie_DevInst aieDevInst = {0};
    auto RC = XAie_CfgInitialize(&aieDevInst, &cfg);
    if (RC != XAIE_OK) {
      xrt_core::message::send(xrt_core::message::severity_level::warning, "XRT", "AIE Driver Initialization Failed.");
      return;
    }

    // Get partition columns
    //boost::property_tree::ptree aiePartitionPt = xdp::aie::getAIEPartitionInfo(metadata->getHandle());
    // Currently, assuming only one Hw Context is alive at a time
    //uint8_t startCol = static_cast<uint8_t>(aiePartitionPt.front().second.get<uint64_t>("start_col"));
    uint8_t startCol = 0;
    uint8_t numCols = meta_config.num_columns;
    uint8_t startRow = meta_config.aie_tile_row_start;
    uint8_t numRows = meta_config.aie_tile_num_rows;

    std::string tranxName = "AieHalt";
    xrt_core::message::send(xrt_core::message::severity_level::debug, "XRT",
      "Starting transaction " + tranxName);

    // Initialize transaction
    std::unique_ptr<aie::NPU3Transaction> tranxHandler = std::make_unique<aie::NPU3Transaction>(); 
    if (!tranxHandler->initializeTransaction(&aieDevInst, tranxName))
      return false;

    std::stringstream msg;
    msg << " Set AIE Core breakpoint at Lock Acquire Req Instr, Start col "
        << +startCol << ", Num col " << +numCols << std::endl;
    xrt_core::message::send(xrt_core::message::severity_level::info, "XRT", msg.str());

    // Initial break on Event 82: Lock Acquire instruction
    constexpr uint32_t AIE_EVENT_INSTR_LOCK_ACQ_REQ = 0x52;
    uint32_t dbg_ctrl_1_reg = AIE_EVENT_INSTR_LOCK_ACQ_REQ << XAIEMLGBL_CORE_MODULE_DEBUG_CONTROL1_DEBUG_HALT_CORE_EVENT0_LSB;
    
    for (uint8_t col = startCol; col < (startCol + numCols); col++) {
      for (uint8_t row = startRow; row < (startRow + numRows); row++) {
        auto tileOffset = XAie_GetTileAddr(&aieDevInst, row, col);
        XAie_Write32(&aieDevInst, tileOffset + npu3::cm_debug_control1, dbg_ctrl_1_reg);
        //XAie_CoreDebugHalt(&aieDevInst, XAie_TileLoc(col, row));
      }
    }

    tranxHandler->submitTransaction(&aieDevInst, hwContext);
    xrt_core::message::send(severity_level::info, "XRT", "Successfully scheduled AIE Halt.");
  }
#ifdef _WIN32
#pragma warning(pop)
#endif

  void AIEHaltNPU3Impl::finishflushDevice(void* /*hwCtxImpl*/)
  {
  }
}
