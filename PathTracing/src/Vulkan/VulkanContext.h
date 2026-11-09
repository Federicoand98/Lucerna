#pragma once

#ifndef VULKANCONTEXT_H
#define VULKANCONTEXT_H

#include "ptpch.h"
#include <vulkan/vulkan.h>
#include <functional>
#include "Vulkan/GpuProfiler.h"

struct GLFWwindow;

namespace Lucerna {

	// A cosa serve un'allocazione GPU: la memoria si conta per categoria quando si alloca (ADR 0023).
	// Una che resta in Other e' un'allocazione che ha dimenticato la sua.
	enum class MemoryCategory : uint8_t {
		Other, ColorTextures, DataTextures, Environment, TriangleBuffers, SceneBuffers,
		Blas, Tlas, AccelScratch, RenderTargets, Staging, Count
	};
	const char* MemoryCategoryName(MemoryCategory category);

	// La memoria dell'app per categoria, divisa tra l'heap della GPU e il resto (host-visible),
	// e, se il driver ha VK_EXT_memory_budget, quanto ne usa davvero l'app e quanto puo' usarne.
	struct MemoryReport {
		VkDeviceSize Device[static_cast<size_t>(MemoryCategory::Count)] = {};
		VkDeviceSize Host[static_cast<size_t>(MemoryCategory::Count)] = {};
		VkDeviceSize HeapSize = 0;    // l'heap device-local piu' grande
		bool HasBudget = false;
		VkDeviceSize HeapBudget = 0;  // quanto puo' usarne l'app, tolti gli altri processi
		VkDeviceSize HeapUsage = 0;   // quanto ne usa l'app, driver compreso
		VkDeviceSize DeviceTotal() const;
	};

	struct GpuBuffer {
		VkBuffer Buffer = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkDeviceSize Size = 0;
		void* Mapped = nullptr;      // solo per i buffer host-visible
		VkDeviceAddress Address = 0; // solo con VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		VkDeviceSize Allocated = 0;  // la memoria davvero allocata, con allineamento e padding
		MemoryCategory Category = MemoryCategory::Other;
		bool DeviceLocal = false;
	};

	struct GpuImage {
		VkImage Image = VK_NULL_HANDLE;
		VkDeviceMemory Memory = VK_NULL_HANDLE;
		VkImageView View = VK_NULL_HANDLE;
		VkFormat Format = VK_FORMAT_UNDEFINED;
		uint32_t Width = 0, Height = 0, Layers = 1, Levels = 1;
		VkDeviceSize Allocated = 0;
		MemoryCategory Category = MemoryCategory::Other;
		bool DeviceLocal = false;
	};

	/*
	* Instance, device e swapchain, piu' il ciclo di vita del frame e la creazione di risorse.
	*
	* C'e' un solo frame in volo: BeginFrame aspetta la fine del precedente, quindi da li' in
	* poi nessuna risorsa e' in uso dalla GPU. Il path tracer accumula su immagini che
	* comunque si leggono frame dopo frame, quindi un secondo frame in volo non darebbe nulla.
	*/
	class VulkanContext {
	public:
		static VulkanContext& Get() { return *s_Instance; }

		bool Initialize(GLFWwindow* window, bool vsync);
		void Shutdown();

		// Ordine di un frame: BeginFrame, comandi compute su Cmd(), BeginUiPass, disegno ImGui,
		// EndUiPass, SubmitFrame, finestre secondarie di ImGui, PresentFrame.
		// BeginFrame ritorna false se il frame va saltato (finestra minimizzata, swapchain rifatta).
		bool BeginFrame();
		void BeginUiPass();
		void EndUiPass();
		void SubmitFrame();
		void PresentFrame();

		void SetVsync(bool vsync);
		VkCommandBuffer Cmd() const { return m_Cmd; }

		// Distrugge qualcosa dopo la fine del frame in corso: i comandi gia' registrati (o le
		// draw list di ImGui) possono ancora riferirlo.
		void Defer(std::function<void()> destroy) { m_Deferred.push_back(std::move(destroy)); }
		void FlushDeferred(); // solo a GPU ferma: BeginFrame dopo la fence, o allo spegnimento

