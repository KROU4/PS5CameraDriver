#pragma once
// HRESULT and its helpers outside Windows, so that the pipeline has one interface everywhere.
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdint>

using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057u);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000Eu);
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001u);
constexpr HRESULT E_NOT_VALID_STATE = static_cast<HRESULT>(0x8007139Fu);
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#endif
