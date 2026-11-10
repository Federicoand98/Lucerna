#pragma once

#ifndef CORE_ALIASTABLE_H
#define CORE_ALIASTABLE_H

#include <cstdint>
#include <vector>

namespace Lucerna {

	// Una casella di una alias table (Vose): si sceglie una casella k uniforme, poi si tiene k con
	// probabilita' Prob, altrimenti si prende Alias. Ogni indice esce con probabilita' peso / totale,
	// con un solo numero casuale e due letture (ADR 0025).
	struct AliasEntry {
		float Prob = 1.0f;
		uint32_t Alias = 0;
	};

	// Vuota se non c'e' nessun peso positivo. I pesi negativi contano come zero.
	std::vector<AliasEntry> BuildAliasTable(const std::vector<float>& weights);
}

#endif // CORE_ALIASTABLE_H
