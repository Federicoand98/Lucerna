#pragma once

#ifndef CORE_PARALLEL_H
#define CORE_PARALLEL_H

#include <cstddef>
#include <functional>

namespace Lucerna {

	// Esegue fn(i) per ogni i in [0, count) su tutti i core e ritorna quando ha finito. Anche il
	// thread che chiama lavora. Il pool nasce alla prima chiamata e vive fino all'uscita; due
	// chiamate da thread diversi si mettono in fila, e una chiamata da dentro fn gira in sequenza
	// sul thread che la fa. fn non deve lanciare eccezioni.
	void ParallelFor(size_t count, const std::function<void(size_t)>& fn);

	// true dentro fn, su un worker o sul thread che ha chiamato ParallelFor
	bool InsideParallelFor();
}

#endif // CORE_PARALLEL_H
