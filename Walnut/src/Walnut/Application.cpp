#include "Application.h"

//
// Adapted from Dear ImGui Vulkan example
//

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "RTDispatch.h"
#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // abort
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <iostream>
#include <exception>
#include <optional>
#include <unordered_set>
#include <iterator>

// Emedded font
#include "ImGui/Roboto-Regular.embed"

extern bool g_ApplicationRunning;

// [Win32] Our example includes a copy of glfw3.lib pre-compiled with VS2010 to maximize ease of testing and compatibility with old VS compilers.
// To link with VS2010-era libraries, VS2015+ requires linking with legacy_stdio_definitions.lib, which we do using this pragma.
// Your own project should not be affected, as you are likely to link with a newer binary of GLFW that is adequate for your version of Visual Studio.
#if defined(_MSC_VER) && (_MSC_VER >= 1900) && !defined(IMGUI_DISABLE_WIN32_FUNCTIONS)
#pragma comment(lib, "legacy_stdio_definitions")
#endif

//#define IMGUI_UNLIMITED_FRAME_RATE

// Validation layers are opt-in via --validate (ApplicationSpecification::
// EnableValidation), never implied by the build configuration.
//
// This used to be `#ifdef _DEBUG`, which forced validation on for every Debug
// run. Two reasons it is gone rather than re-expressed as WL_DEBUG:
//
// 1. Debug now builds against the release CRT so it can link the prebuilt
//    /MD NRD/NRI libraries, so _DEBUG is no longer defined and the coupling
//    would have broken silently -- Debug would have stopped enabling
//    validation with nothing to say so.
// 2. Tying a Vulkan diagnostic to the CRT choice was always the wrong seam.
//    --validate is explicit, works in both configurations, and is the flag
//    the headless gates already use.
//
// If you want validation, pass --validate. Defining IMGUI_VULKAN_DEBUG_REPORT
// here still forces it on unconditionally, as before.

// Runtime validation override (set via ApplicationSpecification::EnableValidation)
static bool g_EnableRuntimeValidation = false;
static bool g_EnableSyncValidation = false;

static VkAllocationCallbacks* g_Allocator = NULL;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;
static bool                     g_RayTracingSupported = false;
static bool                     g_RayTracingPipelineSupported = false;
static VkPhysicalDeviceRayTracingPipelinePropertiesKHR g_RTPipelineProperties = {};

static ImGui_ImplVulkanH_Window g_MainWindowData;
static int                      g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

// Per-frame-in-flight
static std::vector<std::vector<VkCommandBuffer>> s_AllocatedCommandBuffers;
static std::vector<std::vector<std::function<void()>>> s_ResourceFreeQueue;

// Unlike g_MainWindowData.FrameIndex, this is not the the swapchain image index
// and is always guaranteed to increase (eg. 0, 1, 2, 0, 1, 2)
static uint32_t s_CurrentFrameIndex = 0;

static Walnut::Application* s_Instance = nullptr;

void check_vk_result(VkResult err)
{
	if (err == 0)
		return;
	fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
	fprintf(stderr, "[vulkan] Crashing. Press Enter to exit...\n");
	getchar();
	if (err < 0)
		abort();
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData)
{
	(void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix; // Unused arguments
	fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
	return VK_FALSE;
}

static void PublishOptionalDiagnostic(
	const Walnut::ApplicationSpecification& specification,
	std::vector<Walnut::OptionalVulkanFeatureDiagnostic>& diagnostics,
	Walnut::OptionalVulkanFeatureDiagnostic diagnostic)
{
	Walnut::PublishOptionalVulkanDiagnostic(
		diagnostics,
		std::move(diagnostic),
		specification.optionalVulkanFeatureDiagnosticSink,
		[](const Walnut::OptionalVulkanFeatureDiagnostic& logged) {
			std::cerr << "[Walnut] optional Vulkan feature '" << logged.featureName
			          << "' disabled: " << logged.message << "\n";
		});
}

static bool EnumerateInstanceExtensions(std::vector<VkExtensionProperties>& extensions, VkResult& result)
{
	uint32_t count = 0;
	result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	if (result != VK_SUCCESS)
		return false;
	extensions.resize(count);
	result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
	if (result != VK_SUCCESS)
	{
		extensions.clear();
		return false;
	}
	extensions.resize(count);
	return true;
}

static bool EnumerateDeviceExtensions(VkPhysicalDevice physicalDevice, std::vector<VkExtensionProperties>& extensions, VkResult& result)
{
	uint32_t count = 0;
	result = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
	if (result != VK_SUCCESS)
		return false;
	extensions.resize(count);
	result = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data());
	if (result != VK_SUCCESS)
	{
		extensions.clear();
		return false;
	}
	extensions.resize(count);
	return true;
}

