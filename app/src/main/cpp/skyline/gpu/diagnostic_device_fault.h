// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "diagnostic_fault_compat.h"
#include <common/diagnostic_recorder.h>
#include <common/diagnostic_session.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <sstream>
#include <vector>
namespace skyline::gpu::diagnostics {
inline VkDevice faultDevice{};
inline PFN_vkGetDeviceFaultInfoEXT faultFunction{};
inline std::atomic_flag faultCaptured = ATOMIC_FLAG_INIT;
inline void InitializeDeviceFault(VkDevice device, PFN_vkGetDeviceProcAddr getProc, bool enabled) noexcept {
    faultDevice=device;faultFunction=enabled?reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(getProc(device,"vkGetDeviceFaultInfoEXT")):nullptr;
    faultCaptured.clear();
    skyline::diagnostics::Feature("device_fault",faultFunction?"enabled; address and vendor records; vendor binary disabled":"unavailable or unsupported");
}
inline void CaptureDeviceFault(VkDevice device) noexcept {
    if(device!=faultDevice || !faultFunction || faultCaptured.test_and_set())return;
    try {
        VkDeviceFaultCountsEXT counts{};counts.sType=VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        auto result=faultFunction(device,&counts,nullptr);
        if(result!=VK_SUCCESS) {
            skyline::diagnostics::WriteJson("device_fault.json","{\"query_result\":"+std::to_string(result)+"}");return;
        }
        const auto requestedAddresses=counts.addressInfoCount, requestedVendors=counts.vendorInfoCount;
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(std::min(counts.addressInfoCount,256U));
        std::vector<VkDeviceFaultVendorInfoEXT> vendors(std::min(counts.vendorInfoCount,64U));
        counts.addressInfoCount=addresses.size();counts.vendorInfoCount=vendors.size();counts.vendorBinarySize=0;
        VkDeviceFaultInfoEXT info{};info.sType=VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        info.pAddressInfos=addresses.data();info.pVendorInfos=vendors.data();
        result=faultFunction(device,&counts,&info);
        std::ostringstream out;
        out<<"{\"query_result\":"<<result<<",\"requested_addresses\":"<<requestedAddresses<<",\"requested_vendors\":"<<requestedVendors
           <<",\"vendor_binary\":\"not_enabled\",\"description\":\""<<skyline::diagnostics::JsonEscape(std::string_view(info.description,strnlen(info.description,sizeof(info.description))))<<"\",\"addresses\":[";
        if(result==VK_SUCCESS||result==VK_INCOMPLETE) {
            for(size_t i=0;i<std::min<size_t>(counts.addressInfoCount,addresses.size());i++){if(i)out<<',';auto&a=addresses[i];out<<"{\"type\":"<<a.addressType<<",\"address\":"<<a.reportedAddress<<",\"precision\":"<<a.addressPrecision<<'}';}
        }
        out<<"],\"vendors\":[";
        if(result==VK_SUCCESS||result==VK_INCOMPLETE) {
            for(size_t i=0;i<std::min<size_t>(counts.vendorInfoCount,vendors.size());i++){if(i)out<<',';auto&v=vendors[i];out<<"{\"description\":\""<<skyline::diagnostics::JsonEscape(std::string_view(v.description,strnlen(v.description,sizeof(v.description))))<<"\",\"code\":"<<v.vendorFaultCode<<",\"data\":"<<v.vendorFaultData<<'}';}
        }
        out<<"]}";skyline::diagnostics::WriteJson("device_fault.json",out.str());
    }catch(...){skyline::diagnostics::Event("device_fault_query_failed");}
}
}
