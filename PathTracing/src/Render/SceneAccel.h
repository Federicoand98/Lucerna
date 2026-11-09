#pragma once

#ifndef SCENEACCEL_H
#define SCENEACCEL_H

#include "Vulkan/VulkanContext.h"
#include "Render/GpuTypes.h"
#include "Scene/Primitives.h"

namespace Lucerna {

	/*
	* Strutture di accelerazione per il ray query.
	*
	* - Un BLAS per asset di geometria, in spazio locale: cambia solo con la geometria, e le
	*   istanze dello stesso asset lo condividono.
	* - Un BLAS procedurale di AABB per sfere e quad, che stanno gia' in world space.
	* - Una TLAS con un'istanza per mesh e una per le primitive analitiche. Si ricostruisce quando
	*   cambia una delle due: e' cosi' che i transform delle mesh restano modificabili a costo basso.
	*
	* I BLAS delle mesh dividono un solo buffer: con migliaia di mesh, un'allocazione a testa
	* sforerebbe il limite di allocazioni del driver.
	*/
	// Quanto pesano le strutture costruite e quanto ci ha messo la GPU: le legge il profiler
	struct AccelStats {
		struct Mesh {
			VkDeviceSize Size = 0, Scratch = 0;
		};
		std::vector<Mesh> Meshes; // nello stesso ordine degli asset passati a BuildMeshes
		float MeshBuildMs = 0.0f; // i BLAS delle mesh si costruiscono tutti insieme
		uint32_t Primitives = 0;
		VkDeviceSize ProceduralSize = 0, ProceduralScratch = 0;
		float ProceduralBuildMs = 0.0f;
		uint32_t Instances = 0;
		VkDeviceSize TlasSize = 0, TlasScratch = 0;
		float TlasBuildMs = 0.0f;
	};

	class SceneAccel {
	public:
		SceneAccel();
		~SceneAccel();

		// Un BLAS per asset di geometria: di ogni MeshInfo contano solo FirstTriangle, NumTriangles, NumOpaque
		void BuildMeshes(const std::vector<MeshInfo>& assets, const GpuBuffer& positions);
		// primitives = le AABB di sfere e quad, in quest'ordine: l'indice di primitiva dello shader
		void BuildPrimitives(const std::vector<AABB>& primitives);
		// La TLAS: un'istanza per mesh, piu' quella delle primitive. Va rifatta dopo ognuno dei due build.
		// blas[i] = il BLAS dell'istanza i (-1 = nessuno): istanze dello stesso asset lo dividono
		void BuildTopLevel(const std::vector<MeshInfo>& meshes, const std::vector<int>& blas);

		const VkAccelerationStructureKHR& TopLevel() const { return m_Tlas; }
		const AccelStats& Stats() const { return m_Stats; }

	private:
		void DestroyMeshes();
		VkDeviceAddress AddressOf(VkAccelerationStructureKHR structure) const;

	private:
		std::vector<VkAccelerationStructureKHR> m_MeshBlas; // uno per asset, null se e' vuoto
		GpuBuffer m_MeshStorage;

		VkAccelerationStructureKHR m_ProceduralBlas = VK_NULL_HANDLE;
		GpuBuffer m_ProceduralStorage;
		GpuBuffer m_Aabbs;

		VkAccelerationStructureKHR m_Tlas = VK_NULL_HANDLE;
		GpuBuffer m_TlasStorage;
		GpuBuffer m_Instances;

		GpuBuffer m_Scratch;
		VkDeviceSize m_ScratchAlignment = 0;
		AccelStats m_Stats;
	};
}

#endif // SCENEACCEL_H
