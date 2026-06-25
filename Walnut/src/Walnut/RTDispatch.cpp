#include "RTDispatch.h"
#include "vulkan/vulkan.h"

RTDispatch g_RTDispatch;

void RTDispatchInit(VkDevice device)
{
	RTDispatch d = {};

#define LOAD(name) d.name = (PFN_vk##name)vkGetDeviceProcAddr(device, "vk" #name)

	// VK_KHR_acceleration_structure
	LOAD(CreateAccelerationStructureKHR);
	LOAD(DestroyAccelerationStructureKHR);
	LOAD(GetAccelerationStructureDeviceAddressKHR);
	LOAD(GetAccelerationStructureBuildSizesKHR);
	LOAD(CmdBuildAccelerationStructuresKHR);
	LOAD(CmdCopyAccelerationStructureKHR);

	// VK_KHR_ray_tracing_pipeline
	LOAD(CreateRayTracingPipelinesKHR);
	LOAD(CmdTraceRaysKHR);
	LOAD(GetRayTracingShaderGroupHandlesKHR);

#undef LOAD

	g_RTDispatch = d;
}