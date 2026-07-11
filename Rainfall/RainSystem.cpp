#include "RainSystem.h"

#include <algorithm>
#include <cmath>

namespace
{
    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    constexpr float kWindOffset = 0.6f;
    constexpr float kAngleRad = 0.18f; // slight diagonal fall
}

void RainSystem::Initialize(int width, int height)
{
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);

    const int area = width_ * height_;
    const int dropCount = std::clamp(area / 5000, 200, 400);

    drops_.clear();
    drops_.resize(static_cast<size_t>(dropCount));

    for (auto& drop : drops_)
    {
        Respawn(drop, true);
    }
}

void RainSystem::ApplyDepth(Raindrop& drop)
{
    const float depth = drop.depth;

    drop.speed = Lerp(4.0f, 14.0f, depth);
    drop.length = Lerp(10.0f, 36.0f, depth);
    drop.thickness = Lerp(1.0f, 2.8f, depth);
    drop.alpha = Lerp(0.18f, 0.95f, depth);
}

void RainSystem::Respawn(Raindrop& drop, bool randomY)
{
    std::uniform_real_distribution<float> xDist(0.0f, static_cast<float>(width_));
    std::uniform_real_distribution<float> yDist(-static_cast<float>(height_), 0.0f);
    std::uniform_real_distribution<float> depthDist(0.0f, 1.0f);

    drop.x = xDist(rng_);
    drop.y = randomY ? yDist(rng_) : -drop.length;
    drop.depth = depthDist(rng_);
    ApplyDepth(drop);
}

void RainSystem::Update()
{
    const float dx = std::sin(kAngleRad) * kWindOffset;
    const float dy = std::cos(kAngleRad);

    for (auto& drop : drops_)
    {
        drop.x += dx * drop.speed;
        drop.y += dy * drop.speed;

        if (drop.y - drop.length > height_ || drop.x < -drop.length || drop.x > width_ + drop.length)
        {
            Respawn(drop, false);
        }
    }
}
