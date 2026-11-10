/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Timer.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"

#include "skeuomorph/CurlD3D.h"
#include "skeuomorph/ScreenCopy.h"

// the UI thread never waits longer; a copy takes 1-2 ms, about a frame at 4K
constexpr int kCopyWaitMs = 200;

struct ScreenCopyDesk {
    Mutex mutex;
    HANDLE wake = nullptr; // the drawing thread's, not owned
    HANDLE done = nullptr;

    // a request; src == nullptr: none, or the UI thread gave up waiting
    HDC src = nullptr;
    Rect area;
    int slot = 0;
    int asked = 0;
    int served = 0;
    bool ok = false;
};

ScreenCopyDesk* NewScreenCopyDesk(HANDLE wake) {
    auto desk = new ScreenCopyDesk();
    desk->wake = wake;
    desk->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return desk;
}

void DeleteScreenCopyDesk(ScreenCopyDesk* desk) {
    if (!desk) {
        return;
    }
    CloseHandle(desk->done);
    delete desk;
}

Size ScreenCopySize(MainWindow* win) {
    if (!win || !win->buffer) {
        return {};
    }
    return win->buffer->rect.Size();
}

bool ScreenCopyAsk(ScreenCopyDesk* desk, MainWindow* win, Rect area, int slot) {
    if (!desk || !win || !win->buffer) {
        return false;
    }
    HDC src = win->buffer->GetDC();
    area = area.Intersect(Rect(Point(), ScreenCopySize(win)));
    if (!src || area.IsEmpty()) {
        return false;
    }

    desk->mutex.Lock();
    desk->src = src;
    desk->area = area;
    desk->slot = slot;
    int ticket = ++desk->asked;
    desk->mutex.Unlock();
    SetEvent(desk->wake);

    // an older request may have signalled late: wait for this ticket
    TimeStamp start = TimeGet();
    bool ok = false;
    while (true) {
        int left = kCopyWaitMs - (int)TimeSinceInMs(start);
        if (left <= 0) {
            break;
        }
        WaitForSingleObject(desk->done, (DWORD)left);
        desk->mutex.Lock();
        bool served = desk->served == ticket;
        ok = served && desk->ok;
        desk->mutex.Unlock();
        if (served) {
            break;
        }
    }

    // gave up: take the DC back. The drawing thread copies under the mutex,
    // so once we hold it no copy is running
    desk->mutex.Lock();
    desk->src = nullptr;
    desk->mutex.Unlock();
    return ok;
}

// a GDI-compatible picture of the given size; kept when the size matches
static bool EnsurePicture(ID3D11Device* dev, CurlPicture* pic, Size size) {
    D3D11_TEXTURE2D_DESC have{};
    if (pic->tex) {
        pic->tex->GetDesc(&have);
        if ((int)have.Width == size.dx && (int)have.Height == size.dy) {
            return true;
        }
    }
    pic->tex.Reset();
    pic->srv.Reset();

    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)size.dx;
    d.Height = (UINT)size.dy;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &pic->tex))) {
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(pic->tex.Get(), nullptr, &pic->srv))) {
        pic->tex.Reset();
        return false;
    }
    return true;
}

static bool CopyInto(ID3D11Device* dev, HDC src, Rect area, CurlPicture* pic) {
    pic->ok = false;
    pic->area = area;
    if (!EnsurePicture(dev, pic, area.Size())) {
        return false;
    }
    ComPtr<IDXGISurface1> surf;
    if (FAILED(pic->tex.As(&surf))) {
        return false;
    }
    HDC dst = nullptr;
    if (FAILED(surf->GetDC(TRUE, &dst))) {
        return false;
    }
    BOOL ok = BitBlt(dst, 0, 0, area.dx, area.dy, src, area.x, area.y, SRCCOPY);
    surf->ReleaseDC(nullptr);
    pic->ok = !!ok;
    return pic->ok;
}

void ScreenCopyServe(ScreenCopyDesk* desk, ID3D11Device* dev, CurlPicture* slots, int nSlots) {
    desk->mutex.Lock();
    if (!desk->src) {
        desk->mutex.Unlock();
        return;
    }
    bool ok = false;
    if (desk->slot >= 0 && desk->slot < nSlots) {
        ok = CopyInto(dev, desk->src, desk->area, &slots[desk->slot]);
    }
    desk->src = nullptr;
    desk->served = desk->asked;
    desk->ok = ok;
    desk->mutex.Unlock();
    SetEvent(desk->done);
}
