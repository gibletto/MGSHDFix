#pragma once

struct ID3D11DeviceContext;

// Diagnostic: logs the doorjamb draw's clip-space vertex each frame so we can see the
// dolly-zoom jitter and tell whether it's far-from-origin float precision or matrix math.
namespace MGS2DoorjamProbe
{
    void Initialize(ID3D11DeviceContext* context);
}
