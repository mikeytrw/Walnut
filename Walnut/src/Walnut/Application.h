#pragma once

#include "Layer.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <utility>
#include <optional>
#include <exception>

#include "imgui.h"
#include "vulkan/vulkan.h"

void check_vk_result(VkResult err);

struct GLFWwindow;

namespace Walnut {

	// A small C++17 result type keeps optional feature setup independent from
	// any RT2 or SDK error type.  A failed result is deliberately explicit;
	// callers must choose the optional-feature fallback path.
	template<typename T>
	struct Result
	{
		bool succeeded = false;
		T value{};
		std::string error;
		VkResult vkResult = VK_SUCCESS;

		static Result Success(T result)
		{
			Result r;
			r.succeeded = true;
			r.value = std::move(result);
			return r;
		}

		static Result Failure(std::string message, VkResult result = VK_SUCCESS)
		{
			Result r;
			r.error = std::move(message);
			r.vkResult = result;
			return r;
		}

		explicit operator bool() const { return succeeded; }
	};

	struct OptionalVulkanFeatureRequirements
	{
		std::vector<std::string> instanceExtensions;
		std::function<Result<std::vector<std::string>>(VkInstance, VkPhysicalDevice)> deviceExtensions;
		std::string featureName;
	};

	enum class OptionalVulkanFeatureRequirementPhase
	{
		Provider,
		InstanceExtensions,
		DeviceExtensions,
	};

	enum class OptionalVulkanFeatureDisableReason
	{
		ProviderFailure,
		ExtensionEnumerationFailure,
		MissingExtension,
		InstanceCreateFailure,
		DeviceCreateFailure,
	};

	struct OptionalVulkanFeatureDiagnostic
	{
		std::string featureName;
		OptionalVulkanFeatureRequirementPhase phase = OptionalVulkanFeatureRequirementPhase::Provider;
		OptionalVulkanFeatureDisableReason reason = OptionalVulkanFeatureDisableReason::ProviderFailure;
		std::vector<std::string> unavailableExtensions;
		VkResult vkResult = VK_SUCCESS;
		std::string message;
	};

	using OptionalVulkanFeatureDiagnosticSink = std::function<void(const OptionalVulkanFeatureDiagnostic&)>;

	// Pure selection helper used by the bootstrap and by CPU-only seam tests.
	// It preserves request order, removes duplicates, and reports names that
	// are not in Vulkan's enumerated extension set.
	struct OptionalVulkanExtensionSelection
	{
		std::vector<std::string> enabled;
		std::vector<std::string> missing;
	};

	inline OptionalVulkanExtensionSelection DeduplicateAndValidateVulkanExtensions(
		const std::vector<std::string>& requested,
		const std::vector<VkExtensionProperties>& available)
	{
		std::unordered_set<std::string> availableNames;
		availableNames.reserve(available.size());
		for (const VkExtensionProperties& extension : available)
			availableNames.emplace(extension.extensionName);

		OptionalVulkanExtensionSelection selection;
		std::unordered_set<std::string> seen;
		seen.reserve(requested.size());
		for (const std::string& name : requested)
		{
			if (!seen.emplace(name).second)
				continue;
			if (name.empty() || availableNames.find(name) == availableNames.end())
				selection.missing.emplace_back(name);
			else
				selection.enabled.emplace_back(name);
		}
		return selection;
	}

	// Hook-parameterized bootstrap authority. Production supplies real Vulkan
	// operations; focused tests supply fakes without a loader or physical GPU.
	struct OptionalVulkanRequirementsHooks
	{
			std::function<Result<std::vector<VkExtensionProperties>>()> enumerateInstanceExtensions;
			std::function<Result<std::vector<VkExtensionProperties>>(VkInstance, VkPhysicalDevice)> enumerateDeviceExtensions;
			std::function<VkResult(const std::vector<std::string>&, bool, VkInstance&)> createInstance;
			std::function<VkResult(VkInstance, VkPhysicalDevice, const std::vector<std::string>&, bool, VkDevice&)> createDevice;
			std::function<VkPhysicalDevice(VkInstance)> selectPhysicalDevice;
		};

