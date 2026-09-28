#pragma once

#include <cstdint>

namespace Walnut {

	class Layer
	{
	public:
		virtual ~Layer() = default;

		virtual void OnAttach() {}
		virtual void OnDetach() {}

		virtual void OnUpdate(float ts) {}
		virtual void OnUIRender() {}

		// Optional pre-dock hook: called inside the existing dock host after
		// Begin and before DockSpace submission, with the host dockspace ID
		// as an opaque integer (no ImGui types leak into Layer.h). Layers
		// reserve toolbar space or apply pending layouts here; the default
		// is a no-op so existing layers are unaffected.
		virtual void OnDockspaceUI(uint32_t dockspaceId) { (void)dockspaceId; }
	};

}