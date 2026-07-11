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

    const std::vector<Raindrop>& GetDrops() const { return drops_; }

private:
    void Respawn(Raindrop& drop, bool randomY);
    void ApplyDepth(Raindrop& drop);

    std::vector<Raindrop> drops_;
    int width_ = 0;
    int height_ = 0;
    std::mt19937 rng_{ std::random_device{}() };
};
