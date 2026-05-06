#include "common/perf_recorder.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace ml {

bool PerfRecorder::initialize_from_environment() {
    const char* path = std::getenv("MINECRAFT_LEGACY_PERF_LOG");
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    const std::filesystem::path output_path {path};
    std::error_code error;
    if (const std::filesystem::path parent = output_path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            log_message(LogLevel::Warning, "PerfRecorder: failed to create log directory: " + error.message());
            return false;
        }
    }

    output_.open(output_path, std::ios::out | std::ios::trunc);
    if (!output_) {
        log_message(LogLevel::Warning, "PerfRecorder: failed to open perf log " + output_path.string());
        return false;
    }

    frame_times_ms_.reserve(4096);
    start_time_ = std::chrono::steady_clock::now();
    write_header();
    log_message(LogLevel::Info, "PerfRecorder: writing CSV perf log to " + output_path.string());
    return true;
}

bool PerfRecorder::enabled() const {
    return output_.is_open();
}

void PerfRecorder::record_frame(const FrameSample& sample) {
    if (!enabled()) {
        return;
    }

    frame_times_ms_.push_back(sample.frame_ms);
    const auto now = std::chrono::steady_clock::now();
    const float elapsed_ms = std::chrono::duration<float, std::milli>(now - start_time_).count();
    const float p50 = percentile(frame_times_ms_, 0.50f);
    const float p95 = percentile(frame_times_ms_, 0.95f);
    const float p99 = percentile(frame_times_ms_, 0.99f);

    output_ << frame_index_++ << ','
        << elapsed_ms << ','
        << sample.frame_ms << ','
        << p50 << ','
        << p95 << ','
        << p99 << ','
        << sample.streaming.last_generate_ms << ','
        << sample.streaming.last_light_ms << ','
        << sample.streaming.last_mesh_ms << ','
        << sample.streaming.last_apply_ms << ','
        << sample.upload_ms << ','
        << sample.streaming.pending_uploads << ','
        << sample.streaming.pending_upload_bytes << ','
        << sample.streaming.pending_upload_sections << ','
        << sample.uploads_this_frame << ','
        << sample.edit_uploads_this_frame << ','
        << sample.uploaded_bytes_this_frame << ','
        << sample.uploaded_sections_this_frame << ','
        << sample.streaming.queued_generates << ','
        << sample.streaming.queued_decorates << ','
        << sample.streaming.queued_lights << ','
        << sample.streaming.queued_meshes << ','
        << sample.streaming.queued_fast_meshes << ','
        << sample.streaming.queued_final_meshes << ','
        << sample.streaming.stale_results << ','
        << sample.streaming.stale_uploads << ','
        << sample.streaming.light_stale_results << ','
        << sample.streaming.edge_fixups << ','
        << sample.streaming.deduped_uploads << ','
        << sample.streaming.deferred_rebuilds << ','
        << sample.streaming.final_light_jobs << ','
        << sample.streaming.pending_upload_unique_chunks << ','
        << sample.streaming.provisional_uploads << ','
        << sample.streaming.provisional_lifetime_frames << ','
        << sample.streaming.missing_light_borders << ','
        << sample.streaming.urgent_edit_chunks << ','
        << sample.streaming.urgent_edit_uploads << ','
        << sample.streaming.edit_upload_latency_frames << ','
        << sample.streaming.renderer_upload_failures << ','
        << sample.draw.drawn_chunks << ','
        << sample.draw.visible_sections << ','
        << sample.draw.drawn_sections << ','
        << sample.draw.frustum_culled_sections << ','
        << sample.draw.cave_culled_sections << ','
        << sample.draw.surface_culled_sections << ','
        << sample.draw.occlusion_culled_sections << ','
        << sample.draw.draw_calls << ','
        << sample.draw.drawn_vertices << ','
        << sample.draw.drawn_indices << ','
        << sample.renderer.new_gpu_buffers << ','
        << sample.renderer.pooled_gpu_buffers << '\n';
}

float PerfRecorder::percentile(std::vector<float> values, float fraction) {
    if (values.empty()) {
        return 0.0f;
    }
    std::sort(values.begin(), values.end());
    const float scaled_index = fraction * static_cast<float>(values.size() - 1);
    const auto index = static_cast<std::size_t>(scaled_index + 0.5f);
    return values[std::min(index, values.size() - 1)];
}

void PerfRecorder::write_header() {
    output_ << "frame,elapsed_ms,frame_ms,frame_p50_ms,frame_p95_ms,frame_p99_ms,"
        "generate_ms,light_ms,mesh_ms,apply_ms,upload_ms,"
        "pending_uploads,pending_upload_bytes,pending_upload_sections,"
        "uploads_this_frame,edit_uploads_this_frame,uploaded_bytes_this_frame,uploaded_sections_this_frame,"
        "queued_generates,queued_decorates,queued_lights,queued_meshes,queued_fast_meshes,queued_final_meshes,"
        "stale_results,stale_uploads,light_stale_results,edge_fixups,deduped_uploads,deferred_rebuilds,final_light_jobs,pending_upload_unique_chunks,provisional_uploads,"
        "provisional_lifetime_frames,missing_light_borders,urgent_edit_chunks,urgent_edit_uploads,edit_upload_latency_frames,renderer_upload_failures,"
        "drawn_chunks,visible_sections,drawn_sections,frustum_culled_sections,cave_culled_sections,surface_culled_sections,occlusion_culled_sections,draw_calls,drawn_vertices,drawn_indices,"
        "new_gpu_buffers,pooled_gpu_buffers\n";
}

}
