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

Walnut::OptionalVulkanRequirementsHooks BaseHooks(
	std::vector<VkExtensionProperties> instanceAvailable,
	std::vector<VkExtensionProperties> deviceAvailable)
{
	Walnut::OptionalVulkanRequirementsHooks hooks;
	hooks.enumerateInstanceExtensions = [instanceAvailable = std::move(instanceAvailable)]() {
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(instanceAvailable);
	};
	hooks.enumerateDeviceExtensions = [deviceAvailable = std::move(deviceAvailable)](VkInstance, VkPhysicalDevice) {
		return Walnut::Result<std::vector<VkExtensionProperties>>::Success(deviceAvailable);
	};
	hooks.selectPhysicalDevice = [](VkInstance) {
		return reinterpret_cast<VkPhysicalDevice>(static_cast<uintptr_t>(2));
	};
	hooks.createInstance = [](const std::vector<std::string>&, bool, VkInstance& instance) {
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	hooks.createDevice = [](VkInstance, VkPhysicalDevice, const std::vector<std::string>&, const std::vector<std::string>&, bool, VkDevice& device) {
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	return hooks;
}

void TestOrderingLifetimeAndDedupe()
{
	Walnut::OptionalVulkanRequirementsHooks hooks = BaseHooks(
		{ Extension("VK_KHR_surface"), Extension("VK_EXT_optional_instance") },
		{ Extension("VK_KHR_swapchain"), Extension("VK_EXT_optional_device") });
	std::vector<std::string> instanceSeen;
	std::vector<std::string> deviceSeen;
	bool instanceOptional = false;
	bool deviceOptional = false;
	hooks.createInstance = [&instanceSeen, &instanceOptional](const std::vector<std::string>& names, bool optional, VkInstance& instance) {
		instanceSeen = names;
		instanceOptional = optional;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	std::vector<std::string> optionalDeviceSeen;
	hooks.createDevice = [&deviceSeen, &optionalDeviceSeen, &deviceOptional](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, const std::vector<std::string>& optionalNames, bool optional, VkDevice& device) {
		deviceSeen = names;
		optionalDeviceSeen = optionalNames;
		deviceOptional = optional;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};

	auto result = Walnut::RunOptionalVulkanRequirements(
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
	Require(instanceOptional && deviceOptional, "production hooks did not receive enabled optional phase");
	Require(instanceSeen == std::vector<std::string>({ "VK_KHR_surface", "VK_EXT_optional_instance" }), "instance names were not copied/deduped");
	Require(deviceSeen == std::vector<std::string>({ "VK_KHR_swapchain", "VK_EXT_optional_device" }), "device names were not copied/deduped");
	Require(optionalDeviceSeen == std::vector<std::string>({ "VK_KHR_swapchain", "VK_EXT_optional_device" }), "provider device provenance was not retained");
	Require(result.events.size() >= 7 && result.events[0] == "provider" && result.events[1] == "enumerate-instance" &&
		result.events[2] == "create-instance-optional" && result.events[3] == "select-physical-device" &&
		result.events[4] == "provider-device" && result.events[5] == "enumerate-device" &&
		result.events[6] == "create-device-optional", "provider/physical-device ordering changed");
}

void TestMissingExtensionFallback()
{
	Walnut::OptionalVulkanRequirementsHooks hooks = BaseHooks({}, {});
	auto result = Walnut::RunOptionalVulkanRequirements(
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
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::MissingExtension &&
		result.diagnostics[0].phase == Walnut::OptionalVulkanFeatureRequirementPhase::InstanceExtensions &&
		result.diagnostics[0].unavailableExtensions == std::vector<std::string>({ "VK_EXT_missing_instance" }),
		"missing extension diagnostic absent");
}

void TestInstanceCreateRetry()
{
	Walnut::OptionalVulkanRequirementsHooks hooks = BaseHooks(
		{ Extension("VK_EXT_optional_instance") }, {});
	int createCount = 0;
	std::vector<bool> instanceOptional;
	hooks.createInstance = [&createCount, &instanceOptional](const std::vector<std::string>& names, bool optional, VkInstance& instance) {
		++createCount;
		instanceOptional.push_back(optional);
		if (names.size() == 2)
			return VK_ERROR_INITIALIZATION_FAILED;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	auto result = Walnut::RunOptionalVulkanRequirements(
		[] {
			Walnut::OptionalVulkanFeatureRequirements requirements;
			requirements.instanceExtensions = { "VK_EXT_optional_instance" };
			return Walnut::Result<Walnut::OptionalVulkanFeatureRequirements>::Success(std::move(requirements));
		},
		{ "VK_KHR_surface" }, { "VK_KHR_swapchain" }, hooks);
	Require(createCount == 2 && !result.baselineInstanceCreateFailed && !result.optionalFeatureEnabled,
		"instance optional-create retry contract failed");
	Require(instanceOptional == std::vector<bool>({ true, false }), "instance retry did not disable optional hook phase");
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::InstanceCreateFailure &&
		result.diagnostics[0].vkResult == VK_ERROR_INITIALIZATION_FAILED,
		"instance create failure diagnostic missing exact VkResult");
}

void TestDeviceCreateRetry()
{
	Walnut::OptionalVulkanRequirementsHooks hooks = BaseHooks(
		{}, { Extension("VK_EXT_optional_device") });
	int createCount = 0;
	std::vector<bool> deviceOptional;
	hooks.createDevice = [&createCount, &deviceOptional](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, const std::vector<std::string>&, bool optional, VkDevice& device) {
		++createCount;
		deviceOptional.push_back(optional);
		if (names.size() == 2)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	auto result = Walnut::RunOptionalVulkanRequirements(
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
	Require(deviceOptional == std::vector<bool>({ true, false }), "device retry did not disable optional hook phase");
	Require(result.diagnostics.size() == 1 && result.diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::DeviceCreateFailure &&
		result.diagnostics[0].vkResult == VK_ERROR_FEATURE_NOT_PRESENT,
		"device create failure diagnostic missing exact VkResult");
}

void TestNoProviderUnchanged()
{
	Walnut::OptionalVulkanRequirementsHooks hooks = BaseHooks({}, {});
	int instanceCount = 0;
	int deviceCount = 0;
	hooks.createInstance = [&instanceCount](const std::vector<std::string>& names, bool, VkInstance& instance) {
		++instanceCount;
		if (names != std::vector<std::string>({ "VK_KHR_surface" }))
			return VK_ERROR_INITIALIZATION_FAILED;
		instance = reinterpret_cast<VkInstance>(static_cast<uintptr_t>(1));
		return VK_SUCCESS;
	};
	hooks.createDevice = [&deviceCount](VkInstance, VkPhysicalDevice, const std::vector<std::string>& names, const std::vector<std::string>&, bool, VkDevice& device) {
		++deviceCount;
		if (names != std::vector<std::string>({ "VK_KHR_swapchain" }))
			return VK_ERROR_INITIALIZATION_FAILED;
		device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(3));
		return VK_SUCCESS;
	};
	auto result = Walnut::RunOptionalVulkanRequirements({}, { "VK_KHR_surface", "VK_KHR_surface" }, { "VK_KHR_swapchain", "VK_KHR_swapchain" }, hooks);
	Require(instanceCount == 1 && deviceCount == 1 && result.diagnostics.empty() && !result.optionalFeatureEnabled,
		"no-provider startup behavior changed");
}

void TestRealHookOverlapNormalization()
{
	const std::vector<std::string> baseline = { "VK_KHR_swapchain", "VK_KHR_buffer_device_address", "VK_EXT_rt" };
	const std::vector<std::string> requested = { "VK_KHR_swapchain", "VK_KHR_buffer_device_address", "VK_EXT_rt" };
	const std::vector<std::string> providerOwned = { "VK_KHR_buffer_device_address" };
	Require(Walnut::NormalizeVulkanDeviceExtensions(requested, baseline, providerOwned, false) ==
		std::vector<std::string>({ "VK_KHR_swapchain", "VK_KHR_buffer_device_address" }),
		"real-hook normalization dropped an overlapping provider extension");
	Require(Walnut::NormalizeVulkanDeviceExtensions(requested, baseline, {}, false) ==
		std::vector<std::string>({ "VK_KHR_swapchain" }),
		"baseline RT fallback no longer filters unsupported extensions");
}

void TestThrowingDiagnosticSinkIsContained()
{
	std::vector<Walnut::OptionalVulkanFeatureDiagnostic> diagnostics;
	std::vector<std::string> logged;
	Walnut::PublishOptionalVulkanDiagnostic(
		diagnostics,
		{ "sink-feature", Walnut::OptionalVulkanFeatureRequirementPhase::InstanceExtensions,
			Walnut::OptionalVulkanFeatureDisableReason::MissingExtension, { "VK_EXT_missing" }, VK_SUCCESS,
			"optional extension unavailable" },
		[](const Walnut::OptionalVulkanFeatureDiagnostic&) { throw std::runtime_error("sink fault"); },
		[&logged](const Walnut::OptionalVulkanFeatureDiagnostic& diagnostic) { logged.push_back(diagnostic.message); });
	Require(diagnostics.size() == 2 && diagnostics[0].reason == Walnut::OptionalVulkanFeatureDisableReason::MissingExtension &&
		diagnostics[1].reason == Walnut::OptionalVulkanFeatureDisableReason::DiagnosticSinkFailure &&
		diagnostics[1].message == "optional Vulkan diagnostic sink threw an exception: sink fault",
		"throwing diagnostic sink was not contained and retained");
	Require(logged.size() == 2 && logged[0] == "optional extension unavailable" && logged[1] == diagnostics[1].message,
		"throwing diagnostic sink failure was not deterministically logged");
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
		TestRealHookOverlapNormalization();
		TestThrowingDiagnosticSinkIsContained();
		std::cout << "Optional Vulkan requirements tests passed\n";
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Optional Vulkan requirements test failure: " << exception.what() << "\n";
		return 1;
	}
}
