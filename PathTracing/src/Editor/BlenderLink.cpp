#include "Editor/BlenderLink.h"
#include "Core/Paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Lucerna {

	namespace {
		double NowSeconds() {
			using namespace std::chrono;
			return duration<double>(steady_clock::now().time_since_epoch()).count();
		}

		bool IsExecutable(const std::filesystem::path& path) {
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec))
				return false;
#ifdef _WIN32
			return true;
#else
			return access(path.c_str(), X_OK) == 0;
#endif
		}

		// Le cartelle che iniziano con prefix, in ordine inverso: "Blender 4.2" prima di "Blender 3.6"
		std::vector<std::filesystem::path> VersionedDirs(const std::filesystem::path& parent, const std::string& prefix) {
			std::vector<std::filesystem::path> dirs;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(parent, ec))
				if (entry.is_directory(ec) && entry.path().filename().string().rfind(prefix, 0) == 0)
					dirs.push_back(entry.path());
			std::sort(dirs.rbegin(), dirs.rend());
			return dirs;
		}
	}

	BlenderLink::BlenderLink(std::filesystem::path manifest) : m_Manifest(std::move(manifest)) {}

	std::filesystem::path BlenderLink::DefaultManifest() {
#ifdef _WIN32
		const unsigned long pid = GetCurrentProcessId();
#else
		const long pid = static_cast<long>(getpid());
#endif
		std::error_code ec;
		return std::filesystem::temp_directory_path(ec) / ("lucerna-blender-link-" + std::to_string(pid) + ".json");
	}

	std::filesystem::path BlenderLink::ScriptPath() {
		return ResourceDirectory().parent_path() / "tools" / "blender" / "lucerna_link.py";
	}

	std::string BlenderLink::FindBlender(const std::string& preferred) {
		if (!preferred.empty())
			return IsExecutable(preferred) ? preferred : std::string();

		std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
		const char* exe = "blender.exe";
		const char separator = ';';
#else
		const char* exe = "blender";
		const char separator = ':';
#endif
		if (const char* path = std::getenv("PATH")) {
			std::string list = path;
			for (size_t start = 0; start <= list.size();) {
				const size_t end = std::min(list.find(separator, start), list.size());
				if (end > start)
					candidates.push_back(std::filesystem::path(list.substr(start, end - start)) / exe);
				start = end + 1;
			}
		}
#ifdef _WIN32
		for (const char* root : { "C:/Program Files/Blender Foundation", "C:/Program Files (x86)/Blender Foundation" })
			for (const auto& dir : VersionedDirs(root, "Blender"))
				candidates.push_back(dir / exe);
#else
		candidates.push_back("/usr/bin/blender");
		candidates.push_back("/usr/local/bin/blender");
		candidates.push_back("/snap/bin/blender");
		candidates.push_back("/var/lib/flatpak/exports/bin/org.blender.Blender");
		const std::filesystem::path home = HomeDirectory();
		candidates.push_back(home / ".local/share/flatpak/exports/bin/org.blender.Blender");
		for (const std::filesystem::path& root : { std::filesystem::path("/opt"), home / "Applications", home / "apps" })
			for (const auto& dir : VersionedDirs(root, "blender"))
				candidates.push_back(dir / exe);
#endif
		for (const auto& candidate : candidates)
			if (IsExecutable(candidate))
				return candidate.string();
		return {};
	}

	bool BlenderLink::Launch(const std::string& blender, std::string& error) {
		const std::filesystem::path script = ScriptPath();
		if (!std::filesystem::exists(script)) {
			error = "The Blender add-on is missing: " + script.string();
			return false;
		}
		std::error_code ec;
		std::filesystem::remove(m_Manifest, ec); // un manifest vecchio non e' un export di questa sessione
		const std::string manifest = m_Manifest.string();

#ifdef _WIN32
		auto quote = [](const std::string& s) { return "\"" + s + "\""; };
		const std::string command = quote(blender) + " --python " + quote(script.string()) + " -- --lucerna-manifest " + quote(manifest);
		std::wstring wide(command.begin(), command.end()); // ponytail: path solo ASCII, MultiByteToWideChar se servono gli accenti
		STARTUPINFOW startup{ sizeof(startup) };
		PROCESS_INFORMATION info{};
		if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
			error = "Could not start Blender: " + blender;
			return false;
		}
		CloseHandle(info.hThread);
		if (m_Process)
			CloseHandle(static_cast<HANDLE>(m_Process));
		m_Process = info.hProcess;
