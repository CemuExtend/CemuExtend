#include "Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalLayer.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#include <utility>

MetalLayerHandle::MetalLayerHandle(MTL::Device* device, const Vector2i& size, bool mainWindow)
{
	const auto window = g_renderer ? g_renderer->GetNativeSurfaces() :
		Host::NativeSurfaceSnapshot{};
	const auto& windowInfo = mainWindow ? window.mainSurface : window.padSurface;

    m_layer = (CA::MetalLayer*)CreateMetalLayer(windowInfo.surface, m_layerScaleX, m_layerScaleY);
    m_layer->setDevice(device);
    m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
    m_layer->setFramebufferOnly(true);
}

MetalLayerHandle::~MetalLayerHandle()
{
    if (m_layer)
        m_layer->release();
}

MetalLayerHandle::MetalLayerHandle(MetalLayerHandle&& other) noexcept
	: m_layer(std::exchange(other.m_layer, nullptr)),
	  m_layerScaleX(other.m_layerScaleX),
	  m_layerScaleY(other.m_layerScaleY),
	  m_drawable(std::exchange(other.m_drawable, nullptr))
{
}

MetalLayerHandle& MetalLayerHandle::operator=(MetalLayerHandle&& other) noexcept
{
	if (this == &other)
		return *this;
	if (m_layer)
		m_layer->release();
	m_layer = std::exchange(other.m_layer, nullptr);
	m_layerScaleX = other.m_layerScaleX;
	m_layerScaleY = other.m_layerScaleY;
	m_drawable = std::exchange(other.m_drawable, nullptr);
	return *this;
}

void MetalLayerHandle::Resize(const Vector2i& size)
{
    m_layer->setDrawableSize(CGSize{(float)size.x * m_layerScaleX, (float)size.y * m_layerScaleY});
}

bool MetalLayerHandle::AcquireDrawable()
{
    if (m_drawable)
        return true;

    m_drawable = m_layer->nextDrawable();
    if (!m_drawable)
    {
        cemuLog_log(LogType::Force, "layer {} failed to acquire next drawable", (void*)this);
        return false;
    }

    return true;
}

void MetalLayerHandle::PresentDrawable(MTL::CommandBuffer* commandBuffer)
{
    commandBuffer->presentDrawable(m_drawable);
    m_drawable = nullptr;
}
