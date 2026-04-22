#pragma once

#include "common/math.hpp"
#include "game/world_types.hpp"
#include "platform/input.hpp"

namespace ml {

class WorldStreamer;

class PlayerController {
public:
    void update(const InputState& input, float dt, const WorldStreamer& world);
    CameraFrameData camera_frame_data(float aspect_ratio) const;
    Vec3 position() const;
    Vec3 eye_position() const;
    Vec3 forward() const;
    Aabb bounds() const;
    bool is_grounded() const;
    void set_body_position(Vec3 position);
    void set_view_from_forward(Vec3 direction);

private:
    bool collides_with_world(const Aabb& box, const WorldStreamer& world) const;
    Vec3 right() const;

    Vec3 body_position_ {32.0f, 46.0f, 80.0f};
    Vec3 velocity_ {};
    float yaw_ {-90.0f};
    float pitch_ {-22.0f};
    float move_speed_ {5.1f};
    float jump_speed_ {7.2f};
    float gravity_ {22.0f};
    float eye_height_ {1.62f};
    float height_ {1.8f};
    float radius_ {0.30f};
    float mouse_sensitivity_ {0.12f};
    bool grounded_ {false};
};

}