#else
		const std::string scriptPath = script.string();
		std::vector<char*> argv = { const_cast<char*>(blender.c_str()), const_cast<char*>("--python"), const_cast<char*>(scriptPath.c_str()),
		                            const_cast<char*>("--"), const_cast<char*>("--lucerna-manifest"), const_cast<char*>(manifest.c_str()), nullptr };
		pid_t pid = 0;
		if (posix_spawn(&pid, blender.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
			error = "Could not start Blender: " + blender;
			return false;
		}
		m_Pid = pid;
#endif
		std::cout << "Blender: " << blender << " (manifest " << manifest << ")" << std::endl;
		m_Running = true;
		m_State = State::Starting;
		m_ManifestTime = {};
		m_LastSync = 0;
		m_Blend.clear();
		m_Gltf.clear();
		return true;
	}

	bool BlenderLink::ProcessRunning() {
#ifdef _WIN32
		if (!m_Process)
			return false;
		DWORD code = 0;
		return GetExitCodeProcess(static_cast<HANDLE>(m_Process), &code) && code == STILL_ACTIVE;
#else
		if (m_Pid <= 0)
			return false;
		int status = 0;
		if (waitpid(m_Pid, &status, WNOHANG) == 0)
			return true;
		m_Pid = 0; // raccolto: niente processi zombie
		return false;
#endif
	}

	std::optional<BlenderLink::Sync> BlenderLink::Poll() {
		if (m_State == State::Off) {
			ProcessRunning(); // scollegato: si raccoglie solo il processo, se e' finito
			return std::nullopt;
		}
		if (m_Running && !ProcessRunning()) {
			m_Running = false;
			m_State = State::Closed;
		}

		// Si rilegge quando cambia l'mtime, e comunque ogni ManifestPollSeconds: Linux aggiorna l'mtime
		// con una granularita' di qualche millisecondo, e due export ravvicinati potrebbero averlo
		// uguale. Decide il numero del sync, non l'mtime.
		std::error_code ec;
		const auto time = std::filesystem::last_write_time(m_Manifest, ec);
		if (ec)
			return std::nullopt;
		const double now = NowSeconds();
		if (time == m_ManifestTime && now - m_ManifestRead < ManifestPollSeconds)
			return std::nullopt;
		m_ManifestTime = time;
		m_ManifestRead = now;

		nlohmann::json manifest;
		try {
			std::ifstream file(m_Manifest);
			file >> manifest;
		}
		catch (const std::exception&) {
			return std::nullopt; // illeggibile: si riprova alla prossima riscrittura
		}
		if (manifest.value("version", 0) != 1)
			return std::nullopt;

		const std::string status = manifest.value("status", "");
		if (m_Running)
			m_State = status == "exported" ? State::Linked : State::Unsaved;
		const uint64_t number = manifest.value("sync", uint64_t(0));
		if (status != "exported" || number == m_LastSync)
			return std::nullopt;

		m_LastSync = number;
		m_Blend = manifest.value("blend", "");
		m_Gltf = manifest.value("gltf", "");
		m_ExportMs = manifest.value("exportMs", 0.0f);
		m_SyncTime = NowSeconds();
		return Sync{ m_Gltf, m_Blend, number, m_ExportMs };
	}

	void BlenderLink::RequestSync() {
		std::ofstream(m_Manifest.string() + ".request") << "sync\n";
	}

	void BlenderLink::Disconnect() {
		m_State = State::Off;
		m_Running = false;
#ifdef _WIN32
		if (m_Process)
			CloseHandle(static_cast<HANDLE>(m_Process));
		m_Process = nullptr;
#endif
		// su Linux il pid resta: Poll raccoglie il processo quando Blender si chiude
	}

	double BlenderLink::SecondsSinceSync() const {
		return m_LastSync == 0 ? -1.0 : NowSeconds() - m_SyncTime;
	}

	void BlenderLink::ShowInFileManager(const std::filesystem::path& folder) {
#ifdef _WIN32
		ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		const std::string path = folder.string();
		char* argv[] = { const_cast<char*>("xdg-open"), const_cast<char*>(path.c_str()), nullptr };
		pid_t pid = 0;
		if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, argv, environ) == 0)
			waitpid(pid, nullptr, WNOHANG);
#endif
	}
}