static VkResult CreateVulkanInstance(const std::vector<std::string>& names, bool, VkInstance& instance)
{
	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "RT2";
	app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	app_info.pEngineName = "RT2";
	app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	app_info.apiVersion = VK_API_VERSION_1_2;

	std::vector<const char*> pointers;
	pointers.reserve(names.size());
	for (const std::string& name : names)
		pointers.emplace_back(name.c_str());

	VkInstanceCreateInfo create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create_info.pApplicationInfo = &app_info;
	create_info.enabledExtensionCount = static_cast<uint32_t>(pointers.size());
	create_info.ppEnabledExtensionNames = pointers.data();
	const bool useValidation = g_EnableRuntimeValidation
#ifdef IMGUI_VULKAN_DEBUG_REPORT
		|| true
#endif
		;
	const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
	VkValidationFeaturesEXT validationFeatures = {};
	VkValidationFeatureEnableEXT enabledFeatures[1] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
	if (useValidation)
	{
		create_info.enabledLayerCount = 1;
		create_info.ppEnabledLayerNames = layers;
		if (g_EnableSyncValidation)
		{
			validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
			validationFeatures.enabledValidationFeatureCount = 1;
			validationFeatures.pEnabledValidationFeatures = enabledFeatures;
			create_info.pNext = &validationFeatures;
		}
	}
	return vkCreateInstance(&create_info, g_Allocator, &instance);
}

static VkPhysicalDevice SelectPhysicalDevice(VkInstance instance)
{
	VkResult err;
	uint32_t gpu_count = 0;
	err = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
	check_vk_result(err);
	IM_ASSERT(gpu_count > 0);
	std::vector<VkPhysicalDevice> gpus(gpu_count);
	err = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
	check_vk_result(err);
	int use_gpu = 0;
	for (int i = 0; i < static_cast<int>(gpu_count); ++i)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(gpus[i], &properties);
		if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			use_gpu = i;
			break;
		}
	}
	return gpus[use_gpu];
}

