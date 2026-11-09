#include "Import/Model.h"

#include <charconv>

namespace Lucerna {

	namespace {
		// Il parser lavora su una riga alla volta come vista nel buffer del file: niente
		// stringstream e niente allocazioni per token, che su Bistro (311 MB) costavano 1,6 s.
		struct Cursor {
			const char* At;
			const char* End;

			static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

			// Il prossimo token separato da spazi, vuoto a fine riga.
			std::string_view Token() {
				while (At < End && IsSpace(*At)) At++;
				const char* begin = At;
				while (At < End && !IsSpace(*At)) At++;
				return { begin, static_cast<size_t>(At - begin) };
			}

			// Come "ss >> value": un numero che manca o non si legge lascia 0.
			template <typename T>
			T Number() {
				std::string_view token = Token();
				if (!token.empty() && token.front() == '+')
					token.remove_prefix(1);
				T value{};
				std::from_chars(token.data(), token.data() + token.size(), value);
				return value;
			}
		};

		// Un intero all'inizio del testo, come std::stoi: false se non comincia con un numero.
		bool ParseIndex(std::string_view text, int& out) {
			if (!text.empty() && text.front() == '+')
				text.remove_prefix(1);
			return std::from_chars(text.data(), text.data() + text.size(), out).ec == std::errc();
		}

		// Un token di faccia OBJ puo' essere "v", "v/vt", "v//vn" o "v/vt/vn".
		// Estrae gli indici di vertice, UV e normale (0 = assente).
		// Ritorna false se il token non contiene un indice di vertice valido.
		bool ParseFaceToken(std::string_view token, int& vertexIndex, int& uvIndex, int& normalIndex) {
			vertexIndex = uvIndex = normalIndex = 0;

			const size_t firstSlash = token.find('/');
			if (!ParseIndex(token.substr(0, firstSlash), vertexIndex))
				return false;
			if (firstSlash == std::string_view::npos)
				return true;

			const size_t secondSlash = token.find('/', firstSlash + 1);
			if (secondSlash == std::string_view::npos) {
				ParseIndex(token.substr(firstSlash + 1), uvIndex); // forma "v/vt"
				return true;
			}

			// fra le due barre c'e' la UV; se vuoto ("v//vn") resta 0
			ParseIndex(token.substr(firstSlash + 1, secondSlash - firstSlash - 1), uvIndex);
			ParseIndex(token.substr(secondSlash + 1), normalIndex);
			return true;
		}
	}

	void Model::LoadObj(const char* filePath) {
		m_Name = std::filesystem::path(filePath).filename().string();
		m_Path = filePath;

		std::cout << "Loading model: " << filePath << std::endl;

		if (!std::filesystem::exists(filePath)) {
			std::cerr << "File does not exist: " << filePath << std::endl;
			return;
		}

		std::ifstream file(filePath, std::ios::binary);
		if (!file.is_open()) {
			std::cout << "Error while opening obj file..." << std::endl;
			return;
		}
		std::string content(std::filesystem::file_size(filePath), '\0');
		file.read(content.data(), static_cast<std::streamsize>(content.size()));
		content.resize(static_cast<size_t>(file.gcount()));

		int currentMaterial = -1; // materiale attivo dall'ultimo "usemtl"
		std::vector<int> faceVertices, faceUVs;

		const char* at = content.data();
		const char* const end = at + content.size();
		while (at < end) {
			const char* lineEnd = static_cast<const char*>(memchr(at, '\n', static_cast<size_t>(end - at)));
			if (lineEnd == nullptr)
				lineEnd = end;
			Cursor line{ at, lineEnd };
			at = lineEnd + 1;

			const std::string_view prefix = line.Token();
			if (prefix == "v") {
				glm::vec3 v;
				v.x = line.Number<float>();
				v.y = line.Number<float>();
				v.z = line.Number<float>();
				m_Vertices.push_back(v);
			}
			else if (prefix == "vt") {
				glm::vec2 uv;
				uv.x = line.Number<float>();
				uv.y = line.Number<float>();
				m_UVs.push_back(uv);
			}
			else if (prefix == "f") {
				// Le facce OBJ possono essere poligoni (quad, ngon), non solo triangoli:
				// le triangoliamo a ventaglio attorno al primo vertice.
				faceVertices.clear();
				faceUVs.clear();
				for (std::string_view token = line.Token(); !token.empty(); token = line.Token()) {
					int v = 0, vt = 0, vn = 0;
					if (!ParseFaceToken(token, v, vt, vn))
						continue;

					// gli indici negativi sono relativi alla fine della lista corrente
					if (v < 0) v = static_cast<int>(m_Vertices.size()) + v + 1;
					if (vt < 0) vt = static_cast<int>(m_UVs.size()) + vt + 1;

					faceVertices.push_back(v);
					faceUVs.push_back(vt);
				}

				for (size_t i = 1; i + 1 < faceVertices.size(); i++) {
					Face face;
					face.vertex_ins[0] = faceVertices[0];
					face.vertex_ins[1] = faceVertices[i];
					face.vertex_ins[2] = faceVertices[i + 1];
					face.uv_ins[0] = faceUVs[0];
					face.uv_ins[1] = faceUVs[i];
					face.uv_ins[2] = faceUVs[i + 1];
					face.material = currentMaterial;
					m_Faces.push_back(face);
					m_TriangleCount++;
				}
			}
			else if (prefix == "usemtl") {
				const std::string_view matName = line.Token();
				currentMaterial = -1;
				for (size_t i = 0; i < m_Materials.size(); i++)
					if (m_Materials[i].Name == matName) { currentMaterial = (int)i; break; }
			}
			else if (prefix == "mtllib") {
				// il .mtl e' relativo alla cartella dell'OBJ
				const std::string_view mtlName = line.Token();
				if (!mtlName.empty())
					ParseMtl((std::filesystem::path(filePath).parent_path() / mtlName).lexically_normal().string());
			}
		}

		MakeTriangles();
		CalculateBoundingBox();
		m_Vertices = {};
		m_UVs = {};
		m_Faces = {};
	}

