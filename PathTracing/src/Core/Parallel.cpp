#include "Core/Parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace Lucerna {

	namespace {
		thread_local bool t_InsideParallelFor = false;

		// I worker dormono finche' non arriva un lavoro, poi si prendono gli indici da un contatore
		// atomico finche' non finiscono. Ogni lavoro ha un numero di generazione: un worker sveglio
		// lo confronta con l'ultimo che ha fatto, quindi nessuno salta un lavoro o ne fa uno due volte.
		class Pool {
		public:
			Pool() {
				const unsigned workers = std::max(1u, std::thread::hardware_concurrency()) - 1;
				for (unsigned i = 0; i < workers; i++)
					m_Workers.emplace_back([this] { Work(); });
			}

			~Pool() {
				{
					std::lock_guard lock(m_Mutex);
					m_Stop = true;
				}
				m_Wake.notify_all();
				for (std::thread& worker : m_Workers)
					worker.join();
			}

			void Run(size_t count, const std::function<void(size_t)>& fn) {
				std::lock_guard serial(m_Serial);
				{
					std::lock_guard lock(m_Mutex);
					m_Fn = &fn;
					m_Count = count;
					m_Next = 0;
					m_Busy = m_Workers.size();
					m_Generation++;
				}
				m_Wake.notify_all();
				Drain();
				std::unique_lock lock(m_Mutex);
				m_Done.wait(lock, [this] { return m_Busy == 0; });
				m_Fn = nullptr;
			}

		private:
			void Drain() {
				for (size_t i = m_Next++; i < m_Count; i = m_Next++)
					(*m_Fn)(i);
			}

			void Work() {
				t_InsideParallelFor = true;
				uint64_t done = 0;
				for (;;) {
					{
						std::unique_lock lock(m_Mutex);
						m_Wake.wait(lock, [&] { return m_Stop || m_Generation != done; });
						if (m_Stop)
							return;
						done = m_Generation;
					}
					Drain();
					std::lock_guard lock(m_Mutex);
					if (--m_Busy == 0)
						m_Done.notify_one();
				}
			}

			std::vector<std::thread> m_Workers;
			std::mutex m_Serial; // un lavoro alla volta
			std::mutex m_Mutex;
			std::condition_variable m_Wake, m_Done;
			const std::function<void(size_t)>* m_Fn = nullptr;
			size_t m_Count = 0;
			std::atomic<size_t> m_Next{ 0 };
			size_t m_Busy = 0;
			uint64_t m_Generation = 0;
			bool m_Stop = false;
		};
	}

	void ParallelFor(size_t count, const std::function<void(size_t)>& fn) {
		if (count <= 1 || t_InsideParallelFor) {
			for (size_t i = 0; i < count; i++)
				fn(i);
			return;
		}
		static Pool pool;
		t_InsideParallelFor = true;
		pool.Run(count, fn);
		t_InsideParallelFor = false;
	}
}
