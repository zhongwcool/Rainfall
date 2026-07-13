#pragma once

#include <vector>
#include <random>

struct Raindrop
{
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.5f;
    float speed = 0.0f;
    float length = 0.0f;
    float thickness = 1.0f;
    float alpha = 0.5f;
};

class RainSystem
{
public:
    void Initialize(int width, int height);
    void Update();

    void SetDensityScale(float scale);
    void SetWindScale(float scale);
    // 雨势：一次性设置速度、长度、粗细倍率（雨势小时雨更慢、更长、更细）
    void SetIntensity(float speedScale, float lengthScale, float thicknessScale);
    // dir >= 0: lean right; dir < 0: lean left
    void SetWindDirection(int dir);

    float GetAngleRad() const;

    const std::vector<Raindrop>& GetDrops() const { return drops_; }

private:
    void Respawn(Raindrop& drop, bool randomY);
    void ApplyDepth(Raindrop& drop);
    void Rebuild();

    std::vector<Raindrop> drops_;
    int width_ = 0;
    int height_ = 0;
    float densityScale_ = 1.0f;
    float windScale_ = 1.0f;
    float speedScale_ = 1.0f;
    float lengthScale_ = 1.0f;
    float thicknessScale_ = 1.0f;
    float windDirection_ = 1.0f;
    std::mt19937 rng_{ std::random_device{}() };
};
