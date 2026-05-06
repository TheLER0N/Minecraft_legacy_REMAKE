#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>

namespace ml {

struct WorldRuntimeTuning {
    std::size_t worker_count {2};
    std::size_t max_new_chunk_requests_per_frame {8};
    std::size_t max_completed_results_per_tick {48};
    std::size_t max_job_queue_size {512};
    std::size_t chunk_upload_byte_budget_per_frame {2ull * 1024ull * 1024ull};
    std::size_t chunk_upload_backlog_budget_per_frame {4ull * 1024ull * 1024ull};
    std::size_t chunk_upload_max_count_per_frame {2};
    float completed_result_apply_budget_ms {1.5f};
    int target_chunk_radius {16};
    float streaming_update_distance_blocks {4.0f};
    float forward_priority_weight {30.0f};
    float side_priority_weight {18.0f};
    float back_priority_penalty {1000.0f};
    int min_forward_buffer_chunks {3};
    int max_forward_buffer_chunks {8};
    int min_forward_width_chunks {3};
    int max_forward_width_chunks {7};
    float forward_buffer_pipeline_seconds {0.75f};
    float forward_buffer_safety_blocks {24.0f};
    float fast_flight_speed_threshold {25.0f};
    float very_fast_flight_speed_threshold {45.0f};
    int spawn_preload_radius {0};
    std::size_t spawn_preload_min_visible_chunks {0};
    int spawn_preload_max_frames {1200};
    std::size_t spawn_preload_requests_per_frame {20};
    std::size_t spawn_preload_upload_max_count {20};
    std::size_t streaming_backlog_requests_per_frame {8};
    std::size_t world_exit_mesh_unload_budget_per_step {128};
    int contiguous_generation_ring_window {3};
    float preload_required_fraction {0.5f};
    float world_loading_min_seconds {2.0f};
    float world_leaving_min_seconds {2.0f};
    int transition_black_frames {30};
};

inline WorldRuntimeTuning world_runtime_tuning() {
    const std::size_t hardware_threads =
        std::max<std::size_t>(2, std::thread::hardware_concurrency());

#ifdef __ANDROID__
    const std::size_t worker_count = std::clamp<std::size_t>(
        hardware_threads > 1 ? hardware_threads - 1 : 1,
        std::size_t {1},
        std::size_t {2}
    );

    return WorldRuntimeTuning {
        worker_count,
        std::size_t {2},
        std::size_t {10},
        std::size_t {192},
        512ull * 1024ull,
        1024ull * 1024ull,
        std::size_t {2},
        1.0f,
        8,
        4.0f,
        24.0f,
        18.0f,
        1000.0f,
        3,
        5,
        3,
        5,
        0.75f,
        20.0f,
        18.0f,
        35.0f,
        0,
        std::size_t {0},
        900,
        std::size_t {8},
        std::size_t {8},
        std::size_t {4},
        std::size_t {64},
        2,
        0.5f,
        2.0f,
        2.0f,
        30
    };
#else
    const std::size_t reserved_threads = hardware_threads >= 8 ? 2 : 1;
    const std::size_t available_workers =
        hardware_threads > reserved_threads ? hardware_threads - reserved_threads : 2;

    const std::size_t worker_count = std::clamp<std::size_t>(
        available_workers,
        std::size_t {2},
        std::size_t {12}
    );

    return WorldRuntimeTuning {
        worker_count,
        std::clamp<std::size_t>(worker_count * 3, std::size_t {8}, std::size_t {24}),
        std::clamp<std::size_t>(worker_count * 12, std::size_t {48}, std::size_t {144}),
        std::clamp<std::size_t>(worker_count * 96, std::size_t {384}, std::size_t {1536}),
        3ull * 1024ull * 1024ull,
        6ull * 1024ull * 1024ull,
        std::clamp<std::size_t>(worker_count / 2, std::size_t {2}, std::size_t {6}),
        1.5f,
        16,
        4.0f,
        30.0f,
        18.0f,
        1000.0f,
        3,
        8,
        3,
        7,
        0.75f,
        24.0f,
        25.0f,
        45.0f,
        0,
        std::size_t {0},
        1200,
        std::size_t {20},
        std::size_t {20},
        std::size_t {8},
        std::size_t {128},
        3,
        0.5f,
        2.0f,
        2.0f,
        30
    };
#endif
}

} // namespace ml