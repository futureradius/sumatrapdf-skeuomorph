/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Direct3D 11 and DirectComposition headers for the Curl files (CurlLayer,
// CurlRenderer, ScreenCopy). Only those .cpp files include this; the DLLs are
// loaded by hand, nothing is linked.

// dcomp.h needs Windows 8; same trick as src/shared/PrintWin11.cpp
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN8
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>
#pragma pop_macro("NTDDI_VERSION")

using Microsoft::WRL::ComPtr;

// a picture on the GPU of part of the canvas; area is in canvas pixels
struct CurlPicture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    Rect area;
    bool ok = false;
};
