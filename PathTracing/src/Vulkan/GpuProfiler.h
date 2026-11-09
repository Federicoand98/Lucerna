#pragma once

#ifndef VULKAN_GPUPROFILER_H
#define VULKAN_GPUPROFILER_H

#include "ptpch.h"
#include <vulkan/vulkan.h>
#include "Core/Profiler.h"

namespace Lucerna {

	// Tempi GPU con nome su un command buffer (ADR 0023): ogni scope scrive due timestamp in una
	// query pool, e dopo la fence di quel command buffer Collect li trasforma in millisecondi.
	// Gli scope si annidano: la profondita' e' quanti ce ne sono aperti. Oltre la capacita' della
	// pool uno scope non misura niente.
	class GpuProfiler {
	public:
		void Initialize(uint32_t capacity);
		void Release();

		void Reset(VkCommandBuffer cmd); // all'inizio del command buffer, fuori da un render pass
		void Begin(VkCommandBuffer cmd, std::string name);
		void End(VkCommandBuffer cmd);
		std::vector<GpuTiming> Collect(); // solo dopo la fence: i tempi nell'ordine degli scope

	private:
		struct Range {
			std::string Name;
			int Depth = 0;
			uint32_t First = UINT32_MAX; // il timestamp d'inizio; quello di fine e' First + 1
		};

		VkQueryPool m_Pool = VK_NULL_HANDLE;
		uint32_t m_Capacity = 0;
		uint32_t m_Used = 0;
		std::vector<Range> m_Ranges;
		std::vector<size_t> m_Open;
	};

	// Uno scope sul command buffer del frame: GpuScope scope(cmd, "Path tracer");
	class GpuScope {
	public:
		GpuScope(VkCommandBuffer cmd, std::string name);
		~GpuScope();
		GpuScope(const GpuScope&) = delete;
		GpuScope& operator=(const GpuScope&) = delete;
	private:
		VkCommandBuffer m_Cmd;
	};
}

#endif // VULKAN_GPUPROFILER_H
