#pragma once

#include <algorithm>
#include <cmath>

namespace HoshimiLocal::BreastMotion {
    struct SpringAxis {
        float angle = 0.0f;
        float velocity = 0.0f;
        float driveSpeed = 0.0f;

        void Step(float bodySpeed, float dt, float frequency, float limit) {
            // Accelerating the body kicks the spring. Constant velocity adds no kick.
            const float filteredSpeed = std::lerp(driveSpeed, bodySpeed, 1.0f - std::exp(-28.0f * dt));
            const float initialVelocity = std::clamp(velocity + driveSpeed - filteredSpeed, -15.0f, 15.0f);
            driveSpeed = filteredSpeed;

            // Integrate a damped spring over the real frame interval.
            const float decay = frequency * 0.32f;
            const float envelope = std::exp(-decay * dt);
            const float cosine = std::cos(frequency * dt);
            const float sine = std::sin(frequency * dt);
            const float offset = (initialVelocity + decay * angle) / frequency;
            const float nextAngle = envelope * (angle * cosine + offset * sine);
            const float nextVelocity = envelope * (initialVelocity * cosine - (frequency * angle + decay * offset) * sine);
            angle = std::clamp(nextAngle, -limit, limit);
            velocity = angle != nextAngle && nextVelocity * angle > 0.0f ? 0.0f : nextVelocity;
        }
    };
}
