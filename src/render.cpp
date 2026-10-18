// Draws the battle UI inside KH2's own frame: IDXGISwapChain::Present is hooked and Dear ImGui renders
// with the DX11 or DX12 backend, whichever device the game created.
#include "common.h"
#include "ui.h"
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_2.h>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PresentFn oPresent = nullptr; ResizeFn oResize = nullptr; ExecFn oExec = nullptr; Present1Fn oPresent1 = nullptr;
int g_initTries = 0;
using CreateSCFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSCHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
CreateSCFn oCreateSC = nullptr; CreateSCHwndFn oCreateSCHwnd = nullptr;
bool g_queueFromSwapChain = false;

enum class Api { None, Dx11, Dx12 } g_api = Api::None;
bool g_inited = false, g_failed = false;
HWND g_hwnd = nullptr;
WNDPROC g_oldProc = nullptr;

// DX11
ID3D11Device* g_d11 = nullptr; ID3D11DeviceContext* g_ctx = nullptr; ID3D11RenderTargetView* g_rtv = nullptr;
// DX12
ID3D12Device* g_d12 = nullptr; ID3D12CommandQueue* g_queue = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr; ID3D12DescriptorHeap* g_rtvHeap = nullptr;
struct Frame12 { ID3D12CommandAllocator* alloc = nullptr; ID3D12Resource* rt = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}; UINT64 fenceValue = 0; };
std::vector<Frame12> g_frames; ID3D12GraphicsCommandList* g_list = nullptr; UINT g_bufCount = 0;
ID3D12Fence* g_fence = nullptr; UINT64 g_fenceVal = 0; HANDLE g_fenceEv = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_inited) ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    return CallWindowProcW(g_oldProc, h, m, w, l);
}

void Release11() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }
void Create11(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { g_d11->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
}
void Release12() {
    if (g_fence && g_queue) { g_queue->Signal(g_fence, ++g_fenceVal); if (g_fence->GetCompletedValue() < g_fenceVal) { g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEv); WaitForSingleObject(g_fenceEv, 2000); } }
    for (auto& f : g_frames) { if (f.rt) f.rt->Release(); f.rt = nullptr; }
}
void Create12(IDXGISwapChain* sc) {
    UINT inc = g_d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_bufCount; ++i) {
        ID3D12Resource* r = nullptr;
        if (SUCCEEDED(sc->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&r))) { g_d12->CreateRenderTargetView(r, nullptr, h); g_frames[i].rt = r; }
        g_frames[i].rtv = h; h.ptr += inc;
    }
}

