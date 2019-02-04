/*
 * Copyright (C) 2018, Xilinx Inc - All rights reserved
 * Xilinx SDAccel Media Accelerator API
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

#include "xma_profiler.h"
#include "xma_plugin.h"
#include "xdp/profile/core/rt_profile.h"
#include "xdp/profile/writer/csv_profile.h"
#include "xdp/profile/writer/csv_trace.h"

#include <iostream>
#include <cstdio>
#include <cstring>
#include <thread>

namespace xdp {

  static bool pDead = false;

  XMAProfiler* XMAProfiler::Instance()
  {
    if (pDead) {
      std::cout << "XMAProfiler is dead\n";
      return nullptr;
    }
    static XMAProfiler singleton;
    return &singleton;
  };

  XMAProfiler::XMAProfiler()
    : mProfileFlags(0)
  {
  }

  XMAProfiler::~XMAProfiler()
  {
    pDead = true;
    mPlugin->setObjectsReleased(mProfileFinalized);

    if (!mProfileFinalized && mProfileMgr->isApplicationProfileOn()) {
      mPlugin->sendMessage("Profiling may contain incomplete information. Please ensure all OpenCL objects are released by your host code (e.g., clReleaseProgram()).");

      // Before deleting, do a final read of counters and force flush of trace buffers
      // NOTE: we need to know the device handle
      //profile_finalize();
    }
  }

// ********************
// Initialize Profiling
// ********************
void
XMAProfiler::profile_initialize(xclDeviceHandle s_handle, int use_profile, int use_trace,
                                const char* data_transfer_trace, const char* stall_trace)
{
  printf("profile_initialize: s_handle=%p, use_profile=%d, use_trace=%d, data_transfer_trace=%s, stall_trace=%s\n",
         s_handle, use_profile, use_trace, data_transfer_trace, stall_trace);

  mProfileFlags = xdp::RTUtil::PROFILE_APPLICATION;
  mPlugin = std::make_shared<XmaPlugin>(s_handle);
  // Share ownership to ensure correct order of destruction
  mProfileMgr = std::make_unique<RTProfile>(mProfileFlags, mPlugin);

  // Evaluate arguments
  if (use_trace) {
    std::string dataTraceStr = data_transfer_trace;
    std::string stallTraceStr = stall_trace;
    mProfileMgr->setTransferTrace(dataTraceStr);
    mProfileMgr->setStallTrace(stallTraceStr);

    mTraceOption = 0;
    mTraceOption = (dataTraceStr == "coarse") ? 0x1 : 0x0;
    if (stallTraceStr == "dataflow")    mTraceOption |= (0x1 << 2);
    else if (stallTraceStr == "pipe")   mTraceOption |= (0x1 << 3);
    else if (stallTraceStr == "memory") mTraceOption |= (0x1 << 4);
    else if (stallTraceStr == "all")    mTraceOption |= (0x7 << 2);
  }

  // Get design info (clock freqs, device/binary names)
  xclDeviceInfo2 deviceInfo;
  xclGetDeviceInfo2(s_handle, &deviceInfo);
  mDeviceName = deviceInfo.mName;
  mProfileMgr->addDeviceName(mDeviceName);

  double kernelClockFreq = deviceInfo.mOCLFrequency[0];
  mProfileMgr->setTraceClockFreqMHz(kernelClockFreq);
  mPlugin->setKernelClockFreqMHz(mDeviceName, kernelClockFreq);
  // TODO: do we know this?
  mBinaryName = "binary";

  //
  // Profile Summary
  //
  if (use_profile) {
	mProfileMgr->turnOnProfile(xdp::RTUtil::PROFILE_DEVICE_COUNTERS);
    mProfileMgr->turnOnFile(xdp::RTUtil::FILE_SUMMARY);

    mProfileWriter = new xdp::CSVProfileWriter("xdp_profile_summary", "Xilinx", mPlugin.get());
    mProfileMgr->attach(mProfileWriter);
  }

  //
  // Timeline Trace
  //
  if (use_trace) {
	mProfileMgr->turnOnProfile(xdp::RTUtil::PROFILE_DEVICE_TRACE);
    mProfileMgr->turnOnFile(xdp::RTUtil::FILE_TIMELINE_TRACE);

    mTraceWriter = new CSVTraceWriter("xdp_timeline_trace", "Xilinx", mPlugin.get());
    mProfileMgr->attach(mTraceWriter);

    // Make an initialization call for time
    mPlugin->getTraceTime();
  }
}

// ***************
// Start Profiling
// ***************
// TODO: support multiple devices
void
XMAProfiler::profile_start(xclDeviceHandle s_handle)
{
  printf("profile_start: s_handle=%p\n", s_handle);

  //
  // Profile Summary
  //
  if (mProfileMgr->isDeviceProfileOn()) {
    // Start counters (also reads debug_ip_layout)
    xclPerfMonStartCounters(s_handle, XCL_PERF_MON_MEMORY);

    // Read counters
    xclCounterResults counterResults;
    xclPerfMonReadCounters(s_handle, XCL_PERF_MON_MEMORY, counterResults);
    uint64_t timeNsec = mPlugin->getTimeNsec();
    bool firstReadAfterProgram = true;
    mProfileMgr->logDeviceCounters(mDeviceName, mBinaryName, XCL_PERF_MON_MEMORY,
        counterResults, timeNsec, firstReadAfterProgram);
  }

  //
  // Timeline Trace
  //
  if (mProfileMgr->getProfileFlags() & xdp::RTUtil::PROFILE_DEVICE_TRACE) {
    // Start trace
    xclPerfMonStartTrace(s_handle, XCL_PERF_MON_MEMORY, mTraceOption);
    xclPerfMonStartTrace(s_handle, XCL_PERF_MON_ACCEL, mTraceOption);
  }
}

// **************
// Stop Profiling
// **************
// TODO: support multiple devices
void
XMAProfiler::profile_stop(xclDeviceHandle s_handle)
{
  printf("profile_stop: s_handle=%p\n", s_handle);

  //
  // Profile summary
  //
  if (mProfileMgr->isDeviceProfileOn()) {
    // Read counters
    xclCounterResults counterResults;
    xclPerfMonReadCounters(s_handle, XCL_PERF_MON_MEMORY, counterResults);

    // Store results
    uint64_t timeNsec = mPlugin->getTimeNsec();
    bool firstReadAfterProgram = false;
	mProfileMgr->logDeviceCounters(mDeviceName, mBinaryName, XCL_PERF_MON_MEMORY,
        counterResults, timeNsec, firstReadAfterProgram);

    // Stop counters
    xclPerfMonStopCounters(s_handle, XCL_PERF_MON_MEMORY);
  }

  //
  // Timeline Trace
  //
  if (mProfileMgr->getProfileFlags() & xdp::RTUtil::PROFILE_DEVICE_TRACE) {
    // Data transfers
    xclTraceResultsVector traceVector = {0};
    xclPerfMonReadTrace(s_handle, XCL_PERF_MON_MEMORY, traceVector);
    mProfileMgr->logDeviceTrace(mDeviceName, mBinaryName, XCL_PERF_MON_MEMORY, traceVector);

    // Accelerators
    xclPerfMonReadTrace(s_handle, XCL_PERF_MON_ACCEL, traceVector);
    mProfileMgr->logDeviceTrace(mDeviceName, mBinaryName, XCL_PERF_MON_ACCEL, traceVector);

    // Stop trace
    xclPerfMonStopTrace(s_handle, XCL_PERF_MON_MEMORY);
    xclPerfMonStopTrace(s_handle, XCL_PERF_MON_ACCEL);
  }
}

// ******************
// Finalize Profiling
// ******************
void
XMAProfiler::profile_finalize(xclDeviceHandle s_handle)
{
  if (mProfileFinalized)
    return;
  printf("profile_finalize: s_handle=%p\n", s_handle);

  //
  // Profile summary
  //
  if (mProfileMgr->isDeviceProfileOn()) {
    // Write profile summary
    mProfileMgr->writeProfileSummary();

    // Close writer and delete
    mProfileMgr->detach(mProfileWriter);
    delete mProfileWriter;
  }

  //
  // Timeline Trace
  //
  if (mProfileMgr->getProfileFlags() & xdp::RTUtil::PROFILE_DEVICE_TRACE) {
    // Write footer string
    std::stringstream trs;
    trs << "Project," << mProfileMgr->getProjectName() << ",\n";
    std::string stallProfiling = (mProfileMgr->getStallTrace() == xdp::RTUtil::STALL_TRACE_OFF) ? "false" : "true";
    trs << "Stall profiling," << stallProfiling << ",\n";
    std::string flowMode;
    xdp::RTUtil::getFlowModeName(mPlugin->getFlowMode(), flowMode);
    trs << "Target," << flowMode << ",\n";
    std::string DeviceNames = mProfileMgr->getDeviceNames("|");
    trs << "Platform," << DeviceNames << ",\n";

    for (auto& threadId : mProfileMgr->getThreadIds()) {
      trs << "Read/Write Thread," << std::showbase << std::hex << std::uppercase
          << threadId << std::endl;
    }

#if 0
    //
    // Platform/device info
    //
    //auto platform = getclPlatformID();
    auto platform = xdp::RTSingleton::Instance()->getcl_platform_id();
    for (auto device_id : platform->get_device_range()) {
      std::string mDeviceName = device_id->get_unique_name();
      trs << "Device," << mDeviceName << ",begin\n";

      // DDR Bank addresses
      // TODO: this assumes start address of 0x0 and evenly divided banks
      unsigned int ddrBanks = device_id->get_ddr_bank_count();
      if (ddrBanks == 0) ddrBanks = 1;
      size_t ddrSize = device_id->get_ddr_size();
      size_t bankSize = ddrSize / ddrBanks;
      trs << "DDR Banks,begin\n";
      for (unsigned int b=0; b < ddrBanks; ++b)
        trs << "Bank," << std::dec << b << ","
            << (boost::format("0X%09x") % (b * bankSize)) << std::endl;
      trs << "DDR Banks,end\n";
      trs << "Device," << mDeviceName << ",end\n";
    }

    //
    // Unused CUs
    //
    for (auto device_id : platform->get_device_range()) {
      std::string mDeviceName = device_id->get_unique_name();

      for (auto& cu : xocl::xocl(device_id)->get_cus()) {
        auto cuName = cu->get_name();

        if (mProfileMgr->getComputeUnitCalls(mDeviceName, cuName) == 0)
          trs << "UnusedComputeUnit," << cuName << ",\n";
      }
    }
#endif

    mPlugin->setTraceFooterString(trs.str());

    // Close writer and delete
    mProfileMgr->detach(mTraceWriter);
    delete mTraceWriter;
  }

  mProfileFinalized = true;
}

} // xdp

// **************************
// Host Transfer w/ Profiling
// **************************
int
xclSyncBOWithProfile(xclDeviceHandle s_handle, unsigned int boHandle, xclBOSyncDirection dir,
                     size_t size, size_t offset)
{
  printf("xclSyncBOWithProfile: s_handle=%p, boHandle=%u, dir=%d, size=%lu, offset=%lu\n",
         s_handle, boHandle, dir, size, offset);

  int rc;
  xclBOProperties p;
  uint64_t boAddr = !xclGetBOProperties(s_handle, boHandle, &p) ? p.paddr : -1;

  uint64_t boHandleLong = static_cast<uint64_t>(boHandle);
  xdp::RTUtil::e_profile_command_kind objKind =
      (dir == XCL_BO_SYNC_BO_TO_DEVICE) ? xdp::RTUtil::WRITE_BUFFER : xdp::RTUtil::READ_BUFFER;

  // TODO: Are these applicable to XMA/xfDNN?
  uint32_t contextId = 0;
  uint32_t numDevices = 1;
  uint32_t commandQueueId = 0;
  std::string ddrBank = "DDR[0]";

  auto profiler = xdp::XMAProfiler::Instance();
  auto profileMgr = profiler->getProfileManager();

  std::string deviceName;
  profiler->getDeviceName(deviceName);

  printf("logging START of host data transfer\n");
  profileMgr->logDataTransfer(
     boHandleLong
    ,objKind
    ,xdp::RTUtil::START
    ,size
    ,contextId
    ,numDevices
    ,deviceName
    ,commandQueueId
    ,boAddr
    ,ddrBank
    ,std::this_thread::get_id());

  rc = xclSyncBO(s_handle, boHandle, dir, size, offset);

  printf("logging END of host data transfer\n");
  profileMgr->logDataTransfer(
     boHandleLong
    ,objKind
    ,xdp::RTUtil::END
    ,size
    ,contextId
    ,numDevices
    ,deviceName
    ,commandQueueId
    ,boAddr
    ,ddrBank
    ,std::this_thread::get_id());

  return rc;
}
