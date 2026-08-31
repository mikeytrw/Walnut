#include "Walnut/Application.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

VkExtensionProperties Extension(const char* name)
{
	VkExtensionProperties extension = {};
	strncpy_s(extension.extensionName, VK_MAX_EXTENSION_NAME_SIZE, name, _TRUNCATE);
	return extension;
}

Walnut::Testing::OptionalVulkanRequirementsTestHooks BaseHooks(
	std::vector<VkExtensionProperties> instanceAvailable,
	std::vector<VkExtensionProperties> deviceAvailable)
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks;
	hooks.enumerateInstanceExtensions = [instanceAvailable = std::move(instanceAvailable)]() {
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(instanceAvailable);
	};
	hooks.enumerateDeviceExtensions = [deviceAvailable = std::move(deviceAvailable)](VkInstance, VkPhysicalDevice) {
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(deviceAvailable);
	};
	hooks.selectPhysicalDevice = [](VkInstance) {
		return reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(2));
	};
	hooks.createInstance = [](const std::vector<std::string>&, VkInstance& instance) {
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	hooks.createDevice = [](VkInstance, VkPhysicalDevice, const std::vector<std::string>&, VkDevice& device) {
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	return hooks;
}

void TestOrderingLifetimeAndDedupe()
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks = BaseHooks(
		{ Extension("VK_KHR_surface"), Extension("VK_EXT_optional_instance") },
		{ Extension("VK_KHR_swapchain"), Extension("VK_EXT_optional_device") });
	std::vector<std::string> instanceSeen;
	std::vector<std::string> deviceSeen;
	hooks.createInstance = [&instanceSeen](const std::vector<std::string>& names, VkInstance& instance) {
		instanceSeen = names;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	hooks.createDevice = [&deviceSeen](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, VkDevice& device) {
		deviceSeen = names;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};

	auto result = Walnut::Testing::RunOptionalVulkanRequirementsTestFlow(
		[] {
			// These values are local to the provider and must be copied by the flow.
			Walnut::OptionalVulkanFeatureRequirements requirements;
			requirements.featureName = "test-feature";
			requirements.instanceExtensions = { "VK_KHR_surface", "VK_EXT_optional_instance", "VK_EXT_optional_instance" };
			requirements.deviceExtensions = [](VkInstance, VkPhysicalDevice) {
				return Walnut::Result<std::vector<std::string>>::Success({ "VK_KHR_swapchain", "VK_EXT_optional_device", "VK_EXT_optional_device" });
			};
			return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
		},
		{ "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);

	Require(result.optionalFeatureEnabled, "optional feature should remain enabled");
	Require(instanceSeen == std::vector<std::string>({ "VK_KHR_surface", "VK_EXT_optional_instance" }), "instance names were not copied/deduped");
	Require(deviceSeen == std::vector<std::string>({ "VK_KHR_swapchain", "VK_EXT_optional_device" }), "device names were not copied/deduped");
	Require(result.events.size() >= 7 && result.events[0] == "provider" && result.events[1] == "enumerate-instance" &&
		result.events[2] == "create-instance-optional" && result.events[3] == "select-physical-device" &&
		result.events[4] == "provider-device" && result.events[5] == "enumerate-device" &&
		result.events[6] == "create-device-optional", "provider/physical-device ordering changed");
}

void TestMissingExtensionFallback()
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks = BaseHooks({}, {});
	auto result = Walnut::Testing::RunOptionalVulkanRequirementsTestFlow(
		[] {
			Walnut::OptionalVulkanFeatureRequirements requirements;
			requirements.featureName = "missing-feature";
			requirements.instanceExtensions = { "VK_EXT_missing_instance" };
			requirements.deviceExtensions = [](VkInstance, VkPhysicalDevice) {
				return Walnut::Result<std::vector<std::string>>::Success({ "VK_EXT_missing_device" });
			};
			return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
		},
		{ "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);
	Require(!result.optionalFeatureEnabled, "missing instance extension must disable feature");
	Require(result.instanceExtensions == std::vector<std::string>({ "VK_KHR_surface" }), "missing instance fallback changed baseline");
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::MissingExtension,
		"missing extension diagnostic absent");
}

void TestInstanceCreateRetry()
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks = BaseHooks(
		{ Extension("VK_EXT_optional_instance") }, {});
	int createCount = 0;
	hooks.createInstance = [&createCount](const std::vector<std::string>& names, VkInstance& instance) {
		++createCount;
		if (names.size() == 2)
			return VK_ERROR_INITIALIZATION_FAILED;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	auto result = Walnut::Testing::RunOptionalVulkanRequirementsTestFlow(
		[] {
			Walnut::OptionalVulkanFeatureRequirements requirements;
			requirements.instanceExtensions = { "VK_EXT_optional_instance" };
			return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
		},
		{ "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);
	Require(createCount == 2 && !result.baselineInstanceCreateFailed && !result.optionalFeatureEnabled,
		"instance optional-create retry contract failed");
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::InstanceCreateFailure &&
		result.diagnostics[0].vkResult == VK_ERROR_INITIALIZATION_FAILED,
		"instance create failure diagnostic missing exact VkResult");
}

void TestDeviceCreateRetry()
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks = BaseHooks(
		{}, { Extension("VK_EXT_optional_device") });
	int createCount = 0;
	hooks.createDevice = [&createCount](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, VkDevice& device) {
		++createCount;
		if (names.size() == 2)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	auto result = Walnut::Testing::RunOptionalVulkanRequirementsTestFlow(
		[] {
			Walnut::OptionalVulkanFeatureRequirements requirements;
			requirements.deviceExtensions = [](VkInstance, VkPhysicalDevice) {
				return Walnut::Result<std::vector<std::string>>::Success({ "VK_EXT_optional_device" });
			};
			return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
		},
		{ "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);
	Require(createCount == 2 && !result.baselineDeviceCreateFailed && !result.optionalFeatureEnabled,
		"device optional-create retry contract failed");
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::DeviceCreateFailure &&
		result.diagnostics[0].vkResult == VK_ERROR_FEATURE_NOT_PRESENT,
		"device create failure diagnostic missing exact VkResult");
}

void TestNoProviderUnchanged()
{
	Walnut::Testing::OptionalVulkanRequirementsTestHooks hooks = BaseHooks({}, {});
	int instanceCount = 0;
	int deviceCount = 0;
	hooks.createInstance = [&instanceCount](const std::vector<std::string>& names, VkInstance& instance) {
		++instanceCount;
		if (names != std::vector<std::string>({ "VK_KHR_surface" }))
			return VK_ERROR_INITIALIZATION_FAILED;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	hooks.createDevice = [&deviceCount](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, VkDevice& device) {
		++deviceCount;
		if (names != std::vector<std::string>({ "VK_KHR_swapchain" }))
			return VK_ERROR_INITIALIZATION_FAILED;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	auto result = Walnut::Testing::RunOptionalVulkanRequirementsTestFlow({}, { "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);
	Require(instanceCount == 1 && deviceCount == 1 && result.diagnostics.empty() && !result.optionalFeatureEnabled,
		"no-provider startup behavior changed");
}

}

int main()
{
	try
	{
		TestOrderingLifetimeAndDedupe();
		TestMissingExtensionFallback();
		TestInstanceCreateRetry();
		TestDeviceCreateRetry();
		TestNoProviderUnchanged();
		std::cout << "Optional Vulkan requirements tests passed\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Optional Vulkan requirements test failure: " << exception.what() << "\n";
		return 1;
	}
}
