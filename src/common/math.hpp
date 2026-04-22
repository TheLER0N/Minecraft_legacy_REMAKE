#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

namespace ml {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 {
    float x {0.0f};
    float y {0.0f};
};

struct Vec3 {
    float x {0.0f};
    float y {0.0f};
    float z {0.0f};

    Vec3& operator+=(const Vec3& rhs) {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    Vec3& operator-=(const Vec3& rhs) {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }
};

inline Vec3 operator+(Vec3 lhs, const Vec3& rhs) {
    lhs += rhs;
    return lhs;
}

inline Vec3 operator-(Vec3 lhs, const Vec3& rhs) {
    lhs -= rhs;
    return lhs;
}

inline Vec3 operator*(const Vec3& lhs, float scalar) {
    return {lhs.x * scalar, lhs.y * scalar, lhs.z * scalar};
}

inline Vec3 operator/(const Vec3& lhs, float scalar) {
    return {lhs.x / scalar, lhs.y / scalar, lhs.z / scalar};
}

inline float dot(const Vec3& lhs, const Vec3& rhs) {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline Vec3 cross(const Vec3& lhs, const Vec3& rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

inline float length(const Vec3& value) {
    return std::sqrt(dot(value, value));
}

inline Vec3 normalize(const Vec3& value) {
    const float len = length(value);
    if (len <= 0.00001f) {
        return {};
    }
    return value / len;
}

inline float radians(float degrees) {
    return degrees * (kPi / 180.0f);
}

inline float clamp(float value, float min_value, float max_value) {
    return std::max(min_value, std::min(value, max_value));
}

struct Mat4 {
    std::array<float, 16> m {};

    // Column-major matrix layout matching GLSL mat4 memory layout.
    // Element access is m[column * 4 + row].
    static Mat4 identity() {
        Mat4 result {};
        result.m = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        return result;
    }
};

inline Mat4 multiply(const Mat4& lhs, const Mat4& rhs) {
    Mat4 result {};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i) {
                sum += lhs.m[i * 4 + row] * rhs.m[col * 4 + i];
            }
            result.m[col * 4 + row] = sum;
        }
    }
    return result;
}

inline Mat4 perspective(float fov_y_radians, float aspect_ratio, float near_plane, float far_plane) {
    const float tan_half = std::tan(fov_y_radians * 0.5f);

    Mat4 result {};
    result.m = {
        1.0f / (aspect_ratio * tan_half), 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f / tan_half, 0.0f, 0.0f,
        0.0f, 0.0f, far_plane / (near_plane - far_plane), -1.0f,
        0.0f, 0.0f, (near_plane * far_plane) / (near_plane - far_plane), 0.0f
    };
    return result;
}

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 result = Mat4::identity();
    result.m = {
         s.x,  u.x, -f.x, 0.0f,
         s.y,  u.y, -f.y, 0.0f,
         s.z,  u.z, -f.z, 0.0f,
        -dot(s, eye), -dot(u, eye), dot(f, eye), 1.0f
    };
    return result;
}

template <typename T>
inline std::size_t hash_combine(std::size_t seed, const T& value) {
    return seed ^ (std::hash<T> {}(value) + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
}

}
