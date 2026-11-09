#include "Vulkan/GpuProfiler.h"
#include "Vulkan/VulkanContext.h"

namespace Lucerna {

	void GpuProfiler::Initialize(uint32_t capacity) {
		VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = capacity;
		vkCreateQueryPool(VulkanContext::Get().Device(), &info, nullptr, &m_Pool);
		m_Capacity = capacity;
	}

	void GpuProfiler::Release() {
		if (m_Pool != VK_NULL_HANDLE)
			vkDestroyQueryPool(VulkanContext::Get().Device(), m_Pool, nullptr);
		m_Pool = VK_NULL_HANDLE;
	}

	void GpuProfiler::Reset(VkCommandBuffer cmd) {
		vkCmdResetQueryPool(cmd, m_Pool, 0, m_Capacity);
		m_Used = 0;
		m_Ranges.clear();
		m_Open.clear();
	}

	// ALL_COMMANDS su entrambi i lati: il timestamp si scrive quando i comandi prima sono finiti,
	// quindi una passata non si prende la coda di quella precedente.
	void GpuProfiler::Begin(VkCommandBuffer cmd, std::string name) {
		Range range;
		range.Name = std::move(name);
		range.Depth = static_cast<int>(m_Open.size());
		if (m_Used + 2 <= m_Capacity) {
			range.First = m_Used;
			m_Used += 2;
			vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_Pool, range.First);
		}
		m_Open.push_back(m_Ranges.size());
		m_Ranges.push_back(std::move(range));
	}

	void GpuProfiler::End(VkCommandBuffer cmd) {
		if (m_Open.empty())
			return;
		const Range& range = m_Ranges[m_Open.back()];
		m_Open.pop_back();
		if (range.First != UINT32_MAX)
			vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_Pool, range.First + 1);
	}

	std::vector<GpuTiming> GpuProfiler::Collect() {
		std::vector<GpuTiming> timings;
		if (m_Used == 0)
			return timings;
		std::vector<uint64_t> ticks(m_Used);
		if (vkGetQueryPoolResults(VulkanContext::Get().Device(), m_Pool, 0, m_Used, ticks.size() * sizeof(uint64_t), ticks.data(),
		                          sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			return timings;

		const double toMs = VulkanContext::Get().TimestampPeriod() * 1e-6;
		for (const Range& range : m_Ranges)
			if (range.First != UINT32_MAX)
				timings.push_back({ range.Name, range.Depth, static_cast<float>((ticks[range.First + 1] - ticks[range.First]) * toMs) });
		m_Used = 0;
		m_Ranges.clear();
		return timings;
	}

	GpuScope::GpuScope(VkCommandBuffer cmd, std::string name) : m_Cmd(cmd) {
		VulkanContext::Get().FrameTimer().Begin(cmd, std::move(name));
	}

	GpuScope::~GpuScope() {
		VulkanContext::Get().FrameTimer().End(m_Cmd);
	}
}
