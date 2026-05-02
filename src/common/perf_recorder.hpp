#pragma once

#include "game/world_streamer.hpp"
#include "render/renderer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ml {

class PerfRecorder {
public:
    struct FrameSample {
        float frame_ms {0.0f};
        float upload_ms {0.0f};
        std::size_t uploads_this_frame {0};
        std::size_t edit_uploads_this_frame {0};
        std::size_t uploaded_bytes_this_frame {0};
        std::size_t uploaded_sections_this_frame {0};
        WorldStreamer::StreamingStats streaming {};
        Renderer::BufferStats renderer {};
        Renderer::DebugHudData draw {};
    };

    bool initialize_from_environment();
    bool enabled() const;
    void record_frame(const FrameSample& sample);

private:
    static float percentile(std::vector<float> values, float fraction);
    void write_header();

    std::ofstream output_ {};
    std::vector<float> frame_times_ms_ {};
    std::chrono::steady_clock::time_point start_time_ {};
    std::size_t frame_index_ {0};
};

}
