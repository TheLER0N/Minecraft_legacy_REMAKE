#include "game/debug_camera.hpp"

namespace ml {

void DebugCamera::update(const InputState& input, float dt) {
    if (input.capture_mouse) {
        yaw_ += input.mouse_delta.x * mouse_sensitivity_;
        pitch_ -= input.mouse_delta.y * mouse_sensitivity_;
        pitch_ = clamp(pitch_, -89.0f, 89.0f);
    }

    float speed = move_speed_;
    if (input.pressed(Key::Fast)) {
        speed *= fast_multiplier_;
    }

    const Vec3 fwd = forward();
    const Vec3 right_vec = right();
    const Vec3 up {0.0f, 1.0f, 0.0f};

    if (input.pressed(Key::Forward)) {
        position_ += fwd * (speed * dt);
    }
    if (input.pressed(Key::Backward)) {
        position_ -= fwd * (speed * dt);
    }
    if (input.pressed(Key::Right)) {
        position_ += right_vec * (speed * dt);
    }
    if (input.pressed(Key::Left)) {
        position_ -= right_vec * (speed * dt);
    }
    if (input.pressed(Key::Up)) {
        position_ += up * (speed * dt);
    }
    if (input.pressed(Key::Down)) {
        position_ -= up * (speed * dt);
    }
}

CameraFrameData DebugCamera::frame_data(float aspect_ratio) const {
    CameraFrameData frame {};
    const Vec3 fwd = forward();
    const Vec3 right_vec = right();
    const Vec3 up_vec = normalize(cross(right_vec, fwd));
    const Vec3 target = position_ + fwd;
    const Mat4 view = look_at(position_, target, {0.0f, 1.0f, 0.0f});
    Mat4 projection = perspective(radians(70.0f), aspect_ratio, 0.1f, 1000.0f);
    projection.m[5] *= -1.0f;
    frame.view_proj = multiply(projection, view);
    frame.camera_position = position_;
    frame.camera_forward = fwd;
    frame.camera_right = right_vec;
    frame.camera_up = up_vec;
    return frame;
}

Vec3 DebugCamera::position() const {
    return position_;
}

void DebugCamera::set_pose(Vec3 position, float yaw_degrees, float pitch_degrees) {
    position_ = position;
    yaw_ = yaw_degrees;
    pitch_ = pitch_degrees;
}

void DebugCamera::set_view_from_forward(Vec3 direction) {
    const Vec3 normalized = normalize(direction);
    if (length(normalized) <= 0.00001f) {
        return;
    }
    yaw_ = std::atan2(normalized.z, normalized.x) * (180.0f / kPi);
    pitch_ = std::asin(clamp(normalized.y, -1.0f, 1.0f)) * (180.0f / kPi);
}

Vec3 DebugCamera::forward() const {
    const float yaw_rad = radians(yaw_);
    const float pitch_rad = radians(pitch_);
    return normalize({
        std::cos(yaw_rad) * std::cos(pitch_rad),
        std::sin(pitch_rad),
        std::sin(yaw_rad) * std::cos(pitch_rad)
    });
}

Vec3 DebugCamera::right() const {
    return normalize(cross(forward(), {0.0f, 1.0f, 0.0f}));
}

}
