// Controlli della logica senza GPU: si lanciano con ./build/LucernaChecks, escono con 1 al primo
// che fallisce. Ne aggiungo uno quando una logica ha casi limite che un render non mostra.
#include "Core/ObjectId.h"
#include "Core/SceneObject.h"
#include "Scene/Scene.h"
#include "Import/SceneFiles.h"
#include "Import/Importer.h"
#include "Serialization/BuiltinScenes.h"
#include "Serialization/SceneSerializer.h"
#include "Editor/Document.h"
#include "Editor/BlenderLink.h"
#include "Core/Paths.h"
#include "Core/Parallel.h"
#include "Core/Profiler.h"
#include "Core/AliasTable.h"
#include "Core/Json.h"
#include "Render/RenderSettings.h"

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

struct Thing { int Value = 0; };

int main() {
	using namespace Lucerna;

	// ObjectId: 16 cifre esadecimali che si rileggono uguali; zero e' "nessuno"
	const ObjectId id = ObjectId::Generate();
	const std::string text = id.ToString();
	CHECK(text.size() == 16 && !id.IsNil());
	CHECK(ObjectId::Parse(text) == id);
	CHECK(!ObjectId::Parse("not-an-id").has_value());
	CHECK(!ObjectId::Parse("3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f").has_value()); // vecchio UUID a 128 bit
	CHECK(ObjectId::Parse("0000000000000000")->IsNil());
	CHECK(ObjectId::Parse("00000000000000ff")->Value == 255);
	std::unordered_set<ObjectId> seen;
	for (int i = 0; i < 10000; i++) seen.insert(ObjectId::Generate());
	CHECK(seen.size() == 10000);

	// ParallelFor: ogni indice una volta sola, anche annidato e su chiamate ripetute (il pool si riusa)
	for (int round = 0; round < 50; round++) {
		std::vector<std::atomic<int>> hits(1000);
		ParallelFor(hits.size(), [&](size_t i) {
			hits[i]++;
			if (i % 100 == 0)
				ParallelFor(3, [&](size_t) { hits[i]++; });
		});
		bool once = true;
		for (size_t i = 0; i < hits.size(); i++)
			once = once && hits[i] == (i % 100 == 0 ? 4 : 1);
		CHECK(once);
	}
	ParallelFor(0, [&](size_t) { CHECK(false); });

	// Profiler: p99 nearest-rank, e la diagnosi dei tre casi (ADR 0023)
	{
		std::vector<float> values;
		for (int i = 1; i <= 100; i++) values.push_back(static_cast<float>(i));
		const Summary sum = Summarize(values);
		CHECK(sum.Min == 1.0f && sum.Max == 100.0f && sum.P99 == 99.0f && sum.Avg == 50.5f);
		CHECK(Summarize({}).Max == 0.0f && Summarize({ 3.0f }).P99 == 3.0f);

		auto frame = [](float frameMs, float fence, float present, float pt) {
			FrameRecord f;
			f.FrameMs = frameMs; f.FenceMs = fence; f.PresentMs = present;
			f.Gpu = { { "Path tracer", 0, pt }, { "Pass 1", 1, 0.5f } }; // una parte non conta nel totale
			return f;
		};
		const FrameRecord gpuBound = frame(7.4f, 5.8f, 0.3f, 6.1f);   // aspetta la GPU, che lavora
		const FrameRecord vsync = frame(13.9f, 0.1f, 12.5f, 6.1f);     // aspetta il display
		const FrameRecord vsyncInFence = frame(13.9f, 12.0f, 0.2f, 1.0f); // il display, visto come fence
		const FrameRecord cpuBound = frame(4.4f, 0.1f, 0.3f, 0.9f);    // la CPU lavora 4 ms
		const FrameRecord gpuInPresent = frame(3.2f, 0.0f, 2.97f, 2.8f); // NVIDIA: aspetta la GPU nel present
		CHECK(Diagnose({ &gpuInPresent }).Limit == Bottleneck::Gpu);
		CHECK(std::abs(gpuInPresent.GpuWaitMs() - 2.8f) < 1e-4f && std::abs(gpuInPresent.PresentWaitMs() - 0.17f) < 1e-4f);
		CHECK(Diagnose({ &gpuBound }).Limit == Bottleneck::Gpu);
		CHECK(Diagnose({ &gpuBound }).TopPass == "Path tracer" && Diagnose({ &gpuBound }).GpuBusyMs == 6.1f);
		CHECK(Diagnose({ &vsync }).Limit == Bottleneck::Present);
		CHECK(Diagnose({ &vsyncInFence }).Limit == Bottleneck::Present);
		CHECK(Diagnose({ &cpuBound }).Limit == Bottleneck::Cpu);
		CHECK(Diagnose({}).Limit == Bottleneck::None);

		// un giro aspetta i tempi GPU del suo frame; le build in Immediate vengono dopo le passate
		Profiler& profiler = Profiler::Get();
		profiler.Reset();
		profiler.BeginFrame();
		profiler.AddImmediateGpu({ "BLAS build", 0, 2.0f });
		profiler.BeginFrame();                        // il primo giro ora aspetta la GPU
		CHECK(profiler.History().empty());
		profiler.AttachFrameGpu({ { "Path tracer", 0, 1.0f } });
		CHECK(profiler.History().size() == 1 && profiler.History()[0].Gpu.size() == 2);
		CHECK(profiler.History()[0].Gpu[0].Name == "Path tracer" && profiler.History()[0].Gpu[1].Name == "BLAS build");
		profiler.AttachFrameGpu({ { "Path tracer", 0, 1.0f } }); // niente in attesa: si ignora
		CHECK(profiler.History().size() == 1);
		profiler.SetPaused(true);                     // in pausa lo storico resta, Latest no
		profiler.BeginFrame();
		profiler.AttachFrameGpu({ { "Path tracer", 0, 7.0f } });
		CHECK(profiler.History().size() == 1 && profiler.Latest().GpuMs("Path tracer") == 7.0f);
		profiler.BeginFrame();
		profiler.BeginFrame();                        // un giro senza GPU (finestra ridotta) si tiene lo stesso
		CHECK(profiler.Latest().Gpu.empty());
		profiler.Reset();

		// scope CPU: annidati in preordine, nuovi a ogni giro, ignorati fuori dal main thread
		profiler.BeginFrame();
		{
			PROFILE_SCOPE("A");
			{ PROFILE_SCOPE("B"); }
			ParallelFor(64, [](size_t) { PROFILE_SCOPE("worker"); });
			std::thread([] { PROFILE_SCOPE("thread"); }).join();
		}
		{ PROFILE_SCOPE("C"); }
		profiler.BeginScope("left open"); // un giro lo chiude da se'
		profiler.BeginFrame();
		profiler.AttachFrameGpu({});
		const std::vector<CpuTiming>& cpu = profiler.History().back().Cpu;
		CHECK(cpu.size() == 4);
		CHECK(cpu.size() == 4 && cpu[0].Name == "A" && cpu[0].Depth == 0 && cpu[1].Name == "B" && cpu[1].Depth == 1);
		CHECK(cpu.size() == 4 && cpu[2].Name == "C" && cpu[2].Depth == 0 && cpu[3].Name == "left open");
		CHECK(cpu.size() == 4 && cpu[0].Ms >= cpu[1].Ms);
		CHECK(cpu.size() == 4 && profiler.ScopePath(cpu[1].Id) == "A/B" && profiler.ScopeParent(cpu[1].Id) == static_cast<int>(cpu[0].Id));
		CHECK(cpu.size() == 4 && profiler.ScopeParent(cpu[2].Id) == -1 && cpu[2].Id != cpu[0].Id);
		{ PROFILE_SCOPE("A"); }
		profiler.BeginFrame();
		profiler.AttachFrameGpu({});
		CHECK(profiler.History().back().Cpu.size() == 1 && profiler.History().back().Cpu[0].Depth == 0);
		CHECK(profiler.History().back().Cpu.size() == 1 && profiler.History().back().Cpu[0].Id == cpu[0].Id); // lo stesso percorso, lo stesso id
		for (size_t i = 0; i < Profiler::MaxScopes + 10; i++) { PROFILE_SCOPE("many"); }
		profiler.BeginFrame();
		profiler.AttachFrameGpu({});
		CHECK(profiler.History().back().Cpu.size() == Profiler::MaxScopes);
		profiler.Reset();

		// la RAM del processo si legge, e il picco non e' mai sotto la residente
		const ProcessMemoryInfo ram = ProcessMemory();
		CHECK(ram.Resident > 0 && ram.Peak >= ram.Resident);

		// caricamenti: aperti dentro uno scope (il menu), chiusi dal primo present dopo il Sync
		using namespace std::chrono_literals;
		profiler.BeginFrame();
		{
			PROFILE_SCOPE("UI");
			profiler.BeginLoad("scene.json");
			PROFILE_SCOPE("Read scene file");
			std::this_thread::sleep_for(3ms);
			{ PROFILE_SCOPE("too short"); }
		}
		CHECK(!profiler.FramePresented() && profiler.Loading()); // il Sync non c'e' ancora stato
		profiler.BeginFrame();
		{
			PROFILE_SCOPE("Renderer");
			PROFILE_SCOPE("RenderScene.Sync");
			std::this_thread::sleep_for(3ms);
		}
		profiler.LoadSynced();
		CHECK(profiler.FramePresented() && !profiler.Loading());
		const LoadCapture& load = profiler.LastLoad();
		CHECK(load.Label == "scene.json" && load.TotalMs >= 6.0f);
		CHECK(load.Phases.size() >= 3 && load.Phases[0].Name == "Read scene file" && load.Phases[0].Depth == 0);
		CHECK(load.Phases.size() >= 3 && load.Phases[1].Name == "Renderer" && load.Phases[1].Depth == 0);
		CHECK(load.Phases.size() >= 3 && load.Phases[2].Name == "RenderScene.Sync" && load.Phases[2].Depth == 1);
		CHECK(std::none_of(load.Phases.begin(), load.Phases.end(), [](const LoadPhase& p) { return p.Name == "too short" || p.Name == "UI"; }));
		CHECK(!profiler.FramePresented()); // gia' chiuso

		profiler.BeginLoad("broken.obj"); // un caricamento fallito non sostituisce l'ultimo
		profiler.CancelLoad();
		profiler.LoadSynced();
		CHECK(!profiler.FramePresented() && profiler.LastLoad().Label == "scene.json");
		profiler.Reset();
	}

#ifndef _WIN32
	// Il collegamento con Blender (ADR 0024): un finto Blender che dorme, e i manifest scritti
	// come li scrive l'addon (rename atomica)
	{
		namespace fs = std::filesystem;
		CHECK(BlenderLink::FindBlender("/bin/sh") == "/bin/sh");
		CHECK(BlenderLink::FindBlender("/no/such/blender").empty());
		setenv("LUCERNA_RESOURCES", (fs::current_path() / "res").c_str(), 1);
		CHECK(fs::exists(BlenderLink::ScriptPath()));

		const fs::path dir = fs::temp_directory_path() / "lucerna-checks-link";
		fs::create_directories(dir);
		const fs::path fake = dir / "fake-blender";
		std::ofstream(fake) << "#!/bin/sh\nsleep 3\n";
		fs::permissions(fake, fs::perms::owner_all);
		const fs::path manifest = dir / "link.json";
		auto write = [&](const std::string& json) {
			std::ofstream(manifest.string() + ".tmp") << json;
			fs::rename(manifest.string() + ".tmp", manifest);
			std::this_thread::sleep_for(std::chrono::milliseconds(300)); // oltre ManifestPollSeconds: l'mtime puo' non cambiare
		};

		BlenderLink link(manifest);
		CHECK(link.GetState() == BlenderLink::State::Off && !link.Poll());
		std::string error;
		CHECK(link.Launch(fake.string(), error) && error.empty());
		CHECK(!link.Poll() && link.GetState() == BlenderLink::State::Starting);
		write(R"({"version": 1, "sync": 0, "status": "unsaved"})");
		CHECK(!link.Poll() && link.GetState() == BlenderLink::State::Unsaved && link.CanSync());
		write(R"({"version": 1, "sync": 1, "status": "exported", "blend": "/a/room.blend", "gltf": "/a/lucerna_live/scene.gltf", "exportMs": 40})");
		const auto first = link.Poll();
		CHECK(first && first->Number == 1 && first->Blend == "/a/room.blend" && link.GetState() == BlenderLink::State::Linked);
		CHECK(!link.Poll()); // lo stesso export non si applica due volte
		write(R"({"version": 2, "sync": 2, "status": "exported"})"); // un formato che non si conosce si ignora
		CHECK(!link.Poll());
		write(R"({"version": 1, "sync": 3, "status": "exported", "blend": "/a/hall.blend", "gltf": "/a/x.gltf"})");
		const auto second = link.Poll();
		CHECK(second && second->Number == 3 && second->Blend == "/a/hall.blend");
		link.RequestSync();
		CHECK(fs::exists(manifest.string() + ".request"));
		link.Disconnect();
		CHECK(link.GetState() == BlenderLink::State::Off && !link.Poll());
		fs::remove_all(dir);
	}
#endif

	// Alias table (ADR 0025): ogni indice esce con probabilita' peso / totale, esattamente
	{
		auto probabilities = [](const std::vector<AliasEntry>& table) {
			std::vector<double> p(table.size(), 0.0);
			for (size_t k = 0; k < table.size(); k++) {
				p[k] += table[k].Prob / table.size();
				p[table[k].Alias] += (1.0 - table[k].Prob) / table.size();
			}
			return p;
		};
		const std::vector<float> weights = { 1.0f, 0.0f, 7.0f, 2.0f, 0.5f, 0.0f, 3.5f };
		const std::vector<AliasEntry> table = BuildAliasTable(weights);
		const std::vector<double> p = probabilities(table);
		CHECK(table.size() == weights.size());
		bool exact = true;
		for (size_t i = 0; i < weights.size(); i++)
			exact = exact && std::abs(p[i] - weights[i] / 14.0) < 1e-6;
		CHECK(exact);
		CHECK(BuildAliasTable({}).empty() && BuildAliasTable({ 0.0f, -1.0f }).empty());
		CHECK(BuildAliasTable({ 5.0f }).size() == 1 && BuildAliasTable({ 5.0f })[0].Prob == 1.0f);
		// tanti pesi molto diversi, come i triangoli di una mesh: l'errore resta piccolo
		std::vector<float> many(100000);
		double total = 0.0;
		for (size_t i = 0; i < many.size(); i++)
			total += many[i] = (i % 97 == 0) ? 1000.0f : 0.001f * float(i % 13 + 1);
		const std::vector<double> pm = probabilities(BuildAliasTable(many));
		double worst = 0.0;
		for (size_t i = 0; i < many.size(); i++)
			worst = std::max(worst, std::abs(pm[i] - many[i] / total) / (many[i] / total));
		CHECK(worst < 1e-3);
	}

	// Nomi unici come in Blender
	ObjectList<Thing> list("Thing");
	CHECK(list.Add({}).Name == "Thing");
	CHECK(list.Add({}).Name == "Thing.001");
	CHECK(list.Add({}, "stone").Name == "stone");
	CHECK(list.Add({}, "stone").Name == "stone.001");
	CHECK(list.Add({}, "stone.001").Name == "stone.002");   // la base e' "stone"
	CHECK(list.Add({}, "v1.5").Name == "v1.5");             // ".5" non e' un suffisso .NNN
	list.Rename(list[2], "stone");                            // rinominarsi col proprio nome non lo cambia
	CHECK(list[2].Name == "stone");
	list.Rename(list[2], "Thing");
	CHECK(list[2].Name == "Thing.002");

	// Identita' stabile: cancellare un oggetto non cambia gli altri, e assegnare dati la conserva
	const ObjectId third = list[3].Id;
	list.erase(list.begin() + 1);
	CHECK(list.IndexOf(third) == 2 && list[2].Name == "stone.001");
	list[2] = Thing{ 42 };
	CHECK(list[2].Id == third && list[2].Value == 42 && list[2].Name == "stone.001");
	CHECK(list.Pack().size() == list.size());

	// Change tracking (issue #25, #29): ogni modifica dice cosa ha toccato, e solo quello.
	// Scene e AssetLibrary hanno ciascuna i suoi flag.
	Scene scene;
	AssetLibrary library;
	CHECK(scene.TakeChanges() == SceneChanges && library.TakeChanges() == AssetChanges); // tutto da caricare
	CHECK(scene.TakeChanges() == NoChange && library.TakeChanges() == NoChange);         // e si consumano
	const ObjectId materialId = library.AddMaterial(Material{});
	CHECK(library.TakeChanges() == MaterialsChanged && scene.TakeChanges() == NoChange);
	const int s = scene.AddSphere(Sphere{});
	CHECK(scene.TakeChanges() == PrimitivesChanged);
	const ObjectId sphereId = scene.Spheres()[s].Id;
	Sphere moved = scene.Spheres()[s];
	moved.Position.x = 3.0f;
	scene.Set(sphereId, moved);
	CHECK(scene.TakeChanges() == PrimitivesChanged && scene.Spheres()[s].Position.x == 3.0f);
	CHECK(scene.Spheres()[s].Id == sphereId);           // Set cambia i dati, non l'identita'
	library.Set(materialId, Material{});
	CHECK(library.TakeChanges() == MaterialsChanged);
	CHECK(scene.Rename(sphereId, "ball") && !library.Rename(sphereId, "ball")); // il nome non cambia l'immagine
	CHECK(scene.TakeChanges() == NoChange && scene.Spheres()[s].Name == "ball");
	CHECK(library.Rename(materialId, "clay") && library.TakeChanges() == NoChange);
	scene.MoveCamera(glm::vec3(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)); // la vista che naviga
	CHECK(scene.TakeChanges() == NoChange && scene.Camera().Position == glm::vec3(1.0f));
	scene.SetCamera(scene.Camera());
	CHECK(scene.TakeChanges() == CameraChanged);
	scene.Set(ObjectId::Generate(), moved);             // un id che non c'e' non fa niente
	CHECK(scene.TakeChanges() == NoChange);
	scene.CreateBox(glm::vec3(0.0f), glm::vec3(1.0f), ObjectId{});
	CHECK(scene.TakeChanges() == PrimitivesChanged);
	Box box = scene.Boxes()[0];
	const std::vector<Quad> facesBefore = scene.Quads().Pack();
	box.Max = glm::vec4(2.0f, 1.0f, 1.0f, 0.0f);
	scene.Set(scene.Boxes()[0].Id, box);
	CHECK(scene.TakeChanges() == PrimitivesChanged);
	const std::vector<Quad> facesAfter = scene.Quads().Pack();
	bool facesMoved = false;                            // il box riscrive le sue 6 facce
	for (size_t i = 0; i < facesAfter.size(); i++)
		facesMoved |= facesAfter[i].Width != facesBefore[i].Width || facesAfter[i].Height != facesBefore[i].Height
		              || facesAfter[i].PositionLLC != facesBefore[i].PositionLLC;
	CHECK(facesAfter.size() == 6 && facesMoved);
	scene.RemoveSphere(s);
	CHECK(scene.TakeChanges() == PrimitivesChanged && scene.Spheres().empty());

	// Asset (issue #26): lo stesso OBJ piazzato due volte si carica una volta sola
	Scene placed;
	AssetLibrary assets;
	const int first = AddFile("res/models/pawn.obj", placed, assets, glm::mat4(1.0f), ObjectId{});
	const int second = AddFile("res/models/pawn.obj", placed, assets, glm::mat4(2.0f), ObjectId{});
	CHECK(first == 0 && second == 1 && placed.Meshes().size() == 2);
	CHECK(assets.Meshes().size() == 1 && placed.Meshes()[0].Asset == placed.Meshes()[1].Asset);
	CHECK(assets.FindMesh(placed.Meshes()[1].Asset) != nullptr && !assets.FindMesh(placed.Meshes()[1].Asset)->Triangles.empty());

	// Una texture usata da un materiale non si toglie; una inutilizzata si', e libera il suo posto
	assets.TakeChanges();
	assets.RegisterTexture("textures/a.png", TextureKind::Color);
	assets.RegisterTexture("textures/a.png", TextureKind::Color);     // stessa immagine e tipo: una sola
	assets.RegisterTexture("textures/a.png", TextureKind::Data);      // come dato e' un altro asset
	CHECK(assets.Textures().size() == 2 && assets.TakeChanges() == TexturesChanged);
	Material textured;
	textured.AlbedoTexture = assets.Textures()[0].Id;
	assets.AddMaterial(textured);
	CHECK(!assets.RemoveTexture(assets.Textures()[0].Id) && assets.Textures().size() == 2);
	CHECK(assets.RemoveTexture(assets.Textures()[1].Id) && assets.Textures().size() == 1);

	// Registry degli importer (issue #27): per estensione, senza badare alle maiuscole
	CHECK(FindImporter("a/b/model.OBJ") != nullptr && std::string(FindImporter("a/b/model.OBJ")->Name()) == "Wavefront OBJ");
	CHECK(FindImporter("scene.gltf") != nullptr && FindImporter("scene.glb") == FindImporter("scene.gltf"));
	CHECK(FindImporter("scene.fbx") == nullptr && FindImporter("noextension") == nullptr);
	// Un file che non si apre lascia scena e asset come erano
	const size_t instancesBefore = placed.Meshes().size(), materialsBefore = assets.Materials().size();
	CHECK(!OpenFile("res/models/missing.gltf", placed, assets) && !OpenFile("res/models/scene.fbx", placed, assets));
	CHECK(placed.Meshes().size() == instancesBefore && assets.Materials().size() == materialsBefore);
	CHECK(AddFile("res/models/missing.obj", placed, assets, glm::mat4(1.0f), ObjectId{}) == -1);
	// Open di un OBJ: la scena diventa il modello, con i suoi asset e un materiale di default
	CHECK(OpenFile("res/models/pawn.obj", placed, assets) && placed.Meshes().size() == 1 && assets.Meshes().size() == 1);
	CHECK(assets.Materials().size() == 1 && placed.Meshes()[0].Material == assets.Materials()[0].Id);

	// Scene integrate (issue #28): i file di res/scenes/builtin, per numero o per nome
	const std::vector<BuiltinScene> builtin = ListBuiltinScenes("res/scenes/builtin");
	CHECK(builtin.size() == 13 && builtin.front().Number == 0 && builtin.back().Number == 14);
	CHECK(FindBuiltinScene(builtin, "6") != nullptr && FindBuiltinScene(builtin, "6")->Key == "setup-2");
	CHECK(FindBuiltinScene(builtin, "sponza") != nullptr && FindBuiltinScene(builtin, "sponza")->Number == 8);
	CHECK(FindBuiltinScene(builtin, "zero-day") != nullptr && FindBuiltinScene(builtin, "zero-day")->Number == 13);
	CHECK(FindBuiltinScene(builtin, "cornell-emissive-mesh") == &builtin.back());
	CHECK(FindBuiltinScene(builtin, "4") != nullptr && FindBuiltinScene(builtin, "4")->DisplayName == "Cornell Box Meshes");
	CHECK(FindBuiltinScene(builtin, "1") == nullptr && FindBuiltinScene(builtin, "") == nullptr);  // le scene casuali non ci sono piu'

	// Documento: le revisioni crescono a ogni modifica da salvare, anche un nome, non navigando
	Scene doc;
	AssetLibrary docAssets;
	RenderSettings docSettings;
	const uint64_t rev0 = doc.Revision();
	doc.MoveCamera(glm::vec3(2.0f), glm::vec3(0.0f, 0.0f, -1.0f));
	CHECK(doc.Revision() == rev0);
	const int docSphere = doc.AddSphere(Sphere{});
	CHECK(doc.Revision() > rev0);
	const uint64_t rev1 = doc.Revision();
	doc.Rename(doc.Spheres()[docSphere].Id, "named");
	CHECK(doc.Revision() > rev1);

	// Path relativi al file di scena (formato 3), avanti e indietro
	CHECK(CanonicalPath(std::filesystem::absolute("res/models/pawn.obj")) == "res/models/pawn.obj");
	CHECK(RelativeToFile("res/models/pawn.obj", "build/scene.json") == "../res/models/pawn.obj");
	CHECK(ResolveFromFile("../res/models/pawn.obj", "build/scene.json") == "res/models/pawn.obj");
#ifdef _WIN32
	CHECK(ResolveFromFile("C:/abs/model.obj", "build/scene.json") == "C:/abs/model.obj"); // su Windows un path assoluto ha il drive
#else
	CHECK(ResolveFromFile("/abs/model.obj", "build/scene.json") == "/abs/model.obj");
#endif

	Document document;
	document.New(doc, docAssets, docSettings);
	CHECK(!document.IsModified(doc, docAssets, docSettings) && document.Origin() == DocumentOrigin::New && !document.CanSaveInPlace());
	CHECK(document.DisplayName() == "Untitled");
	CHECK(AddFile("res/models/pawn.obj", doc, docAssets, glm::mat4(1.0f), ObjectId{}) == 0);
	CHECK(document.IsModified(doc, docAssets, docSettings));
	CHECK(document.SaveAs("build/check_document.json", doc, docAssets, docSettings));
	CHECK(!document.IsModified(doc, docAssets, docSettings) && document.CanSaveInPlace() && document.DisplayName() == "check_document.json");
	docSettings.RayDepth += 1;                          // le impostazioni di render si salvano col file
	CHECK(document.IsModified(doc, docAssets, docSettings));
	CHECK(document.Save(doc, docAssets, docSettings) && !document.IsModified(doc, docAssets, docSettings));
	// Riaprirlo ritrova il modello: il path salvato e' relativo a build/, quello riletto di nuovo a res/
	Scene reopened;
	AssetLibrary reopenedAssets;
	RenderSettings reopenedSettings;
	Document reopenedDoc;
	CHECK(reopenedDoc.Open("build/check_document.json", reopened, reopenedAssets, reopenedSettings));
	CHECK(reopenedDoc.Origin() == DocumentOrigin::SceneFile && !reopenedDoc.IsModified(reopened, reopenedAssets, reopenedSettings));
	CHECK(reopenedAssets.Meshes().size() == 1 && reopenedAssets.Meshes()[0].Source.File == "res/models/pawn.obj");
	CHECK(!reopenedAssets.Meshes()[0].Triangles.empty() && reopenedSettings.RayDepth == docSettings.RayDepth);
	// Un file importato o una scena integrata non si riscrivono: Save diventa Save As
	CHECK(reopenedDoc.Open("res/models/pawn.obj", reopened, reopenedAssets, reopenedSettings) && reopenedDoc.Origin() == DocumentOrigin::Imported);
	CHECK(!reopenedDoc.CanSaveInPlace() && !reopenedDoc.Save(reopened, reopenedAssets, reopenedSettings));
	CHECK(reopenedDoc.OpenBuiltin(builtin.front(), reopened, reopenedAssets, reopenedSettings) && !reopenedDoc.CanSaveInPlace());
	std::filesystem::remove("build/check_document.json");

	// Mappe PBR (issue #15). Le fixture stanno in tests/data/pbr.
	auto materialNamed = [](const AssetLibrary& lib, const std::string& name) -> const Material* {
		for (const auto& m : lib.Materials())
			if (m.Name == name)
				return &m;
		return nullptr;
	};
	auto textureFile = [](const AssetLibrary& lib, const ObjectId& id, TextureKind kind) {
		const int i = lib.Textures().IndexOf(id);
		return i >= 0 && lib.Textures()[i].Kind == kind ? std::filesystem::path(lib.Textures()[i].Path).filename().string() : std::string();
	};

	// MTL: map_Ke con Ke nullo accende con strength 1 (anche col backslash di Windows); le mappe
	// senza fattore danno fattore 1; una mappa assente su disco non accende niente
	Scene pbrObj;
	AssetLibrary pbrObjAssets;
	CHECK(OpenFile("tests/data/pbr/pbr.obj", pbrObj, pbrObjAssets));
	const Material* lamp = materialNamed(pbrObjAssets, "lamp");
	CHECK(lamp && textureFile(pbrObjAssets, lamp->EmissiveTexture, TextureKind::Color) == "emit.png");
	CHECK(lamp && lamp->EmissiveStrenght == 1.0f && lamp->EmissiveColor == glm::vec4(1.0f));
	const Material* rough = materialNamed(pbrObjAssets, "rough");
	CHECK(rough && textureFile(pbrObjAssets, rough->RoughnessTexture, TextureKind::Data) == "rough.png");
	CHECK(rough && textureFile(pbrObjAssets, rough->MetallicTexture, TextureKind::Data) == "metal.png");
	CHECK(rough && rough->Roughness == 0.5f && rough->RefractionRoughness == 0.5f && rough->Metalness == 1.0f);
	const Material* missing = materialNamed(pbrObjAssets, "missing");
	CHECK(missing && missing->EmissiveTexture.IsNil() && missing->EmissiveStrenght == 0.0f);

	// glTF: l'ORM va in roughness e metallic come la stessa texture di dati, l'occlusione no;
	// l'emissive texture e' a colori e conta solo con un fattore non nullo
	Scene pbrGltf;
	AssetLibrary pbrGltfAssets;
	CHECK(OpenFile("tests/data/pbr/pbr.gltf", pbrGltf, pbrGltfAssets));
	const Material* orm = materialNamed(pbrGltfAssets, "orm");
	CHECK(orm && !orm->RoughnessTexture.IsNil() && orm->RoughnessTexture == orm->MetallicTexture);
	CHECK(orm && textureFile(pbrGltfAssets, orm->RoughnessTexture, TextureKind::Data) == "orm.png");
	CHECK(orm && orm->Roughness == 0.8f && orm->Metalness == 0.5f);
	const Material* glow = materialNamed(pbrGltfAssets, "glow");
	CHECK(glow && textureFile(pbrGltfAssets, glow->EmissiveTexture, TextureKind::Color) == "emit.png");
	CHECK(glow && glow->EmissiveStrenght == 1.0f && glow->EmissiveColor == glm::vec4(1.0f, 0.5f, 0.0f, 1.0f));
	const Material* dark = materialNamed(pbrGltfAssets, "dark");
	CHECK(dark && dark->EmissiveTexture.IsNil() && dark->EmissiveStrenght == 0.0f);
	CHECK(pbrGltfAssets.Textures().size() == 2); // orm e emit: niente occlusion

	// Le mappe si salvano con la scena e si rileggono; una scena di prima della #15 le ha a nil
	CHECK(SceneSerializer(pbrGltf, pbrGltfAssets).Serialize("build/check_pbr.json"));
	Scene pbrReopened;
	AssetLibrary pbrReopenedAssets;
	CHECK(SceneSerializer(pbrReopened, pbrReopenedAssets).Deserialize("build/check_pbr.json"));
	const Material* ormReopened = materialNamed(pbrReopenedAssets, "orm");
	const Material* glowReopened = materialNamed(pbrReopenedAssets, "glow");
	CHECK(ormReopened && ormReopened->RoughnessTexture == ormReopened->MetallicTexture
	      && textureFile(pbrReopenedAssets, ormReopened->RoughnessTexture, TextureKind::Data) == "orm.png");
	CHECK(glowReopened && textureFile(pbrReopenedAssets, glowReopened->EmissiveTexture, TextureKind::Color) == "emit.png");
	std::filesystem::remove("build/check_pbr.json");
	Scene oldScene;
	AssetLibrary oldAssets;
	RenderSettings oldSettings;
	Document oldDocument;
	CHECK(oldDocument.OpenBuiltin(*FindBuiltinScene(builtin, "sponza"), oldScene, oldAssets, oldSettings));
	bool noMaps = !oldAssets.Materials().empty();
	for (const auto& m : oldAssets.Materials())
		noMaps = noMaps && m.RoughnessTexture.IsNil() && m.MetallicTexture.IsNil() && m.EmissiveTexture.IsNil();
	CHECK(noMaps);

	// Tonemap: il vecchio bool "tonemap" si legge ancora (true = ACES, false = niente), e
	// "toneMapper" lo sostituisce e torna uguale
	auto readSettings = [](const char* text) { RenderSettings s; nlohmann::json::parse(text).get_to(s); return s; };
	CHECK(readSettings(R"({"tonemap": true})").ToneMap == ToneMapper::ACES);
	CHECK(readSettings(R"({"tonemap": false})").ToneMap == ToneMapper::None);
	CHECK(readSettings(R"({"tonemap": true, "toneMapper": "agx"})").ToneMap == ToneMapper::AgX);
	CHECK(readSettings(R"({"toneMapper": "bogus"})").ToneMap == ToneMapper::ACES);
	for (int i = 0; i < static_cast<int>(std::size(ToneMapperKeys)); i++) {
		RenderSettings saved;
		saved.ToneMap = static_cast<ToneMapper>(i);
		const nlohmann::json j = saved;
		CHECK(j.get<RenderSettings>().ToneMap == saved.ToneMap);
	}

	std::printf(failures ? "%d checks failed\n" : "all checks passed\n", failures);
	return failures ? 1 : 0;
}
