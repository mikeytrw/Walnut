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

	// VK_KHR_dynamic_rendering. The instance declares Vulkan 1.2, where
	// dynamic rendering is an extension rather than core, so these must be
	// the KHR-suffixed entry points fetched through the device. Calling the
	// core 1.3 names (vkCmdBeginRendering) links and appears to work, because
	// the loader reaches a driver that supports 1.3 -- but with a validation
	// layer in the dispatch chain the 1.3 slot is empty for a 1.2 instance
	// and the call jumps through a null pointer.
	PFN_vkCmdBeginRenderingKHR                       CmdBeginRenderingKHR = nullptr;
	PFN_vkCmdEndRenderingKHR                         CmdEndRenderingKHR = nullptr;
};

// Global singleton populated by Walnut::Application::SetupVulkan.
extern RTDispatch g_RTDispatch;

void RTDispatchInit(VkDevice device);

#endif // RT_DISPATCH_H