static VkResult CreateLogicalDevice(
	VkPhysicalDevice physicalDevice,
	const std::vector<std::string>& requestedNames,
	const std::vector<std::string>& optionalNames,
	bool optional,
	VkDevice& device)
{
	g_PhysicalDevice = physicalDevice;
	const char* baselineNames[] = {
		"VK_KHR_swapchain",
		"VK_KHR_acceleration_structure",
		"VK_KHR_ray_query",
		"VK_KHR_ray_tracing_pipeline",
		"VK_KHR_buffer_device_address",
		"VK_KHR_deferred_host_operations",
		"VK_KHR_storage_buffer_storage_class",
		"VK_KHR_spirv_1_4",
		"VK_KHR_shader_float_controls",
		"VK_KHR_shader_non_semantic_info",
		"VK_EXT_descriptor_indexing",
		"VK_KHR_synchronization2",
		"VK_KHR_create_renderpass2",
		"VK_KHR_dynamic_rendering",
		"VK_KHR_ray_tracing_maintenance1",
		"VK_EXT_memory_budget"
	};
	const std::vector<std::string> baselineExtensionNames(std::begin(baselineNames), std::end(baselineNames));

	uint32_t queue_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queue_count, nullptr);
	std::vector<VkQueueFamilyProperties> queues(queue_count);
	vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &queue_count, queues.data());
	for (uint32_t i = 0; i < queue_count; ++i)
		if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			g_QueueFamily = i;
			break;
		}
	IM_ASSERT(g_QueueFamily != (uint32_t)-1);

	VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address_features = {};
	buffer_device_address_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR accel_features = {};
	accel_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	accel_features.pNext = &buffer_device_address_features;
	VkPhysicalDeviceRayQueryFeaturesKHR ray_query_features = {};
	ray_query_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	ray_query_features.pNext = &accel_features;
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt_pipeline_features = {};
	rt_pipeline_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
	rt_pipeline_features.pNext = &ray_query_features;
	VkPhysicalDeviceDescriptorIndexingFeaturesEXT descriptor_indexing_features = {};
	descriptor_indexing_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT;
	descriptor_indexing_features.pNext = &rt_pipeline_features;
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering_features = {};
	dynamic_rendering_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
	dynamic_rendering_features.pNext = &descriptor_indexing_features;
	VkPhysicalDeviceVulkan11Features vulkan11_features = {};
	vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	vulkan11_features.pNext = &dynamic_rendering_features;
	VkPhysicalDeviceFeatures2 device_features2 = {};
	device_features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	device_features2.pNext = &vulkan11_features;
	vkGetPhysicalDeviceFeatures2(g_PhysicalDevice, &device_features2);
	const bool rt_supported = buffer_device_address_features.bufferDeviceAddress == VK_TRUE &&
		accel_features.accelerationStructure == VK_TRUE && ray_query_features.rayQuery == VK_TRUE;
	const bool rt_pipeline_supported = rt_supported && rt_pipeline_features.rayTracingPipeline == VK_TRUE;
	g_RayTracingSupported = rt_supported;
	g_RayTracingPipelineSupported = rt_pipeline_supported;

	g_RTPipelineProperties = {};
	g_RTPipelineProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
	VkPhysicalDeviceProperties2 physical_device_properties2 = {};
	physical_device_properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	physical_device_properties2.pNext = &g_RTPipelineProperties;
	vkGetPhysicalDeviceProperties2(g_PhysicalDevice, &physical_device_properties2);
	std::cerr << "[RT2] RT props: maxRayRecursionDepth=" << g_RTPipelineProperties.maxRayRecursionDepth
		          << " shaderGroupHandleSize=" << g_RTPipelineProperties.shaderGroupHandleSize
		          << " baseAlignment=" << g_RTPipelineProperties.shaderGroupBaseAlignment
		          << " handleAlignment=" << g_RTPipelineProperties.shaderGroupHandleAlignment << "\n";

	const std::vector<std::string> enabledNames = Walnut::NormalizeVulkanDeviceExtensions(
		requestedNames, baselineExtensionNames, optional && !optionalNames.empty() ? optionalNames : std::vector<std::string>{}, rt_supported);
	std::vector<const char*> extensionPointers;
	for (const std::string& name : enabledNames)
		extensionPointers.emplace_back(name.c_str());

	const float queue_priority[] = { 1.0f };
	VkDeviceQueueCreateInfo queue_info = {};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = g_QueueFamily;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = queue_priority;
	VkDeviceCreateInfo create_info = {};
	create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create_info.queueCreateInfoCount = 1;
	create_info.pQueueCreateInfos = &queue_info;
	create_info.enabledExtensionCount = static_cast<uint32_t>(extensionPointers.size());
	create_info.ppEnabledExtensionNames = extensionPointers.data();
	if (rt_supported)
	{
		buffer_device_address_features.bufferDeviceAddress = VK_TRUE;
		accel_features.accelerationStructure = VK_TRUE;
		ray_query_features.rayQuery = VK_TRUE;
		if (rt_pipeline_supported)
			rt_pipeline_features.rayTracingPipeline = VK_TRUE;
		descriptor_indexing_features.descriptorBindingPartiallyBound = VK_TRUE;
		descriptor_indexing_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
		descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		descriptor_indexing_features.runtimeDescriptorArray = VK_TRUE;
		dynamic_rendering_features.dynamicRendering = VK_TRUE;
		vulkan11_features.shaderDrawParameters = VK_TRUE;
		create_info.pNext = &device_features2;
		std::cerr << "[RT2] Vulkan Ray Tracing extensions enabled (pipeline="
		          << (rt_pipeline_supported ? "yes" : "no") << ").\n";
	}
	else
		std::cerr << "[RT2] WARNING: Ray Tracing not supported on this device. Falling back to CPU renderer.\n";

	VkResult err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &device);
	if (err == VK_SUCCESS)
	{
		vkGetDeviceQueue(device, g_QueueFamily, 0, &g_Queue);
		RTDispatchInit(device);
	}
	return err;
}

