//
// Created by Federico Andrucci on 21/08/23.
//

#include "Render/Renderer.h"
#include "Core/Profiler.h"
#include "Core/Paths.h"

#include "stb/stb_image_write.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Lucerna {

	namespace {
		// Deve combaciare con il blocco push_constant di Tonemap.comp.
		struct TonemapParams {
			float Exposure;
			int ToneMapper;
			int AovView;
			int SelectedCount;
			uint32_t Selected[Renderer::MaxSelectedIds];
		};

		static_assert(sizeof(TonemapParams) == 48, "TonemapParams deve combaciare con le push constant di Tonemap.comp");

		constexpr VkImageUsageFlags TARGET_USAGE = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	// Il colore e gli AOV vivono alla stessa risoluzione e vanno ridimensionati insieme:
	// lo shader li scrive con lo stesso texelCoord e imageSize().
	void Renderer::ResizeRenderTargets(uint32_t width, uint32_t height) {
		VulkanContext& vk = VulkanContext::Get();

		auto replace = [&](GpuImage& image) {
			GpuImage old = image;
			vk.Defer([old]() mutable { VulkanContext::Get().DestroyImage(old); });
			image = vk.CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, width, height, TARGET_USAGE, MemoryCategory::RenderTargets);
			vk.TransitionToGeneral(image);
		};
		replace(m_RenderedImage);
		replace(m_AlbedoAOV);
		for (int i = 0; i < 2; i++) {
			replace(m_NormalDepth[i]);
			replace(m_Irradiance[i]);
		}
		GpuImage old = m_ObjectIds;
		vk.Defer([old]() mutable { VulkanContext::Get().DestroyImage(old); });
		m_ObjectIds = vk.CreateImage(VK_FORMAT_R32_UINT, width, height, VK_IMAGE_USAGE_STORAGE_BIT, MemoryCategory::RenderTargets);
		vk.TransitionToGeneral(m_ObjectIds);

		// Guard: OnResize gira prima di Initialize, quando il denoiser non esiste ancora.
		if (m_Denoiser)
			m_Denoiser->Resize(width, height);
	}

	void Renderer::ResizeDisplay(uint32_t width, uint32_t height) {
		VulkanContext& vk = VulkanContext::Get();

		// Le draw list di questo frame riferiscono ancora l'immagine vecchia.
		GpuImage old = m_Display;
		vk.Defer([old]() mutable { VulkanContext::Get().DestroyImage(old); });

		m_Display = vk.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, width, height, TARGET_USAGE, MemoryCategory::RenderTargets);
		vk.TransitionToGeneral(m_Display);
		m_DisplayVersion++; // chi la mostra deve registrare l'immagine nuova
	}

	void Renderer::OnResize(uint32_t width, uint32_t height) {
		VulkanContext& vk = VulkanContext::Get();

		if (m_DisplaySampler == VK_NULL_HANDLE) {
			VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			samplerInfo.magFilter = VK_FILTER_LINEAR;
			samplerInfo.minFilter = VK_FILTER_LINEAR;
			samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			vkCreateSampler(vk.Device(), &samplerInfo, nullptr, &m_DisplaySampler);
		}

		if ((m_Width == width && m_Height == height) || width == 0 || height == 0)
			return;

		m_Width = width;
		m_Height = height;
		ResizeDisplay(width, height);
		ResizeRenderTargets(width, height);
		m_RenderW = width;   // i target sono ora a piena risoluzione
		m_RenderH = height;
	}

	namespace {
		// I campi che cambiano i campioni accumulati. Esposizione, tonemap e denoiser agiscono
		// dopo l'accumulo; render scale e history cap valgono solo mentre ci si muove. La vista
		// AOV scrive nell'immagine dell'accumulatore, e riagganciare la vista la sposta.
		auto AccumulationKey(const RenderSettings& s, const ViewSettings& v) {
			return std::tie(s.SamplesPerPixel, s.RayDepth, s.NEE, s.FireflyClamp, s.Reproject, v.AOVView, v.LockViewToCamera);
		}
	}

	void Renderer::Initialize() {
		VulkanContext& vk = VulkanContext::Get();

		m_Scene = std::make_unique<RenderScene>();
		m_PathTracer = std::make_unique<PathTracer>();
		m_Denoiser = std::make_unique<Denoiser>();
		m_Denoiser->Resize(m_Width, m_Height); // OnResize ha gia' fissato m_Width/m_Height
		m_Tonemap = std::make_unique<ComputePipeline>(ShaderPath("Tonemap.spv"),
			std::vector<VkDescriptorType>{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
			static_cast<uint32_t>(sizeof(TonemapParams)));
	}

	void Renderer::Render(const RenderView& view, Scene& scene, AssetLibrary& assets) {
		m_View = view;
		if (m_Width == 0 || m_Height == 0)
			return;

		VkCommandBuffer cmd = VulkanContext::Get().Cmd();

		// Come il picking: il contatore dei raggi del dispatch scorso si legge solo adesso
		if (m_RaysPending) {
			Profiler::Get().AttachRays(m_PathTracer->ReadRayCount(), m_RaysPixels);
			m_RaysPending = false;
		}

		// Il dispatch del picking era nel frame scorso: solo ora il suo risultato e' leggibile.
		if (m_PickPending) {
			m_PathTracer->ReadPickResult(m_PickType, m_PickIndex, m_PickDistance);
			m_PickPending = false;
			m_PickAvailable = true;
		}

		// La scena per la GPU segue scena e asset; qualunque loro cambiamento, o delle impostazioni
		// che toccano l'integratore, invalida i campioni accumulati.
		const uint32_t changes = scene.TakeChanges() | assets.TakeChanges();
		{
			PROFILE_SCOPE("RenderScene.Sync");
			m_Scene->Sync(scene, assets, changes);
		}
		if (changes != NoChange)
			Profiler::Get().LoadSynced(); // un caricamento in corso finisce col present di questo frame
		PROFILE_SCOPE("Record commands");
		if (changes != NoChange || AccumulationKey(Settings, View) != AccumulationKey(m_LastSettings, m_LastView))
			RestartAccumulation();
		m_LastSettings = Settings;
		m_LastView = View;

		// Render scale: mentre la camera si muove si renderizza a risoluzione ridotta (meno
		// raggi -> piu' fluido), poi da fermi si torna a piena risoluzione e riparte l'accumulo.
		// L'immagine ridotta viene poi upscalata dal sampler del tonemap sull'intero viewport.
		// Il picking forza la piena risoluzione: pickPixel e' in coordinate viewport e su una
		// griglia ridotta non corrisponderebbe a nessun texel.
		bool reduce = m_Interacting && Settings.RenderScale < 1.0f && !m_PickRequested;
		uint32_t rw = m_Width, rh = m_Height;
		if (reduce) {
			rw = std::max(8u, static_cast<uint32_t>(m_Width * Settings.RenderScale));
			rh = std::max(8u, static_cast<uint32_t>(m_Height * Settings.RenderScale));
		}
		if (rw != m_RenderW || rh != m_RenderH) {
			m_RenderW = rw;
			m_RenderH = rh;
			ResizeRenderTargets(rw, rh); // cambio risoluzione: l'accumulo precedente non e' piu' valido
			RestartAccumulation();
		}

		// Ping-pong: si scrive nell'indice corrente e si legge la storia dall'altro. Alla fine
		// del frame gli indici si scambiano, quindi la storia non costa nessuna copia.
		const int cur = m_Ping, prev = 1 - m_Ping;

		glm::ivec2 pickPixel = m_PickRequested ? glm::ivec2(m_PickX, m_PickY) : glm::ivec2(-1, -1);

		// Senza riproiezione si torna al comportamento classico: la camera che si muove invalida
		// l'accumulo e si riparte da capo. E' esattamente cio' che la riproiezione evita, quindi
		// il toggle serve solo come termine di paragone.
		if (!Settings.Reproject && m_Interacting)
			RestartAccumulation();

		// La storia non e' valida al primo frame dopo un reset (cambio scena/materiale/risoluzione):
		// m_PTCounter torna a 1 e i buffer contengono ancora il mondo precedente. Da fermo la
		// riproiezione e' l'identita', quindi questo path copre anche l'accumulo classico.
		bool hasHistory = m_PTCounter > 1;
		bool cameraMoved = m_Interacting;

		// Gli AOV dipendono solo dalla camera: si ritracciano solo se e' cambiata davvero.
		// Il confronto e' sulle matrici, non su Camera::OnUpdate: quello riporta il movimento
		// da tastiera, ma la rotazione col mouse cambia comunque la vista e lascerebbe AOV
		// stantii. Da fermo questo risparmia un raggio primario per pixel.
		glm::mat4 viewProjection = view.Projection * view.View;
		bool aovNeedsUpdate = (viewProjection != m_PrevViewProjection) || m_PTCounter <= 1;

		// In pausa l'accumulo resta com'e' e il path tracer non gira: denoiser e tonemap lavorano
		// sull'ultimo frame, che dopo lo scambio degli indici sta in prev. Un cambiamento che
		// invaliderebbe l'accumulo (modifica, camera, Reset) fa ripartire il rendering, altrimenti
		// si guarderebbe un'immagine che non e' piu' la scena. Il picking traccia comunque un
		// frame: e' un campione valido in piu'.
		if (!View.PathTracing && (aovNeedsUpdate || m_Interacting))
			View.PathTracing = true;
		const bool paused = !View.PathTracing && !m_PickRequested;
		const int last = paused ? prev : cur;

		ComputeUniformContainer container = { static_cast<uint32_t>(m_PTCounter), static_cast<uint32_t>(Settings.SamplesPerPixel), static_cast<uint32_t>(Settings.RayDepth), Settings.FireflyClamp, scene.Camera().Aperture, scene.Camera().FocusDistance, View.AOVView, m_PrevViewProjection, m_PrevCameraPosition, aovNeedsUpdate, hasHistory, cameraMoved, Settings.HistoryCap, Settings.NEE, pickPixel, *m_Scene, m_View };

		// Le scritture del frame scorso diventano visibili a questo, e il tonemap non deve
		// sovrascrivere il display mentre ImGui lo sta ancora leggendo.
		{
			VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		container.CountRays = m_CountRays;
		if (!paused) {
			GpuScope scope(cmd, "Path tracer");
			m_RaysPending = m_CountRays;
			m_RaysPixels = m_RenderW * m_RenderH;
			m_PathTracer->Dispatch(cmd, container, { m_RenderedImage, m_AlbedoAOV, m_NormalDepth[cur], m_Irradiance[cur], m_NormalDepth[prev], m_Irradiance[prev], m_ObjectIds });
		}
		ComputeBarrier(cmd);

		if (m_PickRequested) {
			m_PickRequested = false;
			m_PickPending = true;
		}

		// Denoiser à-trous (view filter): gira solo sulla vista Beauty, non in debug AOV.
		// L'output rimpiazza m_RenderedImage come sorgente del tonemap.
		// L'auto-fade ora vive nello shader ed e' PER-PIXEL (ogni pixel ha il suo N): qui resta
		// solo il peso manuale.
		//
		// Skip: le 5 passate full-screen sono frame rubati al path tracer, che accumula piu'
		// lentamente e quindi RESTA piu' rumoroso. Si puo' saltare solo se il fade e' trascurabile
		// OVUNQUE. Il minimo N sullo schermo e' ~m_FramesSinceMove: i pixel disoccclusi allo stop
		// sono i piu' indietro e crescono di 1 al frame. In movimento non si salta mai.
		m_FramesSinceMove = m_Interacting ? 0 : m_FramesSinceMove + 1;
		float minFade = 1.0f;
		if (Settings.DenoiseAutoFade && m_FramesSinceMove > 1)
			minFade = std::clamp(1.0f / std::sqrt(static_cast<float>(m_FramesSinceMove)), 0.0f, 1.0f);

		const GpuImage* source = &m_RenderedImage;
		if (Settings.Denoise && View.AOVView == 0 && Settings.DenoiseStrength * minFade > 0.01f) {
			GpuScope scope(cmd, "Denoiser");
			source = &m_Denoiser->Run(cmd, m_Irradiance[last], m_RenderedImage, m_AlbedoAOV, m_NormalDepth[last],
			                          Settings.DenoiseStrength, Settings.DenoiseAutoFade, Settings.DenoiseCPhi, Settings.DenoiseNPhi, Settings.DenoisePPhi, Settings.DenoiseAPhi);
		}

		{
			GpuScope scope(cmd, "Tonemap");
			TonemapParams tonemap = { Settings.Exposure, static_cast<int>(Settings.ToneMap), View.AOVView, 0, {} };
			for (uint32_t id : m_SelectedIds)
				if (tonemap.SelectedCount < MaxSelectedIds)
					tonemap.Selected[tonemap.SelectedCount++] = id;
			m_Tonemap->Bind(cmd);
			m_Tonemap->Sampler(0, *source, m_DisplaySampler, VK_IMAGE_LAYOUT_GENERAL).Image(1, m_Display).Image(2, m_ObjectIds).Commit(cmd);
			m_Tonemap->PushConstants(cmd, &tonemap, sizeof(tonemap));
			vkCmdDispatch(cmd, (m_Width + 7) / 8, (m_Height + 7) / 8, 1);
		}

		// ImGui campiona il display nel fragment shader.
		{
			VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		if (paused)
			return;

		// Il frame corrente diventa la storia del prossimo: si scambiano gli indici e si
		// memorizza la camera con cui e' stato prodotto (serve a riproiettare e a validare).
		m_Ping = 1 - m_Ping;
		m_PrevViewProjection = view.Projection * view.View;
		m_PrevCameraPosition = view.Position;
		m_PTCounter++;
	}

	void Renderer::ReadImage(const GpuImage& image, void* pixels, size_t bytes) {
		VulkanContext& vk = VulkanContext::Get();
		vkDeviceWaitIdle(vk.Device());

		GpuBuffer staging = vk.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, MemoryCategory::Staging);
		vk.Immediate([&](VkCommandBuffer cmd) {
			VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
			VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(cmd, &dependency);

			VkBufferImageCopy region{};
			region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.imageExtent = { image.Width, image.Height, 1 };
			vkCmdCopyImageToBuffer(cmd, image.Image, VK_IMAGE_LAYOUT_GENERAL, staging.Buffer, 1, &region);
		});
		memcpy(pixels, staging.Mapped, bytes);
		vk.DestroyBuffer(staging);
	}

	// Il contenuto e' gia' passato dal tonemap, quindi il file corrisponde a cio' che si vede.
	std::string Renderer::SaveScreenshot(const std::string& path) {
		std::string fileName = path;
		if (fileName.empty()) {
			// nella cartella Immagini dell'utente, non accanto ai dati (ADR 0022)
			const std::filesystem::path folder = UserPicturesDirectory();
			std::filesystem::create_directories(folder);
			fileName = (folder / (std::to_string(screenshotsCount++) + ".png")).string();
		} else {
			std::filesystem::path p(fileName);
			if (p.has_parent_path())
				std::filesystem::create_directories(p.parent_path());
		}

		std::vector<unsigned char> pixels(static_cast<size_t>(m_Display.Width) * m_Display.Height * 4);
		ReadImage(m_Display, pixels.data(), pixels.size());

		// riga 0 = NDC y -1, cioe' il fondo dell'immagine: i PNG partono dall'alto
		stbi_flip_vertically_on_write(true);
		if (!stbi_write_png(fileName.c_str(), m_Display.Width, m_Display.Height, 4, pixels.data(), m_Display.Width * 4)) {
			std::cout << "Screenshot: scrittura fallita: " << fileName << std::endl;
			return "";
		}

		std::cout << "Screenshot: " << fileName << " (" << m_Display.Width << "x" << m_Display.Height << ")" << std::endl;
		return fileName;
	}

	void Renderer::SaveHdr(const std::string& path) {
		const int width = static_cast<int>(m_RenderedImage.Width), height = static_cast<int>(m_RenderedImage.Height);
		std::vector<float> pixels(static_cast<size_t>(width) * height * 4);
		ReadImage(m_RenderedImage, pixels.data(), pixels.size() * sizeof(float));

		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(&width), sizeof(int));
		file.write(reinterpret_cast<const char*>(&height), sizeof(int));
		file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(float));
	}

	void Renderer::Release() {
		VulkanContext& vk = VulkanContext::Get();
		m_PathTracer.reset();
		m_Scene.reset();
		m_Denoiser.reset();
		m_Tonemap.reset();
		for (GpuImage* image : { &m_RenderedImage, &m_AlbedoAOV, &m_NormalDepth[0], &m_NormalDepth[1], &m_Irradiance[0], &m_Irradiance[1], &m_Display, &m_ObjectIds })
			vk.DestroyImage(*image);
		if (m_DisplaySampler != VK_NULL_HANDLE)
			vkDestroySampler(vk.Device(), m_DisplaySampler, nullptr);
		m_DisplaySampler = VK_NULL_HANDLE;
	}
}