	struct OptionalVulkanRequirementsExecution
		{
			bool optionalFeatureEnabled = false;
			bool baselineInstanceCreateFailed = false;
			bool baselineDeviceCreateFailed = false;
			VkResult baselineInstanceCreateResult = VK_SUCCESS;
			VkResult baselineDeviceCreateResult = VK_SUCCESS;
			std::vector<std::string> instanceExtensions;
			std::vector<std::string> deviceExtensions;
			std::vector<std::string> events;
			std::vector<OptionalVulkanFeatureDiagnostic> diagnostics;
		};

	inline OptionalVulkanRequirementsExecution RunOptionalVulkanRequirements(
			const std::function<Result<OptionalVulkanFeatureRequirements>()>& provider,
			const std::vector<std::string>& baselineInstanceExtensions,
			const std::vector<std::string>& baselineDeviceExtensions,
			const OptionalVulkanRequirementsHooks& hooks)
		{
			OptionalVulkanRequirementsExecution result;
			auto deduplicate = [](std::vector<std::string>& names) {
				std::vector<std::string> unique;
				unique.reserve(names.size());
				std::unordered_set<std::string> seen;
				for (const std::string& name : names)
					if (seen.emplace(name).second)
						unique.emplace_back(name);
				names = std::move(unique);
			};
			std::vector<std::string> normalizedBaselineInstanceExtensions = baselineInstanceExtensions;
			std::vector<std::string> normalizedBaselineDeviceExtensions = baselineDeviceExtensions;
			deduplicate(normalizedBaselineInstanceExtensions);
			deduplicate(normalizedBaselineDeviceExtensions);
			result.instanceExtensions = normalizedBaselineInstanceExtensions;
			result.deviceExtensions = normalizedBaselineDeviceExtensions;
			deduplicate(result.instanceExtensions);
			deduplicate(result.deviceExtensions);
			std::optional<OptionalVulkanFeatureRequirements> requirements;
			std::string featureName = "optional-vulkan-feature";
			result.events.emplace_back("provider");
			if (provider)
			{
				Result<OptionalVulkanFeatureRequirements> provided;
				try
				{
					provided = provider();
				}
				catch (const std::exception& exception)
				{
					provided = Result<OptionalVulkanFeatureRequirements>::Failure(
						std::string("provider threw an exception: ") + exception.what());
				}
				catch (...)
				{
					provided = Result<OptionalVulkanFeatureRequirements>::Failure("provider threw an unknown exception");
				}
				if (provided)
				{
					requirements = std::move(provided.value);
					if (!requirements->featureName.empty())
						featureName = requirements->featureName;
				}
				else
				{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::Provider,
						OptionalVulkanFeatureDisableReason::ProviderFailure, {}, provided.vkResult,
						provided.error.empty() ? "provider returned failure" : provided.error});
				}
			}

			VkInstance instance = VK_NULL_HANDLE;
			if (requirements && hooks.enumerateInstanceExtensions)
			{
				result.events.emplace_back("enumerate-instance");
				Result<std::vector<VkExtensionProperties>> available = hooks.enumerateInstanceExtensions();
				if (!available)
				{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::InstanceExtensions,
						OptionalVulkanFeatureDisableReason::ExtensionEnumerationFailure, {}, available.vkResult,
						available.error.empty() ? "instance extension enumeration failed" : available.error});
					requirements.reset();
				}
				else
				{
					const OptionalVulkanExtensionSelection selection =
						DeduplicateAndValidateVulkanExtensions(requirements->instanceExtensions, available.value);
					if (!selection.missing.empty())
					{
						result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::InstanceExtensions,
						OptionalVulkanFeatureDisableReason::MissingExtension, selection.missing, VK_SUCCESS,
							"one or more optional instance extensions are unavailable"});
						requirements.reset();
					}
					else
					{
						result.instanceExtensions.insert(result.instanceExtensions.end(), selection.enabled.begin(), selection.enabled.end());
						deduplicate(result.instanceExtensions);
					}
				}
			}

			if (hooks.createInstance)
			{
				result.events.emplace_back(requirements ? "create-instance-optional" : "create-instance-baseline");
				VkResult createResult = hooks.createInstance(result.instanceExtensions, requirements.has_value(), instance);
				if (createResult != VK_SUCCESS && requirements)
				{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::InstanceExtensions,
						OptionalVulkanFeatureDisableReason::InstanceCreateFailure, {}, createResult,
						"vkCreateInstance failed with optional requirements; baseline retry performed"});
					requirements.reset();
					result.instanceExtensions = normalizedBaselineInstanceExtensions;
					result.events.emplace_back("create-instance-baseline-retry");
					createResult = hooks.createInstance(result.instanceExtensions, false, instance);
				}
				if (createResult != VK_SUCCESS)
				{
					result.baselineInstanceCreateFailed = true;
					result.baselineInstanceCreateResult = createResult;
				}
			}

			VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
			if (!result.baselineInstanceCreateFailed && hooks.selectPhysicalDevice)
			{
				result.events.emplace_back("select-physical-device");
				physicalDevice = hooks.selectPhysicalDevice(instance);
			}

			if (!result.baselineInstanceCreateFailed && requirements && hooks.enumerateDeviceExtensions)
			{
				result.events.emplace_back("provider-device");
				Result<std::vector<std::string>> provided;
				try
				{
					provided = requirements->deviceExtensions
						? requirements->deviceExtensions(instance, physicalDevice)
						: Result<std::vector<std::string>>::Success({});
				}
				catch (const std::exception& exception)
				{
					provided = Result<std::vector<std::string>>::Failure(
						std::string("device extension provider threw an exception: ") + exception.what());
				}
				catch (...)
				{
					provided = Result<std::vector<std::string>>::Failure("device extension provider threw an unknown exception");
				}
				if (!provided)
				{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::DeviceExtensions,
						OptionalVulkanFeatureDisableReason::ProviderFailure, {}, provided.vkResult,
						provided.error.empty() ? "device extension provider returned failure" : provided.error});
					requirements.reset();
				}
				else
				{
					result.events.emplace_back("enumerate-device");
					Result<std::vector<VkExtensionProperties>> available = hooks.enumerateDeviceExtensions(instance, physicalDevice);
					if (!available)
					{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::DeviceExtensions,
						OptionalVulkanFeatureDisableReason::ExtensionEnumerationFailure, {}, available.vkResult,
							available.error.empty() ? "device extension enumeration failed" : available.error});
						requirements.reset();
					}
					else
					{
						const OptionalVulkanExtensionSelection selection =
							DeduplicateAndValidateVulkanExtensions(provided.value, available.value);
						if (!selection.missing.empty())
						{
							result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::DeviceExtensions,
								OptionalVulkanFeatureDisableReason::MissingExtension, selection.missing, VK_SUCCESS,
								"one or more optional device extensions are unavailable"});
							requirements.reset();
						}
						else
						{
							result.deviceExtensions.insert(result.deviceExtensions.end(), selection.enabled.begin(), selection.enabled.end());
							deduplicate(result.deviceExtensions);
							result.optionalFeatureEnabled = true;
						}
					}
				}
			}

			if (!result.baselineInstanceCreateFailed && hooks.createDevice)
			{
				result.events.emplace_back(result.optionalFeatureEnabled ? "create-device-optional" : "create-device-baseline");
				VkDevice device = VK_NULL_HANDLE;
				VkResult createResult = hooks.createDevice(instance, physicalDevice, result.deviceExtensions, result.optionalFeatureEnabled, device);
				if (createResult != VK_SUCCESS && result.optionalFeatureEnabled)
				{
					result.diagnostics.push_back({featureName, OptionalVulkanFeatureRequirementPhase::DeviceExtensions,
						OptionalVulkanFeatureDisableReason::DeviceCreateFailure, {}, createResult,
						"vkCreateDevice failed with optional requirements; baseline retry performed"});
					result.optionalFeatureEnabled = false;
					result.deviceExtensions = normalizedBaselineDeviceExtensions;
					result.events.emplace_back("create-device-baseline-retry");
					createResult = hooks.createDevice(instance, physicalDevice, result.deviceExtensions, false, device);
				}
				if (createResult != VK_SUCCESS)
				{
					result.baselineDeviceCreateFailed = true;
					result.baselineDeviceCreateResult = createResult;
				}
			}
			return result;
		}

	struct ApplicationSpecification
	{
		std::string Name = "Walnut App";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool EnableValidation = false;
		bool EnableSyncValidation = false;
		// Optional requirements are policy-free: Walnut only probes, validates,
		// and enables what the host supplies.  A failed provider disables its
		// feature while baseline Vulkan startup continues.
		std::function<Result<OptionalVulkanFeatureRequirements>()> optionalVulkanFeatureProvider;
		OptionalVulkanFeatureDiagnosticSink optionalVulkanFeatureDiagnosticSink;
	};

	class Application
	{
	public:
		Application(const ApplicationSpecification& applicationSpecification = ApplicationSpecification());
		~Application();

		static Application& Get();

		void Run();
		void SetMenubarCallback(const std::function<void()>& menubarCallback) { m_MenubarCallback = menubarCallback; }
		
		template<typename T>
		void PushLayer()
		{
			static_assert(std::is_base_of<Layer, T>::value, "Pushed type is not subclass of Layer!");
			m_LayerStack.emplace_back(std::make_shared<T>())->OnAttach();
		}

		void PushLayer(const std::shared_ptr<Layer>& layer) { m_LayerStack.emplace_back(layer); layer->OnAttach(); }

		// Immediate, non-cancelable close. Used by headless completion and
		// after the user has explicitly confirmed. Sets m_Running = false so
		// the main loop exits at the next iteration.
		void Close();

		// Interactive, cancelable close request. Fires the close-request
		// callback so the host layer can run an unsaved-changes prompt and
		// cancel the close (e.g. user clicks Cancel). If no callback is
		// set, this falls back to an immediate Close().
		void RequestClose();
		void SetCloseRequestCallback(const std::function<bool()>& cb) { m_CloseRequestCallback = cb; }

		float GetTime();
		GLFWwindow* GetWindowHandle() const { return m_WindowHandle; }

		static VkInstance GetInstance();
		static VkPhysicalDevice GetPhysicalDevice();
		static VkDevice GetDevice();
		static VkQueue GetQueue();
		static uint32_t GetQueueFamily();
		static bool IsRayTracingSupported();
		static bool IsRayTracingPipelineSupported();
		static const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& GetRayTracingPipelineProperties();
		static VkDescriptorPool GetDescriptorPool();
		const std::vector<OptionalVulkanFeatureDiagnostic>& GetOptionalVulkanDiagnostics() const { return m_OptionalVulkanDiagnostics; }
		bool IsOptionalVulkanFeatureEnabled() const { return m_OptionalVulkanFeatureEnabled; }

		static VkCommandBuffer GetCommandBuffer(bool begin);
		static void FlushCommandBuffer(VkCommandBuffer commandBuffer);

		static void SubmitResourceFree(std::function<void()>&& func);
	private:
		void Init();
		void Shutdown();
	private:
		ApplicationSpecification m_Specification;
		GLFWwindow* m_WindowHandle = nullptr;
		bool m_Running = false;

		float m_TimeStep = 0.0f;
		float m_FrameTime = 0.0f;
		float m_LastFrameTime = 0.0f;

		std::vector<std::shared_ptr<Layer>> m_LayerStack;
		std::function<void()> m_MenubarCallback;
		std::function<bool()> m_CloseRequestCallback; // returns false to cancel
		std::vector<OptionalVulkanFeatureDiagnostic> m_OptionalVulkanDiagnostics;
		bool m_OptionalVulkanFeatureEnabled = false;
		std::vector<std::string> m_InstanceExtensionStorage;
		std::vector<std::string> m_DeviceExtensionStorage;
	};

	// Implemented by CLIENT
	Application* CreateApplication(int argc, char** argv);
}
