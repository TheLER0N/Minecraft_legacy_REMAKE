#pragma once

#include "common/math.hpp"
#include "game/world_types.hpp"
#include "platform/input.hpp"

namespace ml {

class DebugCamera {
public:
    void update(const InputState& input, float dt);
    CameraFrameData frame_data(float aspect_ratio) const;
    Vec3 position() const;
    Vec3 forward() const;
    void set_pose(Vec3 position, float yaw_degrees, float pitch_degrees);
    void set_view_from_forward(Vec3 direction);

private:
    Vec3 right() const;

    Vec3 position_ {32.0f, 56.0f, 80.0f};
    float yaw_ {-90.0f};
    float pitch_ {-22.0f};
    float move_speed_ {18.0f};
    float fast_multiplier_ {3.0f};
    float mouse_sensitivity_ {0.12f};
};

}
