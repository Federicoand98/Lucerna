#include "Core/AliasTable.h"

#include <algorithm>

namespace Lucerna {

	// Vose: i pesi, scalati a media 1, si dividono in piccoli (< 1) e grandi. Ogni piccolo riempie
	// la sua casella prendendo il resto da un grande, che perde quella quantita' e puo' diventare
	// piccolo a sua volta. In double, perche' su centinaia di migliaia di triangoli gli errori di
	// arrotondamento si accumulano sui grandi.
	std::vector<AliasEntry> BuildAliasTable(const std::vector<float>& weights) {
		double total = 0.0;
		for (float w : weights)
			total += std::max(w, 0.0f);
		if (weights.empty() || total <= 0.0)
			return {};

		const size_t n = weights.size();
		std::vector<double> scaled(n);
		std::vector<uint32_t> small, large;
		for (size_t i = 0; i < n; i++) {
			scaled[i] = std::max(weights[i], 0.0f) * static_cast<double>(n) / total;
			(scaled[i] < 1.0 ? small : large).push_back(static_cast<uint32_t>(i));
		}

		std::vector<AliasEntry> table(n);
		while (!small.empty() && !large.empty()) {
			const uint32_t s = small.back();
			small.pop_back();
			const uint32_t l = large.back();
			table[s] = { static_cast<float>(scaled[s]), l };
			scaled[l] -= 1.0 - scaled[s];
			if (scaled[l] < 1.0) {
				large.pop_back();
				small.push_back(l);
			}
		}
		// quello che resta vale 1 a meno dell'arrotondamento: si tiene sempre
		for (uint32_t i : large)
			table[i] = { 1.0f, i };
		for (uint32_t i : small)
			table[i] = { 1.0f, i };
		return table;
	}
}