static void SetupVulkan(
	const Walnut::ApplicationSpecification& specification,
	std::vector<Walnut::OptionalVulkanFeatureDiagnostic>& diagnostics,
	std::vector<std::string>& instanceExtensionStorage,
	std::vector<std::string>& deviceExtensionStorage,
	bool& optionalFeatureEnabled,
	const char** extensions,
	uint32_t extensions_count)
{
	optionalFeatureEnabled = false;
	instanceExtensionStorage.clear();
	deviceExtensionStorage.clear();

	const bool useValidation = g_EnableRuntimeValidation
#ifdef IMGUI_VULKAN_DEBUG_REPORT
		|| true
#endif
		;
	std::vector<std::string> baselineInstanceExtensions;
	baselineInstanceExtensions.reserve(extensions_count + (useValidation ? 1 : 0));
	for (uint32_t i = 0; i < extensions_count; ++i)
		if (extensions && extensions[i])
			baselineInstanceExtensions.emplace_back(extensions[i]);
	if (useValidation)
		baselineInstanceExtensions.emplace_back("VK_EXT_debug_report");

	const std::vector<std::string> baselineDeviceExtensions = {
		"VK_KHR_swapchain",
		"VK_KHR_acceleration_structure",
		"VK_KHR_ray_query",
		"VK_KHR_ray_tracing_pipeline",
		"VK_KHR_buffer_device_address",
		"VK_KHR_deferred_host_operations",
		"VK_KHR_storage_buffer_storage_class",
		"VK_KHR_spirv_1_4",
		"VK_KHR_shader_float_controls",
		"VK_KHR_shader_non_semantic_info",
		"VK_EXT_descriptor_indexing",
		"VK_KHR_synchronization2",
		"VK_KHR_create_renderpass2",
		"VK_KHR_dynamic_rendering",
		"VK_KHR_ray_tracing_maintenance1",
		"VK_EXT_memory_budget"
	};

	Walnut::OptionalVulkanRequirementsHooks hooks;
	hooks.enumerateInstanceExtensions = [] {
		std::vector<VkExtensionProperties> available;
		VkResult result = VK_SUCCESS;
		if (!EnumerateInstanceExtensions(available, result))
			return Walnut::Result<std::vector<VkExtensionProperties>>::Failure(
				"vkEnumerateInstanceExtensionProperties failed", result);
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(std::move(available));
	};
	hooks.enumerateDeviceExtensions = [](VkInstance, VkPhysicalDevice physicalDevice) {
		std::vector<VkExtensionProperties> available;
		VkResult result = VK_SUCCESS;
		if (!EnumerateDeviceExtensions(physicalDevice, available, result))
			return Walnut::Result<std::vector<VkExtensionProperties>>::Failure(
				"vkEnumerateDeviceExtensionProperties failed", result);
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(std::move(available));
	};
	hooks.createInstance = [](const std::vector<std::string>& names, bool optional, VkInstance& instance) {
		const VkResult result = CreateVulkanInstance(names, optional, instance);
		g_Instance = result == VK_SUCCESS ? instance : VK_NULL_HANDLE;
		return result;
	};
	hooks.selectPhysicalDevice = [](VkInstance instance) {
		g_PhysicalDevice = SelectPhysicalDevice(instance);
		return g_PhysicalDevice;
	};
	hooks.createDevice = [](VkInstance, VkPhysicalDevice physicalDevice, const std::vector<std::string>& names,
		const std::vector<std::string>& optionalNames, bool optional, VkDevice& device) {
		const VkResult result = CreateLogicalDevice(physicalDevice, names, optionalNames, optional, device);
		g_Device = result == VK_SUCCESS ? device : VK_NULL_HANDLE;
		return result;
	};

	const Walnut::OptionalVulkanRequirementsExecution execution =
		Walnut::RunOptionalVulkanRequirements(
			specification.optionalVulkanFeatureProvider,
			baselineInstanceExtensions,
			baselineDeviceExtensions,
			hooks);
	for (const Walnut::OptionalVulkanFeatureDiagnostic& diagnostic : execution.diagnostics)
		PublishOptionalDiagnostic(specification, diagnostics, diagnostic);

	instanceExtensionStorage = execution.instanceExtensions;
	deviceExtensionStorage = execution.deviceExtensions;
	optionalFeatureEnabled = execution.optionalFeatureEnabled;

	if (execution.baselineInstanceCreateFailed)
		check_vk_result(execution.baselineInstanceCreateResult);
	if (execution.baselineDeviceCreateFailed)
		check_vk_result(execution.baselineDeviceCreateResult);

	if (useValidation)
	{
		// Get the function pointer (required for any extensions).
		auto vkCreateDebugReportCallbackEXT =
			(PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
		IM_ASSERT(vkCreateDebugReportCallbackEXT != NULL);

		VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
		debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
		debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
			VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
		debug_report_ci.pfnCallback = debug_report;
		debug_report_ci.pUserData = NULL;
		VkResult result = vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
		check_vk_result(result);
	}
	else
	{
		IM_UNUSED(g_DebugReport);
	}

	// Create Descriptor Pool
	{
		VkDescriptorPoolSize pool_sizes[] =
		{
			{ VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
			{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
			{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 },
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1000 }
		};
		VkDescriptorPoolCreateInfo pool_info = {};
		pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
		pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
		pool_info.pPoolSizes = pool_sizes;
		VkResult err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
		check_vk_result(err);
	}
}

// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height)
{
	wd->Surface = surface;

	// Check for WSI support
	VkBool32 res;
	vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
	if (res != VK_TRUE)
	{
		fprintf(stderr, "Error no WSI support on physical device 0\n");
		exit(-1);
	}

	// Select Surface Format
	const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
	const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
	wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

	// Select Present Mode
#ifdef IMGUI_UNLIMITED_FRAME_RATE
	VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
	VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
	wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
	//printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

	// Create SwapChain, RenderPass, Framebuffer, etc.
	IM_ASSERT(g_MinImageCount >= 2);
	ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void CleanupVulkan()
{
	vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#if defined(IMGUI_VULKAN_DEBUG_REPORT) || 1
	if (g_DebugReport)
	{
		// Remove the debug report callback
		auto vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
		vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
		g_DebugReport = VK_NULL_HANDLE;
	}
#endif

	vkDestroyDevice(g_Device, g_Allocator);
	vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow()
{
	ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data)
{
	VkResult err;

	VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
	VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
	err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
	{
		g_SwapChainRebuild = true;
		return;
	}
	check_vk_result(err);

	s_CurrentFrameIndex = (s_CurrentFrameIndex + 1) % g_MainWindowData.ImageCount;

	ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
	{
		err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
		check_vk_result(err);

		err = vkResetFences(g_Device, 1, &fd->Fence);
		check_vk_result(err);
	}
	
	{
		// Free resources in queue
		for (auto& func : s_ResourceFreeQueue[s_CurrentFrameIndex])
			func();
		s_ResourceFreeQueue[s_CurrentFrameIndex].clear();
	}
	{
		// Free command buffers allocated by Application::GetCommandBuffer
		// These use g_MainWindowData.FrameIndex and not s_CurrentFrameIndex because they're tied to the swapchain image index
		auto& allocatedCommandBuffers = s_AllocatedCommandBuffers[wd->FrameIndex];
		if (allocatedCommandBuffers.size() > 0)
		{
			vkFreeCommandBuffers(g_Device, fd->CommandPool, (uint32_t)allocatedCommandBuffers.size(), allocatedCommandBuffers.data());
			allocatedCommandBuffers.clear();
		}

		err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
		check_vk_result(err);
		VkCommandBufferBeginInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
		check_vk_result(err);
	}
	{
		VkRenderPassBeginInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		info.renderPass = wd->RenderPass;
		info.framebuffer = fd->Framebuffer;
		info.renderArea.extent.width = wd->Width;
		info.renderArea.extent.height = wd->Height;
		info.clearValueCount = 1;
		info.pClearValues = &wd->ClearValue;
		vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
	}

	// Record dear imgui primitives into command buffer
	ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

	// Submit command buffer
	vkCmdEndRenderPass(fd->CommandBuffer);
	{
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &image_acquired_semaphore;
		info.pWaitDstStageMask = &wait_stage;
		info.commandBufferCount = 1;
		info.pCommandBuffers = &fd->CommandBuffer;
		info.signalSemaphoreCount = 1;
		info.pSignalSemaphores = &render_complete_semaphore;

		err = vkEndCommandBuffer(fd->CommandBuffer);
		check_vk_result(err);
		err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
		check_vk_result(err);
	}
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd)
{
	if (g_SwapChainRebuild)
		return;
	VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
	VkPresentInfoKHR info = {};
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1;
	info.pWaitSemaphores = &render_complete_semaphore;
	info.swapchainCount = 1;
	info.pSwapchains = &wd->Swapchain;
	info.pImageIndices = &wd->FrameIndex;
	VkResult err = vkQueuePresentKHR(g_Queue, &info);
	if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
	{
		g_SwapChainRebuild = true;
		return;
	}
	check_vk_result(err);
	wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount; // Now we can use the next set of semaphores
}

static void glfw_error_callback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

namespace Walnut {

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		s_Instance = this;

		Init();
	}

	Application::~Application()
	{
		Shutdown();

		s_Instance = nullptr;
	}

	Application& Application::Get()
	{
		return *s_Instance;
	}

	void Application::Init()
	{
		g_EnableRuntimeValidation = m_Specification.EnableValidation;
		g_EnableSyncValidation = m_Specification.EnableSyncValidation;

		// Setup GLFW window
		glfwSetErrorCallback(glfw_error_callback);
		if (!glfwInit())
		{
			std::cerr << "Could not initalize GLFW!\n";
			return;
		}

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		m_WindowHandle = glfwCreateWindow(m_Specification.Width, m_Specification.Height, m_Specification.Name.c_str(), NULL, NULL);

		// Route OS close (title-bar X / Alt+F4) through RequestClose so the
		// host layer can run an unsaved-changes prompt and cancel the close.
		// We cancel the GLFW close here and let the app drive m_Running via
		// RequestClose(). Headless/internal completion still uses Close()
		// directly and is unaffected.
		glfwSetWindowCloseCallback(m_WindowHandle, [](GLFWwindow* w) {
			Walnut::Application& app = Walnut::Application::Get();
			// Always cancel the GLFW-level close; the app decides via
			// RequestClose whether to set m_Running=false. If no callback is
			// installed, RequestClose falls back to immediate Close().
			app.RequestClose();
			glfwSetWindowShouldClose(w, GLFW_FALSE);
		});

		// Setup Vulkan
		if (!glfwVulkanSupported())
		{
			std::cerr << "GLFW: Vulkan not supported!\n";
			return;
		}
		uint32_t extensions_count = 0;
		const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
		SetupVulkan(m_Specification, m_OptionalVulkanDiagnostics,
			m_InstanceExtensionStorage, m_DeviceExtensionStorage,
			m_OptionalVulkanFeatureEnabled, extensions, extensions_count);

		// Create Window Surface
		VkSurfaceKHR surface;
		VkResult err = glfwCreateWindowSurface(g_Instance, m_WindowHandle, g_Allocator, &surface);
		check_vk_result(err);

		// Create Framebuffers
		int w, h;
		glfwGetFramebufferSize(m_WindowHandle, &w, &h);
		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
		SetupVulkanWindow(wd, surface, w, h);

		s_AllocatedCommandBuffers.resize(wd->ImageCount);
		s_ResourceFreeQueue.resize(wd->ImageCount);

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;         // Enable Multi-Viewport / Platform Windows
		//io.ConfigViewportsNoAutoMerge = true;
		//io.ConfigViewportsNoTaskBarIcon = true;

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
		ImGuiStyle& style = ImGui::GetStyle();
		if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}

		// Setup Platform/Renderer backends
		ImGui_ImplGlfw_InitForVulkan(m_WindowHandle, true);
		ImGui_ImplVulkan_InitInfo init_info = {};
		init_info.Instance = g_Instance;
		init_info.PhysicalDevice = g_PhysicalDevice;
		init_info.Device = g_Device;
		init_info.QueueFamily = g_QueueFamily;
		init_info.Queue = g_Queue;
		init_info.PipelineCache = g_PipelineCache;
		init_info.DescriptorPool = g_DescriptorPool;
		init_info.Subpass = 0;
		init_info.MinImageCount = g_MinImageCount;
		init_info.ImageCount = wd->ImageCount;
		init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		init_info.Allocator = g_Allocator;
		init_info.CheckVkResultFn = check_vk_result;
		ImGui_ImplVulkan_Init(&init_info, wd->RenderPass);

		// Load default font
		ImFontConfig fontConfig;
		fontConfig.FontDataOwnedByAtlas = false;
		ImFont* robotoFont = io.Fonts->AddFontFromMemoryTTF((void*)g_RobotoRegular, sizeof(g_RobotoRegular), 16.0f, &fontConfig);
		io.FontDefault = robotoFont;

		// Tighten ImGui style for a more compact UI
		ImGuiStyle& uiStyle = ImGui::GetStyle();
		uiStyle.FramePadding = ImVec2(3.0f, 2.0f);
		uiStyle.ItemSpacing = ImVec2(6.0f, 4.0f);
		uiStyle.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
		uiStyle.WindowPadding = ImVec2(6.0f, 6.0f);
		uiStyle.IndentSpacing = 14.0f;
		uiStyle.GrabMinSize = 8.0f;
		uiStyle.ScrollbarSize = 12.0f;

		// Upload Fonts
		{
			// Use any command queue
			VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;
			VkCommandBuffer command_buffer = wd->Frames[wd->FrameIndex].CommandBuffer;

			err = vkResetCommandPool(g_Device, command_pool, 0);
			check_vk_result(err);
			VkCommandBufferBeginInfo begin_info = {};
			begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			err = vkBeginCommandBuffer(command_buffer, &begin_info);
			check_vk_result(err);

			ImGui_ImplVulkan_CreateFontsTexture(command_buffer);

			VkSubmitInfo end_info = {};
			end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			end_info.commandBufferCount = 1;
			end_info.pCommandBuffers = &command_buffer;
			err = vkEndCommandBuffer(command_buffer);
			check_vk_result(err);
			err = vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);
			check_vk_result(err);

			err = vkDeviceWaitIdle(g_Device);
			check_vk_result(err);
			ImGui_ImplVulkan_DestroyFontUploadObjects();
		}
	}

	void Application::Shutdown()
	{
		for (auto& layer : m_LayerStack)
			layer->OnDetach();

		m_LayerStack.clear();

		// Cleanup
		VkResult err = vkDeviceWaitIdle(g_Device);
		check_vk_result(err);

		// Free resources in queue
		for (auto& queue : s_ResourceFreeQueue)
		{
			for (auto& func : queue)
				func();
		}
		s_ResourceFreeQueue.clear();

		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();

		CleanupVulkanWindow();
		CleanupVulkan();

		glfwDestroyWindow(m_WindowHandle);
		glfwTerminate();

		g_ApplicationRunning = false;
	}

	void Application::Run()
	{
		m_Running = true;

		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
		ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
		ImGuiIO& io = ImGui::GetIO();

		// Initialize frame timing baseline before the loop starts so the
		// first frame's timestep is near-zero instead of the full app uptime.
		m_LastFrameTime = GetTime();

		// Main loop
		while (!glfwWindowShouldClose(m_WindowHandle) && m_Running)
		{
			// Poll and handle events (inputs, window resize, etc.)
			// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
			// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
			// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
			// Generally you may always pass all inputs to dear imgui, and hide them from the application based on those two flags.
			glfwPollEvents();

			// Compute timestep at the start of the frame (before OnUpdate) so
			// camera movement uses the actual elapsed time for this frame, not
			// the previous frame's measured time. This eliminates 1 frame of
			// input latency and keeps movement consistent under frame-time
			// variance. Clamp to 250ms to avoid huge jumps after stalls (e.g.
			// window drag, loading) without discarding normal frame variance.
			float timeNow = GetTime();
			m_FrameTime = timeNow - m_LastFrameTime;
			m_LastFrameTime = timeNow;
			m_TimeStep = glm::min<float>(m_FrameTime, 0.25f);

			for (auto& layer : m_LayerStack)
				layer->OnUpdate(m_TimeStep);

			// Resize swap chain?
			if (g_SwapChainRebuild)
			{
				int width, height;
				glfwGetFramebufferSize(m_WindowHandle, &width, &height);
				if (width > 0 && height > 0)
				{
					ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
					ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
					g_MainWindowData.FrameIndex = 0;

					// Clear allocated command buffers from here since entire pool is destroyed
					s_AllocatedCommandBuffers.clear();
					s_AllocatedCommandBuffers.resize(g_MainWindowData.ImageCount);

					g_SwapChainRebuild = false;
				}
			}

			// Start the Dear ImGui frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();

			{
				static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

				// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
				// because it would be confusing to have two docking targets within each others.
				ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
				if (m_MenubarCallback)
					window_flags |= ImGuiWindowFlags_MenuBar;

				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::SetNextWindowViewport(viewport->ID);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
				window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
				window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

				// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
				// and handle the pass-thru hole, so we ask Begin() to not render a background.
				if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
					window_flags |= ImGuiWindowFlags_NoBackground;

				// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
				// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
				// all active windows docked into it will lose their parent and become undocked.
				// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
				// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
				ImGui::Begin("DockSpace Demo", nullptr, window_flags);
				ImGui::PopStyleVar();

				ImGui::PopStyleVar(2);

				// Submit the DockSpace
				ImGuiIO& io = ImGui::GetIO();
				if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
				{
					ImGuiID dockspace_id = ImGui::GetID("VulkanAppDockspace");
					for (auto& layer : m_LayerStack)
						layer->OnDockspaceUI((uint32_t)dockspace_id);
					ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
				}

				if (m_MenubarCallback)
				{
					if (ImGui::BeginMenuBar())
					{
						m_MenubarCallback();
						ImGui::EndMenuBar();
					}
				}

				for (auto& layer : m_LayerStack)
					layer->OnUIRender();

				ImGui::End();
			}

			// Rendering
			ImGui::Render();
			ImDrawData* main_draw_data = ImGui::GetDrawData();
			const bool main_is_minimized = (main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f);
			wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
			wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
			wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
			wd->ClearValue.color.float32[3] = clear_color.w;
			if (!main_is_minimized)
				FrameRender(wd, main_draw_data);

			// Update and Render additional Platform Windows
			if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
			}

			// Present Main Platform Window
			if (!main_is_minimized)
				FramePresent(wd);
		}

	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::RequestClose()
	{
		// If a close-request callback is installed, give the host a chance to
		// cancel (e.g. unsaved-changes prompt). Returning false means "do not
		// close"; the host is responsible for re-requesting once the user
		// resolves the prompt. Returning true means proceed with close.
		if (m_CloseRequestCallback)
		{
			if (m_CloseRequestCallback())
				m_Running = false;
			// else: cancelled; m_Running stays true.
		}
		else
		{
			m_Running = false;
		}
	}

	float Application::GetTime()
	{
		return (float)glfwGetTime();
	}

	VkInstance Application::GetInstance()
	{
		return g_Instance;
	}

	VkPhysicalDevice Application::GetPhysicalDevice()
	{
		return g_PhysicalDevice;
	}

	VkDevice Application::GetDevice()
	{
		return g_Device;
	}

	VkQueue Application::GetQueue()
	{
		return g_Queue;
	}

	uint32_t Application::GetQueueFamily()
	{
		return g_QueueFamily;
	}

	bool Application::IsRayTracingSupported()
	{
		return g_RayTracingSupported;
	}

	bool Application::IsRayTracingPipelineSupported()
	{
		return g_RayTracingPipelineSupported;
	}

	const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& Application::GetRayTracingPipelineProperties()
	{
		return g_RTPipelineProperties;
	}

	VkDescriptorPool Application::GetDescriptorPool()
	{
		return g_DescriptorPool;
	}

	VkCommandBuffer Application::GetCommandBuffer(bool begin)
	{
		ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;

		// Use any command queue
		VkCommandPool command_pool = wd->Frames[wd->FrameIndex].CommandPool;

		VkCommandBufferAllocateInfo cmdBufAllocateInfo = {};
		cmdBufAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmdBufAllocateInfo.commandPool = command_pool;
		cmdBufAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdBufAllocateInfo.commandBufferCount = 1;

		VkCommandBuffer& command_buffer = s_AllocatedCommandBuffers[wd->FrameIndex].emplace_back();
		auto err = vkAllocateCommandBuffers(g_Device, &cmdBufAllocateInfo, &command_buffer);

		VkCommandBufferBeginInfo begin_info = {};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		err = vkBeginCommandBuffer(command_buffer, &begin_info);
		check_vk_result(err);

		return command_buffer;
	}

	void Application::FlushCommandBuffer(VkCommandBuffer commandBuffer)
	{
		const uint64_t DEFAULT_FENCE_TIMEOUT = 100000000000;

		VkSubmitInfo end_info = {};
		end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		end_info.commandBufferCount = 1;
		end_info.pCommandBuffers = &commandBuffer;
		auto err = vkEndCommandBuffer(commandBuffer);
		if (err) fprintf(stderr, "[vulkan] vkEndCommandBuffer = %d\n", err);
		check_vk_result(err);

		// Create fence to ensure that the command buffer has finished executing
		VkFenceCreateInfo fenceCreateInfo = {};
		fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceCreateInfo.flags = 0;
		VkFence fence;
		err = vkCreateFence(g_Device, &fenceCreateInfo, nullptr, &fence);
		if (err) fprintf(stderr, "[vulkan] vkCreateFence = %d\n", err);
		check_vk_result(err);

		err = vkQueueSubmit(g_Queue, 1, &end_info, fence);
		if (err) fprintf(stderr, "[vulkan] vkQueueSubmit = %d\n", err);
		check_vk_result(err);

		err = vkWaitForFences(g_Device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT);
		if (err) fprintf(stderr, "[vulkan] vkWaitForFences = %d\n", err);
		check_vk_result(err);

		vkDestroyFence(g_Device, fence, nullptr);
	}


	void Application::SubmitResourceFree(std::function<void()>&& func)
	{
		s_ResourceFreeQueue[s_CurrentFrameIndex].emplace_back(func);
	}

}
