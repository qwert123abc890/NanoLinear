#include <iostream>
#include <vector>
#include <random>
#include <iomanip>
#include <cstdint>
#include <cstdlib>

// Native hardware intrinsics for x86/x64 instruction pipelining control
#include <intrin.h> 

#include "BinManager.h" 

// -------------------------------------------------------------------------
// HARDWARE INSTRUMENTATION UTILITIES
// -------------------------------------------------------------------------

// Serializing memory barrier to stop CPU out-of-order execution leakage
inline void hardware_fence() noexcept {
    _mm_lfence();
}

// Reads the hardware Time-Stamp Counter (TSC) 
inline uint64_t read_hardware_tsc() noexcept {
    unsigned int aux;
    return __rdtscp(&aux);
}

// Trick the compiler optimization pass to prevent Dead-Code Elimination
//  FIXED C4235: Completely removed __asm block to support 64-bit (x64) MSVC perfectly
inline void hardware_volatile_sink(void* ptr) noexcept {
    // We force a compiler read/write memory barrier.
    // This tells MSVC that ALL memory addresses might have been globally modified,
    // which completely stops the optimizer from deleting your allocate() calls!
    _ReadWriteBarrier();
}

// -------------------------------------------------------------------------
// BENCHMARK EXECUTION SUITE
// -------------------------------------------------------------------------
int main() {
    // Allocation of a massive 256MB virtual pool from the operating system
    const size_t POOL_SIZE = 256 * 1024 * 1024;
    BinManager custom_allocator(POOL_SIZE);

    // Iteration profile size
    const int OPERATIONS = 50000;

    // Setup deterministic random distribution sequence to keep the test 100% fair
    std::mt19937 engine(42);
    std::uniform_int_distribution<size_t> allocation_dist(32, 512);

    std::vector<size_t> requested_sizes(OPERATIONS);
    for (int i = 0; i < OPERATIONS; ++i) {
        requested_sizes[i] = allocation_dist(engine);
    }

    std::vector<void*> custom_pointers(OPERATIONS, nullptr);
    std::vector<void*> system_pointers(OPERATIONS, nullptr);

    std::cout << "========================================================\n";
    std::cout << "  X86 CORE BENCHMARK: BITMAP ALLOCATOR VS SYSTEM MALLOC \n";
    std::cout << "  Total Simulated Load Operations: " << OPERATIONS << " iterations\n";
    std::cout << "========================================================\n\n";

    // -------------------------------------------------------------------------
    // ARENA 1: CUSTOM BITMAP-DRIVEN ALLOCATOR PERFORMANCE
    // -------------------------------------------------------------------------
    uint64_t t_custom_alloc_start = 0, t_custom_alloc_end = 0;
    uint64_t t_custom_free_start = 0, t_custom_free_end = 0;

    uint64_t custom_alloc_total_ticks = 0;
    uint64_t custom_free_total_ticks = 0;

    // Execution profile: Custom Allocate Loop
    for (int i = 0; i < OPERATIONS; ++i) {
        hardware_fence();
        t_custom_alloc_start = read_hardware_tsc();

        custom_pointers[i] = custom_allocator.allocate(requested_sizes[i]);

        hardware_fence();
        t_custom_alloc_end = read_hardware_tsc();
        custom_alloc_total_ticks += (t_custom_alloc_end - t_custom_alloc_start);

        // Hardware touch to enforce physical page dirtying and prevent optimization bypass
        if (custom_pointers[i]) {
            char* data_buffer = reinterpret_cast<char*>(custom_pointers[i]);
            data_buffer[0] = 0xAA;
            data_buffer[requested_sizes[i] - 1] = 0xBB;
            hardware_volatile_sink(custom_pointers[i]);
        }
    }

    // Execution profile: Custom Free Loop
    for (int i = 0; i < OPERATIONS; ++i) {
        hardware_fence();
        t_custom_free_start = read_hardware_tsc();

        custom_allocator.deallocate(custom_pointers[i]);

        hardware_fence();
        t_custom_free_end = read_hardware_tsc();
        custom_free_total_ticks += (t_custom_free_end - t_custom_free_start);
    }

    // -------------------------------------------------------------------------
    // ARENA 2: STANDARD OPERATING SYSTEM MALLOC / FREE PERFORMANCE
    // -------------------------------------------------------------------------
    uint64_t t_system_alloc_start = 0, t_system_alloc_end = 0;
    uint64_t t_system_free_start = 0, t_system_free_end = 0;

    uint64_t system_alloc_total_ticks = 0;
    uint64_t system_free_total_ticks = 0;

    // Execution profile: System Allocate Loop
    for (int i = 0; i < OPERATIONS; ++i) {
        hardware_fence();
        t_system_alloc_start = read_hardware_tsc();

        system_pointers[i] = std::malloc(requested_sizes[i]);

        hardware_fence();
        t_system_alloc_end = read_hardware_tsc();
        system_alloc_total_ticks += (t_system_alloc_end - t_system_alloc_start);

        // Hardware touch
        if (system_pointers[i]) {
            char* data_buffer = reinterpret_cast<char*>(system_pointers[i]);
            data_buffer[0] = 0xAA;
            data_buffer[requested_sizes[i] - 1] = 0xBB;
            hardware_volatile_sink(system_pointers[i]);
        }
    }

    // Execution profile: System Free Loop
    for (int i = 0; i < OPERATIONS; ++i) {
        hardware_fence();
        t_system_free_start = read_hardware_tsc();

        std::free(system_pointers[i]);

        hardware_fence();
        t_system_free_end = read_hardware_tsc();
        system_free_total_ticks += (t_system_free_end - t_system_free_start);
    }

    // -------------------------------------------------------------------------
    // METRICS RECONCILIATION & DATA PRINT
    // -------------------------------------------------------------------------
    double avg_custom_alloc = static_cast<double>(custom_alloc_total_ticks) / OPERATIONS;
    double avg_custom_free = static_cast<double>(custom_free_total_ticks) / OPERATIONS;
    double avg_system_alloc = static_cast<double>(system_alloc_total_ticks) / OPERATIONS;
    double avg_system_free = static_cast<double>(system_free_total_ticks) / OPERATIONS;

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "--------------------------------------------------------\n";
    std::cout << " Target Metric (CPU Ticks/Op) | Custom Pool | System Heap | Winner \n";
    std::cout << "--------------------------------------------------------\n";

    std::cout << " Average Latency: Allocate    | "
        << std::setw(11) << avg_custom_alloc << " | "
        << std::setw(11) << avg_system_alloc << " | "
        << (avg_custom_alloc < avg_system_alloc ? "  CUSTOM " : "  SYSTEM") << "\n";

    std::cout << " Average Latency: Deallocate  | "
        << std::setw(11) << avg_custom_free << " | "
        << std::setw(11) << avg_system_free << " | "
        << (avg_custom_free < avg_system_free ? "  CUSTOM " : "  SYSTEM") << "\n";
    std::cout << "--------------------------------------------------------\n";

    double total_custom_lifecycle = avg_custom_alloc + avg_custom_free;
    double total_system_lifecycle = avg_system_alloc + avg_system_free;
    double performance_gain = ((total_system_lifecycle - total_custom_lifecycle) / total_system_lifecycle) * 100.0;

    std::cout << "\n CONCLUSION: Your customized infrastructure is approx "
        << performance_gain << "% faster than standard system heap runtime.\n";
    std::cout << "========================================================\n";

    return 0;
}
