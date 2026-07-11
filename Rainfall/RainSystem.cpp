#include "RainSystem.h"

#include <algorithm>
#include <cmath>

namespace
{
    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    constexpr float kBaseAngleRad = 0.18f; // slight diagonal fall at wind scale 1.0
    constexpr float kMaxAngleRad = 1.05f;  // ~60°，风力最大时的倾斜上限
}

void RainSystem::Initialize(int width, int height)
{
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    Rebuild();
}

void RainSystem::Rebuild()
{
    if (width_ <= 0 || height_ <= 0)
    {
        return;
    }

    const int area = width_ * height_;
    const int baseCount = std::clamp(area / 5000, 200, 400);
    const int dropCount = std::max(1, static_cast<int>(baseCount * densityScale_));

    drops_.clear();
    drops_.resize(static_cast<size_t>(dropCount));

    for (auto& drop : drops_)
    {
        Respawn(drop, true);
    }
}

void RainSystem::SetDensityScale(float scale)
{
    densityScale_ = scale;
    Rebuild();
}

void RainSystem::SetLengthScale(float scale)
{
    lengthScale_ = scale;
    for (auto& drop : drops_)
    {
        ApplyDepth(drop);
    }
}

void RainSystem::SetWindScale(float scale)
{
    windScale_ = scale;
}

void RainSystem::SetSpeedScale(float scale)
{
    speedScale_ = scale;
    for (auto& drop : drops_)
    {
        ApplyDepth(drop);
    }
}

float RainSystem::GetAngleRad() const
{
    return std::min(kBaseAngleRad * windScale_, kMaxAngleRad);
}

void RainSystem::ApplyDepth(Raindrop& drop)
{
    const float depth = drop.depth;

    drop.speed = Lerp(4.0f, 14.0f, depth) * speedScale_;
    drop.length = Lerp(10.0f, 36.0f, depth) * lengthScale_;
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
    const float angle = GetAngleRad();
    const float dx = std::sin(angle);
    const float dy = std::cos(angle);

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
