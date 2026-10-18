#define VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#include <utility>
#include <vulkan/vulkan_raii.hpp>
#include "gpu/diagnostic_device_fault.h"
#include <renderdoc_app.h>
#include <cassert>
static int calls{};
VkResult VKAPI_PTR fakeFault(VkDevice,VkDeviceFaultCountsEXT*counts,VkDeviceFaultInfoEXT*info){
 ++calls;if(!info){counts->addressInfoCount=999;counts->vendorInfoCount=99;return VK_SUCCESS;}
 assert(counts->addressInfoCount==256);assert(counts->vendorInfoCount==64);counts->addressInfoCount=1;counts->vendorInfoCount=1;
 info->pAddressInfos[0]={VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT,1234,4};strcpy(info->description,"fake fault");return VK_INCOMPLETE;
}
PFN_vkVoidFunction VKAPI_PTR getProc(VkDevice,const char*){return reinterpret_cast<PFN_vkVoidFunction>(fakeFault);}
void compileApi(const vk::raii::Context&context,const vk::raii::PhysicalDevice&physicalDevice,const vk::raii::Device&device){
 auto layerExtensions=context.enumerateInstanceExtensionProperties(std::string{"VK_LAYER_KHRONOS_validation"});
 VkPhysicalDeviceFaultFeaturesEXT fault{};fault.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
 VkPhysicalDeviceFeatures2 query{};query.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;query.pNext=&fault;
 physicalDevice.getDispatcher()->vkGetPhysicalDeviceFeatures2(static_cast<VkPhysicalDevice>(*physicalDevice),&query);
 vk::StructureChain<vk::PhysicalDeviceFeatures2,vk::PhysicalDeviceRobustness2FeaturesEXT> enabledFeatures;
 auto&head=enabledFeatures.get<vk::PhysicalDeviceFeatures2>();fault.pNext=head.pNext;head.pNext=&fault;
 skyline::gpu::diagnostics::InitializeDeviceFault(static_cast<VkDevice>(*device),device.getDispatcher()->vkGetDeviceProcAddr,true);
 VkValidationFeaturesEXT validation{};validation.sType=VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
 VkValidationFeatureEnableEXT mode=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;validation.pEnabledValidationFeatures=&mode;
 vk::InstanceCreateInfo info{.pNext=&validation};(void)info;
}
int main(int argc,char**argv){assert(argc==2);skyline::diagnostics::Configure(argv[1]);auto device=reinterpret_cast<VkDevice>(123);skyline::gpu::diagnostics::InitializeDeviceFault(device,getProc,true);skyline::gpu::diagnostics::CaptureDeviceFault(device);skyline::gpu::diagnostics::CaptureDeviceFault(device);assert(calls==2);}
