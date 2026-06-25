#pragma once

#ifndef RT_DISPATCH_H
#define RT_DISPATCH_H

#include "vulkan/vulkan.h"

// Cached KHR ray-tracing entry points, loaded once at device creation
// via vkGetDeviceProcAddr (device-level) and stored here so every call
// site avoids the per-call vkGetInstanceProcAddr lookup that the
// original code did.
struct RTDispatch
{
	// VK_KHR_acceleration_structure
	PFN_vkCreateAccelerationStructureKHR             CreateAccelerationStructureKHR = nullptr;
	PFN_vkDestroyAccelerationStructureKHR            DestroyAccelerationStructureKHR = nullptr;
	PFN_vkGetAccelerationStructureDeviceAddressKHR   GetAccelerationStructureDeviceAddressKHR = nullptr;
	PFN_vkGetAccelerationStructureBuildSizesKHR      GetAccelerationStructureBuildSizesKHR = nullptr;
	PFN_vkCmdBuildAccelerationStructuresKHR          CmdBuildAccelerationStructuresKHR = nullptr;
	PFN_vkCmdCopyAccelerationStructureKHR            CmdCopyAccelerationStructureKHR = nullptr;

	// VK_KHR_ray_tracing_pipeline
	PFN_vkCreateRayTracingPipelinesKHR               CreateRayTracingPipelinesKHR = nullptr;
	PFN_vkCmdTraceRaysKHR                            CmdTraceRaysKHR = nullptr;
	PFN_vkGetRayTracingShaderGroupHandlesKHR         GetRayTracingShaderGroupHandlesKHR = nullptr;
};

// Global singleton populated by Walnut::Application::SetupVulkan.
extern RTDispatch g_RTDispatch;

void RTDispatchInit(VkDevice device);

#endif // RT_DISPATCH_H