	void Model::MakeTriangles() {
		// Smooth shading (come "Shade Smooth" di Blender): invece di usare le normali
		// per-faccia dell'OBJ (che danno lo shading sfaccettato), calcolo una normale
		// per-vertice mediando le normali geometriche di tutte le facce adiacenti.
		// Il cross product NON normalizzato pesa ogni contributo per l'area del triangolo.
		std::vector<glm::vec3> vertexNormals(m_Vertices.size(), glm::vec3(0.0f));

		// scarta le facce che referenziano vertici fuori range (OBJ malformato)
		auto validFace = [this](const Face& f) {
			for (int i = 0; i < 3; i++) {
				int idx = f.vertex_ins[i] - 1;
				if (idx < 0 || idx >= static_cast<int>(m_Vertices.size()))
					return false;
			}
			return true;
		};

		size_t skipped = 0;
		for (const Face& face : m_Faces) {
			if (!validFace(face)) {
				skipped++;
				continue;
			}

			const glm::vec3& a = m_Vertices[face.vertex_ins[0] - 1];
			const glm::vec3& b = m_Vertices[face.vertex_ins[1] - 1];
			const glm::vec3& c = m_Vertices[face.vertex_ins[2] - 1];

			glm::vec3 faceNormal = glm::cross(b - a, c - a); // peso implicito per area

			vertexNormals[face.vertex_ins[0] - 1] += faceNormal;
			vertexNormals[face.vertex_ins[1] - 1] += faceNormal;
			vertexNormals[face.vertex_ins[2] - 1] += faceNormal;
		}

		if (skipped > 0)
			std::cerr << "Warning: skipped " << skipped << " faces with out-of-range vertex indices" << std::endl;

		for (glm::vec3& n : vertexNormals) {
			if (glm::length(n) > 1e-8f)
				n = glm::normalize(n);
		}

		// UV assenti (indice 0) o fuori range -> (0,0): il modello restera' a tinta unita
		auto uvAt = [this](int index) {
			int i = index - 1;
			return (i >= 0 && i < (int)m_UVs.size()) ? m_UVs[i] : glm::vec2(0.0f);
		};

		m_Triangles.reserve(m_Faces.size() - skipped);
		for (const Face& face : m_Faces) {
			if (!validFace(face))
				continue;

			Triangle& triangle = m_Triangles.emplace_back();
			triangle.A = glm::vec4(m_Vertices[face.vertex_ins[0] - 1], 0.0f);
			triangle.B = glm::vec4(m_Vertices[face.vertex_ins[1] - 1], 0.0f);
			triangle.C = glm::vec4(m_Vertices[face.vertex_ins[2] - 1], 0.0f);

			triangle.NormalA = glm::vec4(vertexNormals[face.vertex_ins[0] - 1], 0.0f);
			triangle.NormalB = glm::vec4(vertexNormals[face.vertex_ins[1] - 1], 0.0f);
			triangle.NormalC = glm::vec4(vertexNormals[face.vertex_ins[2] - 1], 0.0f);

			triangle.UVA = uvAt(face.uv_ins[0]);
			triangle.UVB = uvAt(face.uv_ins[1]);
			triangle.UVC = uvAt(face.uv_ins[2]);

			// indice LOCALE nel MTL del modello: diventa lo slot del materiale. -1 resta -1
			// (nessun usemtl) e a valle diventa il fallback al materiale della mesh.
			triangle.MaterialSlot = (float)face.material;
		}
	}

