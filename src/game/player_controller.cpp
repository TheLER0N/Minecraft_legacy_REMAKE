#include "game/player_controller.hpp"

#include "game/world_streamer.hpp"

#include <cmath>

namespace ml {

namespace {

constexpr float kCollisionSkin = 0.001f;

}

void PlayerController::update(const InputState& input, float dt, const WorldStreamer& world) {
    if (input.capture_mouse) {
        yaw_ += input.mouse_delta.x * mouse_sensitivity_;
        pitch_ -= input.mouse_delta.y * mouse_sensitivity_;
        pitch_ = clamp(pitch_, -89.0f, 89.0f);
    }

    const Vec3 fwd = forward();
    const Vec3 planar_forward = normalize({fwd.x, 0.0f, fwd.z});
    const Vec3 right_vec = right();

    Vec3 move_intent {};
    if (input.pressed(Key::Forward)) {
        move_intent += planar_forward;
    }
    if (input.pressed(Key::Backward)) {
        move_intent -= planar_forward;
    }
    if (input.pressed(Key::Right)) {
        move_intent += right_vec;
    }
    if (input.pressed(Key::Left)) {
        move_intent -= right_vec;
    }

    move_intent = normalize(move_intent);
    velocity_.x = move_intent.x * move_speed_;
    velocity_.z = move_intent.z * move_speed_;

    if (grounded_ && input.pressed(Key::Up)) {
        velocity_.y = jump_speed_;
        grounded_ = false;
    }

    velocity_.y -= gravity_ * dt;

    grounded_ = false;

    body_position_.x += velocity_.x * dt;
    if (collides_with_world(bounds(), world)) {
        body_position_.x -= velocity_.x * dt;
        velocity_.x = 0.0f;
    }

    body_position_.z += velocity_.z * dt;
    if (collides_with_world(bounds(), world)) {
        body_position_.z -= velocity_.z * dt;
        velocity_.z = 0.0f;
    }

    body_position_.y += velocity_.y * dt;
    if (collides_with_world(bounds(), world)) {
        body_position_.y -= velocity_.y * dt;
        if (velocity_.y < 0.0f) {
            grounded_ = true;
        }
        velocity_.y = 0.0f;
    }
}

CameraFrameData PlayerController::camera_frame_data(float aspect_ratio) const {
    CameraFrameData frame {};
    const Vec3 eye = eye_position();
    const Vec3 fwd = forward();
    const Vec3 right_vec = right();
    const Vec3 up_vec = normalize(cross(right_vec, fwd));
    const Mat4 view = look_at(eye, eye + fwd, {0.0f, 1.0f, 0.0f});
    Mat4 projection = perspective(radians(70.0f), aspect_ratio, 0.1f, 1000.0f);
    projection.m[5] *= -1.0f;
    frame.view_proj = multiply(projection, view);
    frame.camera_position = eye;
    frame.camera_forward = fwd;
    frame.camera_right = right_vec;
    frame.camera_up = up_vec;
    return frame;
}

Vec3 PlayerController::position() const {
    return body_position_;
}

Vec3 PlayerController::eye_position() const {
    return {
        body_position_.x,
        body_position_.y + eye_height_,
        body_position_.z
    };
}

Vec3 PlayerController::forward() const {
    const float yaw_rad = radians(yaw_);
    const float pitch_rad = radians(pitch_);
    return normalize({
        std::cos(yaw_rad) * std::cos(pitch_rad),
        std::sin(pitch_rad),
        std::sin(yaw_rad) * std::cos(pitch_rad)
    });
}

Aabb PlayerController::bounds() const {
    return {
        {body_position_.x - radius_ + kCollisionSkin, body_position_.y + kCollisionSkin, body_position_.z - radius_ + kCollisionSkin},
        {body_position_.x + radius_ - kCollisionSkin, body_position_.y + height_ - kCollisionSkin, body_position_.z + radius_ - kCollisionSkin}
    };
}

bool PlayerController::is_grounded() const {
    return grounded_;
}

void PlayerController::set_body_position(Vec3 position) {
    body_position_ = position;
    velocity_ = {};
}

void PlayerController::set_view_from_forward(Vec3 direction) {
    const Vec3 normalized = normalize(direction);
    if (length(normalized) <= 0.00001f) {
        return;
    }
    yaw_ = std::atan2(normalized.z, normalized.x) * (180.0f / kPi);
    pitch_ = std::asin(clamp(normalized.y, -1.0f, 1.0f)) * (180.0f / kPi);
}

bool PlayerController::collides_with_world(const Aabb& box, const WorldStreamer& world) const {
    const int min_x = static_cast<int>(std::floor(box.min.x));
    const int min_y = static_cast<int>(std::floor(box.min.y));
    const int min_z = static_cast<int>(std::floor(box.min.z));
    const int max_x = static_cast<int>(std::floor(box.max.x));
    const int max_y = static_cast<int>(std::floor(box.max.y));
    const int max_z = static_cast<int>(std::floor(box.max.z));

    for (int y = min_y; y <= max_y; ++y) {
        for (int z = min_z; z <= max_z; ++z) {
            for (int x = min_x; x <= max_x; ++x) {
                if (!world.is_solid_at_world(x, y, z)) {
                    continue;
                }
                return true;
            }
        }
    }
    return false;
}

Vec3 PlayerController::right() const {
    return normalize(cross(forward(), {0.0f, 1.0f, 0.0f}));
}

}
