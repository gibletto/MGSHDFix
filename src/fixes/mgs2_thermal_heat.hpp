#pragma once

// MGS2: with the thermal goggles on, the PS2 draws IR-reacting objects untextured and clamps the
// lit colour per vertex, so a body reads as one heat colour. The port keeps the diffuse map and
// never clamps, so heads and hair go cold and bodies band. Floors get the PS2's texel density too.
namespace MGS2ThermalHeat
{
    inline bool bEnabled = true;
    void Initialize();
    void OnDeviceReady();
}
