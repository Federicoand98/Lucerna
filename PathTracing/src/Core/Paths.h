#pragma once

#ifndef CORE_PATHS_H
#define CORE_PATHS_H

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace Lucerna {

	// ---- dove stanno le cose (ADR 0022)

	// I dati letti a runtime (fonts, textures, models, scenes): la cartella res del sorgente, il
	// cui path lo scrive CMake. LUCERNA_RESOURCES lo sovrascrive. L'app ci fa chdir all'avvio,
	// quindi i path dei dati restano relativi a lei.
	inline std::filesystem::path ResourceDirectory() {
		if (const char* dir = std::getenv("LUCERNA_RESOURCES"))
			return dir;
#ifdef LUCERNA_RESOURCE_DIR
		return LUCERNA_RESOURCE_DIR;
#else
		return std::filesystem::current_path();
#endif
	}

	// Uno shader compilato (SPIR-V): output del build, in build/shaders. LUCERNA_SHADERS lo sovrascrive.
	inline std::string ShaderPath(const std::string& name) {
		std::filesystem::path dir = "shaders";
#ifdef LUCERNA_SHADER_DIR
		dir = LUCERNA_SHADER_DIR;
#endif
		if (const char* override = std::getenv("LUCERNA_SHADERS"))
			dir = override;
		return (dir / name).string();
	}

	// La cartella da cui l'app e' stata lanciata: Main la ricorda prima di spostarsi nei dati
	inline std::filesystem::path& LaunchDirectory() {
		static std::filesystem::path directory;
		return directory;
	}

	// Un path passato da riga di comando (le variabili PT_*): relativo alla cartella di lancio,
	// come ci si aspetta da una shell. Un file da leggere che li' non c'e' resta relativo ai dati,
	// come prima della riorganizzazione (PT_GLTF=models/...).
	inline std::string FromLaunchDirectory(const std::string& path, bool input) {
		const std::filesystem::path p(path);
		if (p.is_absolute() || LaunchDirectory().empty())
			return path;
		const std::filesystem::path launched = LaunchDirectory() / p;
		if (input && !std::filesystem::exists(launched))
			return path;
		return launched.string();
	}

	inline std::filesystem::path HomeDirectory() {
#ifdef _WIN32
		// Prima USERPROFILE: HOME su Windows lo mette solo Git Bash/MSYS, e non sempre uguale
		if (const char* profile = std::getenv("USERPROFILE"))
			return profile;
#endif
		if (const char* home = std::getenv("HOME"))
			return home;
		if (const char* profile = std::getenv("USERPROFILE"))
			return profile;
		return std::filesystem::current_path();
	}

#ifdef _WIN32
	// La cartella Immagini di Windows, anche se spostata (OneDrive): Paths.cpp, per non portare
	// windows.h in ogni file che include questo header. Vuota se Windows non la conosce.
	std::filesystem::path WindowsPicturesDirectory();
#endif

	// Lo stato dell'utente (layout delle finestre, preferenze): ~/.config/lucerna, o
	// %APPDATA%\Lucerna su Windows. Non sta ne' nel repo ne' accanto ai dati.
	inline std::filesystem::path UserConfigDirectory() {
#ifdef _WIN32
		if (const char* appData = std::getenv("APPDATA"))
			return std::filesystem::path(appData) / "Lucerna";
#endif
		if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
			return std::filesystem::path(config) / "lucerna";
		return HomeDirectory() / ".config" / "lucerna";
	}

	// Gli screenshot: la cartella Immagini dell'utente, piu' Lucerna. Su Linux il suo nome
	// dipende dalla lingua ("Immagini"): lo dice ~/.config/user-dirs.dirs. Su Windows la chiede
	// alla shell.
	inline std::filesystem::path UserPicturesDirectory() {
		std::filesystem::path pictures = HomeDirectory() / "Pictures";
#ifdef _WIN32
		if (std::filesystem::path known = WindowsPicturesDirectory(); !known.empty())
			pictures = known;
#else
		const char* config = std::getenv("XDG_CONFIG_HOME");
		std::ifstream dirs((config && *config ? std::filesystem::path(config) : HomeDirectory() / ".config") / "user-dirs.dirs");
		for (std::string line; std::getline(dirs, line);) {
			const std::string key = "XDG_PICTURES_DIR=\"";
			if (line.rfind(key, 0) != 0 || line.back() != '"')
				continue;
			std::string value = line.substr(key.size(), line.size() - key.size() - 1);
			if (value.rfind("$HOME", 0) == 0)
				value = HomeDirectory().string() + value.substr(5);
			pictures = value;
		}
#endif
		return pictures / "Lucerna";
	}

	// Un path di file nella forma con cui l'app lo ricorda: relativo alla cartella di lavoro (res/)
	// se ci sta dentro, altrimenti assoluto; sempre normalizzato e con "/". Cosi' lo stesso file
	// scelto da un dialog (assoluto) o scritto in una scena (relativo) e' la stessa stringa.
	inline std::string CanonicalPath(const std::filesystem::path& path) {
		namespace fs = std::filesystem;
		const fs::path absolute = fs::absolute(path).lexically_normal();
		const fs::path relative = absolute.lexically_relative(fs::current_path());
		const bool inside = !relative.empty() && *relative.begin() != "..";
		return (inside ? relative : absolute).generic_string();
	}

	// Un file di scena scrive i path relativi alla sua cartella (formato 3), cosi' si sposta insieme
	// ai suoi modelli. Questi due li convertono, avanti e indietro.
	inline std::string RelativeToFile(const std::string& path, const std::string& file) {
		namespace fs = std::filesystem;
		const fs::path folder = fs::absolute(file).parent_path().lexically_normal();
		const fs::path relative = fs::absolute(path).lexically_normal().lexically_relative(folder);
		return relative.empty() ? CanonicalPath(path) : relative.generic_string();
	}

	inline std::string ResolveFromFile(const std::string& path, const std::string& file) {
		namespace fs = std::filesystem;
		const fs::path stored(path);
		return CanonicalPath(stored.is_absolute() ? stored : fs::absolute(file).parent_path() / stored);
	}
}

#endif // CORE_PATHS_H
