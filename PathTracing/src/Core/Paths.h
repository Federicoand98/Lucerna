#pragma once

#ifndef CORE_PATHS_H
#define CORE_PATHS_H

#include <filesystem>
#include <string>

namespace Lucerna {

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
