#pragma once

#include <QuartzCore/QuartzCore.hpp>

#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "util/math/vector2.h"

class MetalLayerHandle
{
public:
    MetalLayerHandle() = default;
    MetalLayerHandle(MTL::Device* device, const Vector2i& size, bool mainWindow);
	MetalLayerHandle(const MetalLayerHandle&) = delete;
	MetalLayerHandle& operator=(const MetalLayerHandle&) = delete;
	MetalLayerHandle(MetalLayerHandle&& other) noexcept;
	MetalLayerHandle& operator=(MetalLayerHandle&& other) noexcept;

    ~MetalLayerHandle();

    void Resize(const Vector2i& size);

    bool AcquireDrawable();

    void PresentDrawable(MTL::CommandBuffer* commandBuffer);

    CA::MetalLayer* GetLayer() const { return m_layer; }

    CA::MetalDrawable* GetDrawable() const { return m_drawable; }

private:
    CA::MetalLayer* m_layer = nullptr;
    float m_layerScaleX{1.0f}, m_layerScaleY{1.0f};

    CA::MetalDrawable* m_drawable = nullptr;
};
