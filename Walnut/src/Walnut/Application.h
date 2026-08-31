#pragma once

#include "Layer.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <utility>

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

		static Result Success(T result)
		{
			Result r;
			r.succeeded = true;
			r.value = std::move(result);
			return r;
		}

		static Result Failure(std::string message)
		{
			Result r;
			r.error = std::move(message);
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
