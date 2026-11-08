#include "Core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>

namespace Lucerna {

	std::filesystem::path WindowsPicturesDirectory() {
		std::filesystem::path result;
		PWSTR path = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &path)))
			result = path;
		CoTaskMemFree(path); // va liberato anche quando la chiamata fallisce
		return result;
	}
}
#endif
