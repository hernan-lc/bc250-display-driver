// SPDX-License-Identifier: MIT
// bc250umd.dll - user-mode display driver stub for BC250Kmd.sys.
//
// dxgkrnl refuses to start a render-capable WDDM adapter without a registered user-mode driver
// (UserModeDriverName). This DLL is that registration target. It implements no Direct3D yet:
// the runtimes get E_NOTIMPL and fall back (WARP / Basic Render), while the kernel driver and
// D3DKMT clients (libbc250wddm, a future Mesa winsys) can use the GPU.
//
// Entry points looked up by the D3D runtimes:
//   OpenAdapter       - Direct3D 9 UMD
//   OpenAdapter10     - Direct3D 10.0 UMD
//   OpenAdapter10_2   - Direct3D 10.1 / 11 / 12-on-11 UMD
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hinst);
    return TRUE;
}

// The argument structures are defined in d3dumddi.h / d3d10umddi.h; the stub never reads them.
__declspec(dllexport) HRESULT APIENTRY OpenAdapter(void* pOpenData)       { (void)pOpenData; return E_NOTIMPL; }
__declspec(dllexport) HRESULT APIENTRY OpenAdapter10(void* pOpenData)     { (void)pOpenData; return E_NOTIMPL; }
__declspec(dllexport) HRESULT APIENTRY OpenAdapter10_2(void* pOpenData)   { (void)pOpenData; return E_NOTIMPL; }
