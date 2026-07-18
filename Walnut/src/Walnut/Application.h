#pragma once

#include "Layer.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "imgui.h"
#include "vulkan/vulkan.h"

void check_vk_result(VkResult err);

struct GLFWwindow;

namespace Walnut {

	struct ApplicationSpecification
	{
		std::string Name = "Walnut App";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool EnableValidation = false;
		bool EnableSyncValidation = false;
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
	};

	// Implemented by CLIENT
	Application* CreateApplication(int argc, char** argv);
}