	// Parser MTL: legge SOLO i campi che il nostro Material sa rappresentare (vedi MtlMaterial).
	// I path delle texture sono risolti rispetto alla cartella del .mtl. Ka, illum, ecc.
	// sono deliberatamente ignorati (non hanno un equivalente, per ora). map_Bump, bump e
	// map_Disp si leggono tutti come normal map in spazio tangente: e' l'uso che ne fanno gli
	// asset reali (Sponza mette i suoi _ddn su map_Disp), le height map vere non le gestiamo.
	// Il campo di MtlMaterial in cui va una mappa, nullptr per le mappe che non leggiamo
	// (map_Ka, map_Ks...: issue #15 le lascia fuori, le _spec di Bistro non hanno un significato
	// PBR certo). map_Bump, bump, map_Disp e disp vanno tutte nella normal map.
	static std::string* MapTarget(MtlMaterial& m, const std::string& prefix) {
		if (prefix == "map_Kd") return &m.map_Kd;
		if (prefix == "map_d") return &m.map_d;
		if (prefix == "map_Pr") return &m.map_Pr;
		if (prefix == "map_Pm") return &m.map_Pm;
		if (prefix == "map_Ke") return &m.map_Ke;
		if (prefix == "map_Bump" || prefix == "map_bump" || prefix == "bump" || prefix == "map_Disp" || prefix == "disp")
			return &m.map_Bump;
		return nullptr;
	}

	void Model::ParseMtl(const std::string& mtlPath) {
		std::ifstream file(mtlPath);
		if (!file.is_open()) {
			std::cerr << "MTL non trovato: " << mtlPath << " (le mesh useranno il materiale della mesh)" << std::endl;
			return;
		}

		const std::filesystem::path mtlDir = std::filesystem::path(mtlPath).parent_path();
		std::string line, prefix;
		std::stringstream ss;

		while (std::getline(file, line)) {
			ss.clear();
			ss.str(line);
			ss >> prefix;

			if (prefix == "newmtl") {
				MtlMaterial m;
				ss >> m.Name;
				m_Materials.push_back(m);
			}
			else if (m_Materials.empty()) {
				continue; // proprieta' prima di qualsiasi newmtl: ignora
			}
			else {
				MtlMaterial& m = m_Materials.back();
				if (prefix == "Kd") ss >> m.Kd.x >> m.Kd.y >> m.Kd.z;
				else if (prefix == "Ke") ss >> m.Ke.x >> m.Ke.y >> m.Ke.z;
				else if (prefix == "Ns") ss >> m.Ns;
				else if (prefix == "Ni") ss >> m.Ni;
				else if (prefix == "d") ss >> m.d;
				else if (prefix == "Tr") { float tr; ss >> tr; m.d = 1.0f - tr; } // Tr = 1 - d
				else if (prefix == "Pr") ss >> m.Pr;
				else if (prefix == "Pm") ss >> m.Pm;
				else if (std::string* target = MapTarget(m, prefix)) {
					// le map possono avere opzioni (-o, -s, -bm, ...): il filename e' l'ULTIMO token
					std::string token, last;
					while (ss >> token) last = token;
					if (last.empty())
						continue;
					// MTL esportati su Windows (Bistro) scrivono "..\Textures\x.png": su Linux il
					// backslash non separa le cartelle, quindi si normalizza sempre a "/"
					std::replace(last.begin(), last.end(), '\\', '/');
					// se un materiale ne ha piu' d'uno, vince il primo
					if (prefix == "map_Kd" || target->empty())
						*target = (mtlDir / last).lexically_normal().string();
				}
			}
		}

		std::cout << "MTL caricato: " << mtlPath << " (" << m_Materials.size() << " materiali)" << std::endl;
	}

	void Model::CalculateBoundingBox() {
		m_BoundsMin = glm::vec4(std::numeric_limits<float>::max());
		m_BoundsMax = glm::vec4(std::numeric_limits<float>::lowest());

		for (const Triangle& triangle : m_Triangles) {
			m_BoundsMin = glm::min(m_BoundsMin, glm::min(triangle.A, glm::min(triangle.B, triangle.C)));
			m_BoundsMax = glm::max(m_BoundsMax, glm::max(triangle.A, glm::max(triangle.B, triangle.C)));
		}
	}
}
