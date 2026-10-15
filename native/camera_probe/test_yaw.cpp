#include "yaw.hpp"
#include <cmath>
#include <cstdio>

int main() {
    // Nonidentity base: 90-degree roll; a local yaw must not be world yaw.
    float input[16] = {0,1,0,0, -1,0,0,0, 0,0,1,0, 4,5,6,1};
    const float expected[16] = {0,0,-1,0, -1,0,0,0, 0,1,0,0, 4,5,6,1};
    ApplyYaw(input, 1.5707963267948966f);
    for (int i = 0; i < 16; ++i) {
        if (std::abs(input[i] - expected[i]) > 0.000001f) {
            std::fprintf(stderr, "local yaw/translation mismatch at %d: %f vs %f\n", i, input[i], expected[i]);
            return 1;
        }
    }
    float second[16] = {0,1,0,0, -1,0,0,0, 0,0,1,0, 4,5,6,1};
    const float delta[9] = {0,0,-1, 0,1,0, 1,0,0};
    ApplyRotation(second, delta);
    for (int i = 0; i < 16; ++i) {
        if (std::abs(second[i] - expected[i]) > 0.000001f) return 2;
    }
    return 0;
}
