#pragma once
#include <cmath>
inline void ApplyRotation(float* matrix, const float* delta) noexcept {
    float result[9];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result[row*3 + column] = 0;
            for (int k = 0; k < 3; ++k)
                result[row*3 + column] += delta[row*3+k] * matrix[k*4+column];
        }
    }
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            matrix[row*4+column] = result[row*3+column];
}
inline void ApplyYaw(float* matrix, float radians) noexcept {
    const float c = std::cos(radians), s = std::sin(radians);
    for (int column = 0; column < 3; ++column) {
        const float right = matrix[column], forward = matrix[8 + column];
        matrix[column] = c * right - s * forward;
        matrix[8 + column] = s * right + c * forward;
    }
}