		// Esegue comandi su un command buffer proprio e aspetta che finiscano. Con un nome, il
		// loro tempo GPU va nel profiler come parte del frame in corso (le build dei BLAS).
		// Ritorna il tempo GPU in millisecondi quando ha un nome, 0 altrimenti.
		float Immediate(const std::function<void(VkCommandBuffer)>& record, const char* gpuName = nullptr);

		// Gli scope GPU del command buffer del frame (GpuScope): si leggono dopo la sua fence
		GpuProfiler& FrameTimer() { return m_FrameTimer; }

		GpuBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory,
		                       MemoryCategory category = MemoryCategory::Other);
		void DestroyBuffer(GpuBuffer& buffer);
		void UploadBuffer(const GpuBuffer& destination, const void* data, VkDeviceSize size, VkDeviceSize offset = 0);

		GpuImage CreateImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
		                     MemoryCategory category, uint32_t layers = 1, uint32_t levels = 1, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);
		// Le immagini di calcolo stanno sempre in GENERAL: bastano le barriere di memoria.
		void TransitionToGeneral(const GpuImage& image);
		void DestroyImage(GpuImage& image);

		VkInstance Instance() const { return m_Instance; }
		VkPhysicalDevice PhysicalDevice() const { return m_PhysicalDevice; }
		VkDevice Device() const { return m_Device; }
		VkQueue Queue() const { return m_Queue; }
		uint32_t QueueFamily() const { return m_QueueFamily; }
		VkFormat SwapchainFormat() const { return m_SwapchainFormat; }
		uint32_t SwapchainImageCount() const { return static_cast<uint32_t>(m_SwapchainImages.size()); }
		const char* DeviceName() const { return m_DeviceProperties.deviceName; }
		float TimestampPeriod() const { return m_DeviceProperties.limits.timestampPeriod; }
		MemoryReport Memory() const; // i totali per categoria e, se c'e', il budget del driver

	private:
		bool CreateInstance(bool validation);
		bool PickPhysicalDevice();
		bool CreateDevice();
		void CreateSwapchain();
		void DestroySwapchainViews();
		uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
		bool IsDeviceLocal(uint32_t memoryType) const;
		void Count(MemoryCategory category, bool deviceLocal, VkDeviceSize bytes, bool add);

	private:
		static VulkanContext* s_Instance;

		GLFWwindow* m_Window = nullptr;
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_Messenger = VK_NULL_HANDLE;
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties m_DeviceProperties{};
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_Queue = VK_NULL_HANDLE;
		uint32_t m_QueueFamily = 0;

		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		VkFormat m_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
		VkExtent2D m_SwapchainExtent{};
		std::vector<VkImage> m_SwapchainImages;
		std::vector<VkImageView> m_SwapchainViews;
		std::vector<VkSemaphore> m_RenderFinished; // uno per immagine: la presentazione li tiene occupati
		bool m_Vsync = true;
		bool m_SwapchainDirty = false;

		VkCommandPool m_CommandPool = VK_NULL_HANDLE;
		VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
		VkCommandBuffer m_ImmediateCmd = VK_NULL_HANDLE;
		VkFence m_FrameFence = VK_NULL_HANDLE;
		VkFence m_ImmediateFence = VK_NULL_HANDLE;
		VkSemaphore m_ImageAvailable = VK_NULL_HANDLE;
		uint32_t m_ImageIndex = 0;
		std::vector<std::function<void()>> m_Deferred;

		MemoryReport m_Memory;       // i totali per categoria; il budget si legge in Memory()
		bool m_HasMemoryBudget = false;

		GpuProfiler m_FrameTimer;     // gli scope del frame, letti nel BeginFrame successivo
		GpuProfiler m_ImmediateTimer; // uno scope per Immediate, letto subito dopo la sua fence
		bool m_FrameSubmitted = false;
	};
}

#endif // VULKANCONTEXT_H