bool Init(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    ++g_initTries;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_d11))) g_api = Api::Dx11;
    else if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&g_d12))) g_api = Api::Dx12;
    else { if (g_initTries < 3) Log("render: swapchain has neither a DX11 nor a DX12 device"); return false; }
    if (g_api == Api::Dx12 && (!g_queue || !g_queueFromSwapChain)) {
        if (g_initTries < 3 || g_initTries % 600 == 0) Log("render: DX12 device, waiting for its command queue (try %d)", g_initTries);
        if (g_d12) { g_d12->Release(); g_d12 = nullptr; } g_api = Api::None; return false;
    }
    Log("render: first Present seen (%s), %u buffers, format %d", g_api == Api::Dx11 ? "DX11" : "DX12", d.BufferCount, (int)d.BufferDesc.Format);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    ui::Setup();
    ImGui_ImplWin32_Init(g_hwnd);
    if (g_api == Api::Dx11) {
        g_d11->GetImmediateContext(&g_ctx);
        ImGui_ImplDX11_Init(g_d11, g_ctx);
        Create11(sc);
    } else {
        g_bufCount = d.BufferCount;
        g_frames.resize(g_bufCount);
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = g_bufCount; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
        for (auto& f : g_frames) if (FAILED(g_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
        if (FAILED(g_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
        g_list->Close();
        g_d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        g_fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ImGui_ImplDX12_InitInfo ii;
        ii.Device = g_d12; ii.CommandQueue = g_queue; ii.NumFramesInFlight = (int)g_bufCount;
        ii.RTVFormat = d.BufferDesc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : d.BufferDesc.Format;
        ii.DSVFormat = DXGI_FORMAT_UNKNOWN; ii.SrvDescriptorHeap = g_srvHeap;
        ii.LegacySingleSrvCpuDescriptor = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        ii.LegacySingleSrvGpuDescriptor = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        if (!ImGui_ImplDX12_Init(&ii)) return false;
        Create12(sc);
    }
    g_oldProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
    Log("render: ImGui on %s, %ux%u, window %p", g_api == Api::Dx11 ? "DX11" : "DX12", d.BufferDesc.Width, d.BufferDesc.Height, g_hwnd);
    return true;
}

void Draw(IDXGISwapChain* sc) {
    if (g_api == Api::Dx11) ImGui_ImplDX11_NewFrame(); else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ui::Frame(g_hwnd);
    ImGui::Render();
    if (g_api == Api::Dx11) {
        if (!g_rtv) Create11(sc);
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    } else {
        if (g_d12->GetDeviceRemovedReason() != S_OK) { g_failed = true; g_inited = false; Log("render: device removed, UI off"); return; }
        if (!g_queueFromSwapChain) return; // never draw on a guessed queue
        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return;
        UINT i = sc3->GetCurrentBackBufferIndex(); sc3->Release();
        if (i >= g_frames.size() || !g_frames[i].rt) return;
        Frame12& f = g_frames[i];
        // Wait until this allocator's last use is finished before reusing it.
        if (f.fenceValue && g_fence->GetCompletedValue() < f.fenceValue) { g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEv); WaitForSingleObject(g_fenceEv, 100); }
        f.alloc->Reset();
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = f.rt;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_list->Reset(f.alloc, nullptr);
        g_list->ResourceBarrier(1, &b);
        g_list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_list->SetDescriptorHeaps(1, &g_srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_list->ResourceBarrier(1, &b);
        g_list->Close();
        ID3D12CommandList* lists[] = {g_list};
        oExec(g_queue, 1, lists);
        g_queue->Signal(g_fence, ++g_fenceVal);
        f.fenceValue = g_fenceVal;
    }
}

void OnPresent(IDXGISwapChain* sc, UINT flags) {
    if (!g_inited && !g_failed) {
        __try { g_inited = Init(sc); } __except (EXCEPTION_EXECUTE_HANDLER) { g_failed = true; Log("render: init crashed, UI off"); }
    }
    if (g_inited && !(flags & DXGI_PRESENT_TEST)) {
        __try { Draw(sc); } __except (EXCEPTION_EXECUTE_HANDLER) { g_inited = false; g_failed = true; Log("render: draw crashed, UI off"); }
    }
}
thread_local bool t_inPresent = false;
HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!t_inPresent) { t_inPresent = true; OnPresent(sc, flags); t_inPresent = false; }
    return oPresent(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!t_inPresent) { t_inPresent = true; OnPresent(sc, flags); t_inPresent = false; }
    return oPresent1(sc, sync, flags, pp);
}
HRESULT STDMETHODCALLTYPE hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    if (g_inited) { if (g_api == Api::Dx11) Release11(); else Release12(); }
    HRESULT r = oResize(sc, n, w, h, fmt, fl);
    if (g_inited && SUCCEEDED(r)) {
        if (g_api == Api::Dx11) Create11(sc);
        else { DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d); if (d.BufferCount == g_bufCount) Create12(sc); else { g_failed = true; g_inited = false; Log("render: buffer count changed, UI off"); } }
    }
    return r;
}
void STDMETHODCALLTYPE hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l) {
    if (!g_queue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) { g_queue = q; q->AddRef(); Log("render: DX12 queue taken from ExecuteCommandLists (fallback)"); }
    oExec(q, n, l);
}
// The queue a DX12 swap chain is created with is the one it presents on: draw on exactly that queue.
void TakeQueue(IUnknown* dev) {
    ID3D12CommandQueue* q = nullptr;
    if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&q)))) {
        if (g_queue && !g_inited) g_queue->Release();
        if (!g_inited) { g_queue = q; g_queueFromSwapChain = true; Log("render: DX12 present queue taken from swap chain creation"); }
        else q->Release();
    }
}
HRESULT STDMETHODCALLTYPE hkCreateSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) { TakeQueue(dev); return oCreateSC(f, dev, d, out); }
HRESULT STDMETHODCALLTYPE hkCreateSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** out) { TakeQueue(dev); return oCreateSCHwnd(f, dev, h, d, fd, o, out); }

// Read vtables from throwaway devices made on a hidden window.
bool GetVtables(void** present, void** resize, void** exec, void** present1) {
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"ffxtb_dummy", nullptr};
    RegisterClassExW(&wc);
    HWND w = CreateWindowW(L"ffxtb_dummy", L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{}; d.BufferCount = 2; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.OutputWindow = w; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr);
    *present1 = nullptr;
    if (ok) {
        void** vt = *(void***)sc; *present = vt[8]; *resize = vt[13];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) { *present1 = (*(void***)sc1)[22]; sc1->Release(); }
        sc->Release(); dev->Release(); ctx->Release();
    }
    *exec = nullptr;
    ID3D12Device* d12 = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) {
        D3D12_COMMAND_QUEUE_DESC qd{}; ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) { *exec = (*(void***)q)[10]; q->Release(); }
        d12->Release();
    }
    DestroyWindow(w); UnregisterClassW(L"ffxtb_dummy", wc.hInstance);
    return ok;
}
}

namespace render {
bool InstallHooks() {
    void *p = nullptr, *r = nullptr, *e = nullptr, *p1 = nullptr;
    if (!GetVtables(&p, &r, &e, &p1)) { Log("render: could not read DXGI vtables"); return false; }
    bool ok = MH_CreateHook(p, (void*)hkPresent, (void**)&oPresent) == MH_OK && MH_CreateHook(r, (void*)hkResize, (void**)&oResize) == MH_OK;
    if (p1 && p1 != p) ok &= MH_CreateHook(p1, (void*)hkPresent1, (void**)&oPresent1) == MH_OK;
    IDXGIFactory2* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        void** vt = *(void***)fac;
        ok &= MH_CreateHook(vt[10], (void*)hkCreateSC, (void**)&oCreateSC) == MH_OK;
        ok &= MH_CreateHook(vt[15], (void*)hkCreateSCHwnd, (void**)&oCreateSCHwnd) == MH_OK;
        fac->Release();
    }
    if (e) ok &= MH_CreateHook(e, (void*)hkExec, (void**)&oExec) == MH_OK;
    ok &= MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    Log("render: hooks %s (dx12 queue hook %s)", ok ? "installed" : "FAILED", e ? "yes" : "no");
    return ok;
}
}
