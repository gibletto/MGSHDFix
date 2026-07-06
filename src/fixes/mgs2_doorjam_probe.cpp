#include "stdafx.h"
#include "mgs2_doorjam_probe.hpp"

#include "common.hpp"
#include "logging.hpp"
#include "d3d11_api.hpp"

// The doorjamb (RenderDoc EID 991: DrawIndexed, IndexCount=21, StartIndexLocation=5493) is drawn
// with a float4 CLIP-SPACE position and no VS matrix - i.e. the MC pre-transforms it on the CPU.
// Its dolly-zoom jitter therefore lives in those clip coords. Read one of its vertices back each
// frame (index buffer -> vertex buffer, via a staging copy) and log clip xyzw + ndc so we can see
// the wobble and its scale.
namespace
{
    constexpr UINT kDrawIndexedVtbl = 12;          // ID3D11DeviceContext::DrawIndexed
    constexpr UINT kDoorjamIndexCount = 21;
    constexpr UINT kDoorjamStartIndex = 5493;

    SafetyHookInline gDrawIndexedHook {};
    ComPtr<ID3D11Buffer> gStaging;
    bool gInstalled = false;

    bool EnsureStaging(ID3D11Device* dev)
    {
        if (gStaging) return true;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = 64;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, gStaging.GetAddressOf()));
    }

    // Copy numBytes from src[srcByteOff] into the staging buffer and read them out. numBytes <= 64.
    bool ReadBytes(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT srcByteOff, UINT numBytes, void* out)
    {
        D3D11_BUFFER_DESC sd {};
        src->GetDesc(&sd);
        if (srcByteOff + numBytes > sd.ByteWidth)
        {
            return false;
        }
        D3D11_BOX box { srcByteOff, 0, 0, srcByteOff + numBytes, 1, 1 };
        ctx->CopySubresourceRegion(gStaging.Get(), 0, 0, 0, 0, src, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(gStaging.Get(), 0, D3D11_MAP_READ, 0, &m)))
        {
            return false;
        }
        memcpy(out, m.pData, numBytes);
        ctx->Unmap(gStaging.Get(), 0);
        return true;
    }

    void LogDoorjam(ID3D11DeviceContext* ctx, UINT startIndex, INT baseVertex)
    {
        ID3D11Device* dev = g_D3D11Hooks.d3dDevice.Get();
        if (!dev || !EnsureStaging(dev))
        {
            return;
        }

        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibfmt = DXGI_FORMAT_R16_UINT; UINT iboff = 0;
        ctx->IAGetIndexBuffer(&ib, &ibfmt, &iboff);
        ID3D11Buffer* vb = nullptr; UINT stride = 0, vboff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vboff);

        if (ib) ib->Release();
        if (vb) vb->Release();

        // The model vertex is static, so the motion/jitter is in the transform. Read the bound
        // VS constant buffers and log the first one that looks like a 4x4 matrix, per frame.
        ID3D11Buffer* cbs[6] = {};
        ctx->VSGetConstantBuffers(0, 6, cbs);
        for (int s = 0; s < 6; ++s)
        {
            if (!cbs[s]) continue;
            D3D11_BUFFER_DESC cd {};
            cbs[s]->GetDesc(&cd);
            float m[16] = {};
            if (cd.ByteWidth >= 64 && ReadBytes(ctx, cbs[s], 0, 64, m))
            {
                spdlog::info("MGS2 doorjam CB{} ({}B): [{:.4f} {:.4f} {:.4f} {:.4f}][{:.4f} {:.4f} {:.4f} {:.4f}][{:.4f} {:.4f} {:.4f} {:.4f}][{:.4f} {:.4f} {:.4f} {:.4f}]",
                    s, cd.ByteWidth,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                    m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
        for (int s = 0; s < 6; ++s) if (cbs[s]) cbs[s]->Release();
    }

    void __stdcall DrawIndexedHooked(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
    {
        gDrawIndexedHook.stdcall<void>(ctx, indexCount, startIndex, baseVertex);
        if (g_Logging.bVerboseLogging && indexCount == kDoorjamIndexCount && startIndex == kDoorjamStartIndex)
        {
            LogDoorjam(ctx, startIndex, baseVertex);
        }
    }
}

void MGS2DoorjamProbe::Initialize(ID3D11DeviceContext* context)
{
    if (gInstalled || !(eGameType & MGS2) || !context)
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(context);
    gDrawIndexedHook = safetyhook::create_inline(vtable[kDrawIndexedVtbl], reinterpret_cast<void*>(DrawIndexedHooked));
    gInstalled = static_cast<bool>(gDrawIndexedHook);
    if (gInstalled)
    {
        spdlog::info("MGS2 doorjam probe: DrawIndexed hooked (watching IndexCount={} StartIndex={}).",
            kDoorjamIndexCount, kDoorjamStartIndex);
    }
}
