
#include "Editor/Application.h"
#include "Core/Paths.h"

int main() {
	// I dati (fonts, textures, models, scenes) si leggono con path relativi a res, ovunque stia
	// l'eseguibile (ADR 0022): ci si sposta li' prima di tutto
	Lucerna::LaunchDirectory() = std::filesystem::current_path(); // i path delle variabili PT_* sono relativi a qui
	const std::filesystem::path resources = Lucerna::ResourceDirectory();
	std::error_code error;
	std::filesystem::current_path(resources, error);
	if (error) {
		std::cerr << "Resource folder not found: " << resources << " (set LUCERNA_RESOURCES)" << std::endl;
		return 1;
	}

	Lucerna::Application application;

	bool success = application.Initialize(1600, 900);

	if (success)
		application.RunLoop();

	application.Shutdown();

	return 0;
}
