// SPDX-License-Identifier: Apache-2.0
// Copyright 2015-2024 The Khronos Group Inc.
// VK_EXT_device_fault definitions from Khronos Vulkan-Headers v1.3.280.
// https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.3.280/include/vulkan/vulkan_core.h
#pragma once
#include <vulkan/vulkan.h>
#ifndef VK_EXT_device_fault
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT static_cast<VkStructureType>(1000341000)
#define VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT static_cast<VkStructureType>(1000341001)
#define VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT static_cast<VkStructureType>(1000341002)
#define VK_EXT_device_fault 1
#define VK_EXT_DEVICE_FAULT_SPEC_VERSION  2
#define VK_EXT_DEVICE_FAULT_EXTENSION_NAME "VK_EXT_device_fault"

typedef enum VkDeviceFaultAddressTypeEXT {
    VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT = 0,
    VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT = 1,
    VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT = 2,
    VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT = 3,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT = 4,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT = 5,
    VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT = 6,
    VK_DEVICE_FAULT_ADDRESS_TYPE_MAX_ENUM_EXT = 0x7FFFFFFF
} VkDeviceFaultAddressTypeEXT;

typedef enum VkDeviceFaultVendorBinaryHeaderVersionEXT {
    VK_DEVICE_FAULT_VENDOR_BINARY_HEADER_VERSION_ONE_EXT = 1,
    VK_DEVICE_FAULT_VENDOR_BINARY_HEADER_VERSION_MAX_ENUM_EXT = 0x7FFFFFFF
} VkDeviceFaultVendorBinaryHeaderVersionEXT;
typedef struct VkPhysicalDeviceFaultFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           deviceFault;
    VkBool32           deviceFaultVendorBinary;
} VkPhysicalDeviceFaultFeaturesEXT;

typedef struct VkDeviceFaultCountsEXT {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           addressInfoCount;
    uint32_t           vendorInfoCount;
    VkDeviceSize       vendorBinarySize;
} VkDeviceFaultCountsEXT;

typedef struct VkDeviceFaultAddressInfoEXT {
    VkDeviceFaultAddressTypeEXT    addressType;
    VkDeviceAddress                reportedAddress;
    VkDeviceSize                   addressPrecision;
} VkDeviceFaultAddressInfoEXT;

typedef struct VkDeviceFaultVendorInfoEXT {
    char        description[VK_MAX_DESCRIPTION_SIZE];
    uint64_t    vendorFaultCode;
    uint64_t    vendorFaultData;
} VkDeviceFaultVendorInfoEXT;

typedef struct VkDeviceFaultInfoEXT {
    VkStructureType                 sType;
    void*                           pNext;
    char                            description[VK_MAX_DESCRIPTION_SIZE];
    VkDeviceFaultAddressInfoEXT*    pAddressInfos;
    VkDeviceFaultVendorInfoEXT*     pVendorInfos;
    void*                           pVendorBinaryData;
} VkDeviceFaultInfoEXT;

typedef struct VkDeviceFaultVendorBinaryHeaderVersionOneEXT {
    uint32_t                                     headerSize;
    VkDeviceFaultVendorBinaryHeaderVersionEXT    headerVersion;
    uint32_t                                     vendorID;
    uint32_t                                     deviceID;
    uint32_t                                     driverVersion;
    uint8_t                                      pipelineCacheUUID[VK_UUID_SIZE];
    uint32_t                                     applicationNameOffset;
    uint32_t                                     applicationVersion;
    uint32_t                                     engineNameOffset;
    uint32_t                                     engineVersion;
    uint32_t                                     apiVersion;
} VkDeviceFaultVendorBinaryHeaderVersionOneEXT;

typedef VkResult (VKAPI_PTR *PFN_vkGetDeviceFaultInfoEXT)(VkDevice device, VkDeviceFaultCountsEXT* pFaultCounts, VkDeviceFaultInfoEXT* pFaultInfo);


#endif
