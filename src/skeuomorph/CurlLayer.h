/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// The Curl layer: one DirectComposition visual over a window's canvas, below
// its child windows (topmost = FALSE, so edit boxes stay on top), showing a
// composition swap chain. Shown only while a Curl plays.
//
// Drawing thread only. Include CurlD3D.h first.
struct CurlLayer;

CurlLayer* NewCurlLayer(ID3D11Device* dev, HWND hwnd, Size size, const char** why);
void DeleteCurlLayer(CurlLayer* layer);

Size CurlLayerSize(CurlLayer* layer);
bool CurlLayerResize(CurlLayer* layer, Size size);

void CurlLayerShow(CurlLayer* layer, bool show);
bool CurlLayerShown(CurlLayer* layer);

HANDLE CurlLayerFrameWait(CurlLayer* layer);
ID3D11RenderTargetView* CurlLayerTarget(CurlLayer* layer);
ID3D11DepthStencilView* CurlLayerDepth(CurlLayer* layer);
bool CurlLayerPresent(CurlLayer* layer);
