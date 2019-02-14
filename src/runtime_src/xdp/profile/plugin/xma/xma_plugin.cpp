/**
 * Copyright (C) 2016-2017 Xilinx, Inc
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

#include "xma_plugin.h"
#include "xdp/rt_singleton.h"
#include "xdp/profile/writer/base_profile.h"
#include "xdp/profile/core/rt_profile.h"

namespace xdp {
  // XML XDP Plugin constructor
  XmaPlugin::XmaPlugin(xclDeviceHandle s_handle)
    : mDeviceHandle(s_handle)
  {
  }

  // **********
  // Trace time
  // **********
  uint64_t XmaPlugin::getTimeNsec()
  {
    // Get trace time similar to XRT
    static auto zero = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now-zero).count();
  }

  double XmaPlugin::getTraceTime()
  {
    uint64_t nsec = getTimeNsec();
    return getTimestampMsec(nsec);
  }

  // *************************
  // Accelerator port metadata
  // *************************

  // Find arguments and memory resources for each accel port on given device
  void XmaPlugin::setArgumentsBank(const std::string& deviceName)
  {
    // TODO: figure out how to get arguments and memory resources in XMA
  }

  // Get the arguments and memory resource for a given device/CU/port
  void XmaPlugin::getArgumentsBank(const std::string& deviceName, const std::string& cuName,
   	                                const std::string& portName, std::string& argNames,
   				                    std::string& memoryName)
  {
    argNames = "All";
    memoryName = "DDR";

    bool foundMemory = false;
    std::string portNameCheck = portName;

    size_t index = portName.find_last_of(PORT_MEM_SEP);
    if (index != std::string::npos) {
      foundMemory = true;
      portNameCheck = portName.substr(0, index);
      memoryName = portName.substr(index+1);
    }
    std::transform(portNameCheck.begin(), portNameCheck.end(), portNameCheck.begin(), ::tolower);

    // Find CU and port, then capture arguments and bank
    for (auto& row : CUPortVector) {
      std::string currCU   = std::get<0>(row);
      std::string currPort = std::get<1>(row);

      if ((currCU == cuName) && (currPort == portNameCheck)) {
        argNames = std::get<2>(row);
        // If already found, replace it; otherwise, use it
        if (foundMemory)
          std::get<3>(row) = memoryName;
        else
          memoryName = std::get<3>(row);
        break;
      }
    }
  }

  // *****************
  // Guidance metadata
  // *****************

  // Gather statistics and put into param/value map
  // NOTE: this needs to be called while the platforms and devices still exist
  void XmaPlugin::getGuidanceMetadata(RTProfile *profile)
  {
    // 1. Device execution times (and unused devices)
    getDeviceExecutionTimes(profile);

    // 2. Unused CUs
    getUnusedComputeUnits(profile);

    // 3. Kernel counts
    getKernelCounts(profile);
  }

  void XmaPlugin::getDeviceExecutionTimes(RTProfile *profile)
  {
    auto platform = xdp::RTSingleton::Instance()->getcl_platform_id();

    // Traverse all devices in this platform
    for (auto device_id : platform->get_device_range()) {
      std::string deviceName = device_id->get_unique_name();

      // Get execution time for this device
      // NOTE: if unused, then this returns 0.0
      double deviceExecTime = profile->getTotalKernelExecutionTime(deviceName);
      mDeviceExecTimesMap[deviceName] = std::to_string(deviceExecTime);
    }
  }

  void XmaPlugin::getUnusedComputeUnits(RTProfile *profile)
  {
    auto platform = xdp::RTSingleton::Instance()->getcl_platform_id();

    // Traverse all devices in this platform
    for (auto device_id : platform->get_device_range()) {
      std::string deviceName = device_id->get_unique_name();

      // Traverse all CUs on current device
      for (auto& cu : xocl::xocl(device_id)->get_cus()) {
        auto cuName = cu->get_name();

        // Get number of calls for current CU
        int numCalls = profile->getComputeUnitCalls(deviceName, cuName);
        std::string cuFullName = deviceName + "|" + cuName;
        mComputeUnitCallsMap[cuFullName] = std::to_string(numCalls);
      }
    }
  }

  void XmaPlugin::getKernelCounts(RTProfile *profile)
  {
    auto platform = xdp::RTSingleton::Instance()->getcl_platform_id();

    // Traverse all devices in this platform
    for (auto device_id : platform->get_device_range()) {
      std::string deviceName = device_id->get_unique_name();

      // Traverse all CUs on current device
      for (auto& cu : xocl::xocl(device_id)->get_cus()) {
        auto kernelName = cu->get_kernel_name();

        if (mKernelCountsMap.find(kernelName) == mKernelCountsMap.end())
          mKernelCountsMap[kernelName] = 1;
        else
          mKernelCountsMap[kernelName] += 1;
      }
    }
  }

  // ****************************************
  // Platform Metadata required by profiler
  // ****************************************

  void XmaPlugin::getProfileKernelName(const std::string& deviceName, const std::string& cuName, std::string& kernelName)
  {
    //xoclp::platform::get_profile_kernel_name(mPlatformHandle, deviceName, cuName, kernelName);
    kernelName = cuName;
  }

  void XmaPlugin::getTraceStringFromComputeUnit(const std::string& deviceName, const std::string& cuName,
                                                 std::string& traceString)
  {
    auto iter = mComputeUnitKernelTraceMap.find(cuName);
    if (iter != mComputeUnitKernelTraceMap.end()) {
      traceString = iter->second;
    }
    else {
      // CR 1003380 - Runtime does not send all CU Names so we create a key
      std::string kernelName;
      getProfileKernelName(deviceName, cuName, kernelName);
      for (const auto &pair : mComputeUnitKernelTraceMap) {
        auto fullName = pair.second;
        auto first_index = fullName.find_first_of("|");
        auto second_index = fullName.find('|', first_index+1);
        auto third_index = fullName.find('|', second_index+1);
        auto fourth_index = fullName.find("|", third_index+1);
        auto fifth_index = fullName.find("|", fourth_index+1);
        auto sixth_index = fullName.find_last_of("|");
        std::string currKernelName = fullName.substr(third_index + 1, fourth_index - third_index - 1);
        if (currKernelName == kernelName) {
          traceString = fullName.substr(0,fifth_index + 1) + cuName + fullName.substr(sixth_index);
          return;
        }
      }
      traceString = std::string();
    }
  }

  void XmaPlugin::setTraceStringForComputeUnit(const std::string& cuName, std::string& traceString)
  {
    mComputeUnitKernelTraceMap[cuName] = traceString;
  }

  size_t XmaPlugin::getDeviceTimestamp(std::string& deviceName)
  {
    //return xoclp::platform::get_device_timestamp(mPlatformHandle,deviceName);
    return 0;
  }

  double XmaPlugin::getReadMaxBandwidthMBps()
  {
    return xclGetReadMaxBandwidthMBps(mDeviceHandle);
  }

  double XmaPlugin::getWriteMaxBandwidthMBps()
  {
    return xclGetWriteMaxBandwidthMBps(mDeviceHandle);
  }

  unsigned XmaPlugin::getProfileNumberSlots(xclPerfMonType type, std::string& deviceName)
  {
    return xclGetProfilingNumberSlots(mDeviceHandle, type);
  }

  void XmaPlugin::getProfileSlotName(xclPerfMonType type, std::string& deviceName,
                                      unsigned slotnum, std::string& slotName)
  {
    char name[128];
    xclGetProfilingSlotName(mDeviceHandle, type, slotnum, name, 128);
    slotName = name;
  }

  unsigned XmaPlugin::getProfileSlotProperties(xclPerfMonType type, std::string& deviceName,
                                                unsigned slotnum)
  {
    return xclGetProfilingSlotProperties(mDeviceHandle, type, slotnum);
  }

  void XmaPlugin::sendMessage(const std::string &msg)
  {
    std::cerr << "WARNING: " << msg << std::endl;
  }
} // xdp

