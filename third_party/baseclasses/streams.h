//------------------------------------------------------------------------------
// File: Streams.h
//
// Desc: DirectShow base classes - defines overall streams architecture.
//
// Copyright (c) 1992-2001 Microsoft Corporation.  All rights reserved.
//------------------------------------------------------------------------------


#ifndef __STREAMS__
#define __STREAMS__

#ifdef	_MSC_VER
// disable some level-4 warnings, use #pragma warning(enable:###) to re-enable
#pragma warning(disable:4100) // warning C4100: unreferenced formal parameter
#pragma warning(disable:4201) // warning C4201: nonstandard extension used : nameless struct/union
#pragma warning(disable:4511) // warning C4511: copy constructor could not be generated
#pragma warning(disable:4512) // warning C4512: assignment operator could not be generated
#pragma warning(disable:4514) // warning C4514: "unreferenced inline function has been removed"

#if _MSC_VER>=1100
#define AM_NOVTABLE __declspec(novtable)
#else
#define AM_NOVTABLE
#endif
#endif	// MSC_VER


// Because of differences between Visual C++ and older Microsoft SDKs,
// you may have defined _DEBUG without defining DEBUG.  This logic
// ensures that both will be set if Visual C++ sets _DEBUG.
#ifdef _DEBUG
#ifndef DEBUG
#define DEBUG
#endif
#endif


#include <windows.h>
#include <windowsx.h>
#include <olectl.h>
#include <ddraw.h>
#include <mmsystem.h>


#ifndef NUMELMS
#if _WIN32_WINNT < 0x0600
   #define NUMELMS(aa) (sizeof(aa)/sizeof((aa)[0]))
#else
   #define NUMELMS(aa) ARRAYSIZE(aa)
#endif
#endif

///////////////////////////////////////////////////////////////////////////
// The following definitions come from the Platform SDK and are required if
// the applicaiton is being compiled with the headers from Visual C++ 6.0.
/////////////////////////////////////////////////// ////////////////////////
#ifndef InterlockedExchangePointer
	#define InterlockedExchangePointer(Target, Value) \
   (PVOID)InterlockedExchange((PLONG)(Target), (LONG)(Value))
#endif

#ifndef _WAVEFORMATEXTENSIBLE_
#define _WAVEFORMATEXTENSIBLE_
typedef struct {
    WAVEFORMATEX    Format;
    union {
        WORD wValidBitsPerSample;       /* bits of precision  */
        WORD wSamplesPerBlock;          /* valid if wBitsPerSample==0 */
        WORD wReserved;                 /* If neither applies, set to zero. */
    } Samples;
    DWORD           dwChannelMask;      /* which channels are */
                                        /* present in stream  */
    GUID            SubFormat;
} WAVEFORMATEXTENSIBLE, *PWAVEFORMATEXTENSIBLE;
#endif // !_WAVEFORMATEXTENSIBLE_

#if !defined(WAVE_FORMAT_EXTENSIBLE)
#define  WAVE_FORMAT_EXTENSIBLE                 0xFFFE
#endif // !defined(WAVE_FORMAT_EXTENSIBLE)

#ifndef GetWindowLongPtr
  #define GetWindowLongPtrA   GetWindowLongA
  #define GetWindowLongPtrW   GetWindowLongW
  #ifdef UNICODE
    #define GetWindowLongPtr  GetWindowLongPtrW
  #else
    #define GetWindowLongPtr  GetWindowLongPtrA
  #endif // !UNICODE
#endif // !GetWindowLongPtr

#ifndef SetWindowLongPtr
  #define SetWindowLongPtrA   SetWindowLongA
  #define SetWindowLongPtrW   SetWindowLongW
  #ifdef UNICODE
    #define SetWindowLongPtr  SetWindowLongPtrW
  #else
    #define SetWindowLongPtr  SetWindowLongPtrA
  #endif // !UNICODE
#endif // !SetWindowLongPtr

#ifndef GWLP_WNDPROC
  #define GWLP_WNDPROC        (-4)
#endif
#ifndef GWLP_HINSTANCE
  #define GWLP_HINSTANCE      (-6)
#endif
#ifndef GWLP_HWNDPARENT
  #define GWLP_HWNDPARENT     (-8)
#endif
#ifndef GWLP_USERDATA
  #define GWLP_USERDATA       (-21)
#endif
#ifndef GWLP_ID
  #define GWLP_ID             (-12)
#endif
#ifndef DWLP_MSGRESULT
  #define DWLP_MSGRESULT  0
#endif
#ifndef DWLP_DLGPROC
  #define DWLP_DLGPROC    DWLP_MSGRESULT + sizeof(LRESULT)
#endif
#ifndef DWLP_USER
  #define DWLP_USER       DWLP_DLGPROC + sizeof(DLGPROC)
#endif


#pragma warning(push)
#pragma warning(disable: 4312 4244)
// _GetWindowLongPtr
// Templated version of GetWindowLongPtr, to suppress spurious compiler warning.
template <class T>
T _GetWindowLongPtr(HWND hwnd, int nIndex)
{
    return (T)GetWindowLongPtr(hwnd, nIndex);
}

// _SetWindowLongPtr
// Templated version of SetWindowLongPtr, to suppress spurious compiler warning.
template <class T>
LONG_PTR _SetWindowLongPtr(HWND hwnd, int nIndex, T p)
{
    return SetWindowLongPtr(hwnd, nIndex, (LONG_PTR)p);
}
#pragma warning(pop)

///////////////////////////////////////////////////////////////////////////
// End Platform SDK definitions
///////////////////////////////////////////////////////////////////////////

#ifdef __MINGW32__

#include <intsafe.h>
#include <sal.h>
// SAL1 annotations the MinGW SDK doesn't carry — extended past
// the upstream snapshot to cover every annotation the BaseClasses
// headers reach for (`__deref_out`, `__out_ecount_part`, etc.).
// They're informational; expanding them to empty is sound. See
// `third_party/baseclasses/README.md` for context.
#define __in
#define __in_opt
#define __out
#define __out_opt
#define __inout
#define __inout_opt
#define __deref_in
#define __deref_out
#define __deref_out_opt
#define __deref_inout_opt
#define __in_bcount(x)
#define __in_bcount_opt(x)
#define __in_ecount(x)
#define __in_ecount_opt(x)
#define __out_bcount(x)
#define __out_bcount_part(x, y)
#define __out_ecount(x)
#define __out_ecount_part(x, y)
#define __inout_ecount_full(x)
#define __field_ecount_opt(x)
#define __deref_out_range(x, y)
#define __out_range(x, y)
#define __range(x, y)
#define __control_entrypoint(x)
#define __success(x)
#define __format_string
#define AM_NOVTABLE
#define __AMVIDEO__
#define WIDTHBYTES(bits) ((DWORD)(((bits)+31) & (~31)) / 8)
#define DIBWIDTHBYTES(bi) (DWORD)WIDTHBYTES((DWORD)(bi).biWidth * (DWORD)(bi).biBitCount)
#define _DIBSIZE(bi) (DIBWIDTHBYTES(bi) * (DWORD)(bi).biHeight)
#define DIBSIZE(bi) ((bi).biHeight < 0 ? (-1)*(_DIBSIZE(bi)) : _DIBSIZE(bi))
#define TRUECOLOR(pbmi) ((TRUECOLORINFO *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define COLORS(pbmi) ((RGBQUAD *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define BITMASKS(pbmi) ((DWORD *)(((LPBYTE)&((pbmi)->bmiHeader)) + (pbmi)->bmiHeader.biSize))
#define SIZE_EGA_PALETTE (iEGA_COLORS * sizeof(RGBQUAD))
#define SIZE_PALETTE (iPALETTE_COLORS * sizeof(RGBQUAD))
#define SIZE_MASKS (iMASK_COLORS * sizeof(DWORD))
#define SIZE_PREHEADER (FIELD_OFFSET(VIDEOINFOHEADER,bmiHeader))
#define SIZE_VIDEOHEADER (sizeof(BITMAPINFOHEADER) + SIZE_PREHEADER)
#define PALETTISED(pbmi) ((pbmi)->bmiHeader.biBitCount <= iPALETTE)
#define PALETTE_ENTRIES(pbmi) ((DWORD) 1 << (pbmi)->bmiHeader.biBitCount)
#define HEADER(pVideoInfo) (&(((VIDEOINFOHEADER *) (pVideoInfo))->bmiHeader))

__inline HRESULT SAFE_DIBWIDTHBYTES(_In_ const BITMAPINFOHEADER *pbi, _Out_ DWORD *pcbWidth)
{
    DWORD dw;
    HRESULT hr;
    if (pbi->biWidth < 0 || pbi->biBitCount <= 0) {
        return E_INVALIDARG;
    }
    //  Calculate width in bits
    hr = DWordMult((DWORD)pbi->biWidth, (DWORD)pbi->biBitCount, &dw);
    if (FAILED(hr)) {
        return hr;
    }
    //  Round up to bytes
    dw = (dw & 7) ? dw / 8 + 1: dw / 8;

    //  Round up to a multiple of 4 bytes
    if (dw & 3) {
        dw += 4 - (dw & 3);
    }

    *pcbWidth = dw;
    return S_OK;
}
__inline HRESULT SAFE_DIBSIZE(_In_ const BITMAPINFOHEADER *pbi, _Out_ DWORD *pcbSize)
{
    DWORD dw;
    DWORD dwWidthBytes;
    HRESULT hr;
    if (pbi->biHeight == 0x80000000) {
        return E_INVALIDARG;
    }
    hr = SAFE_DIBWIDTHBYTES(pbi, &dwWidthBytes);
    if (FAILED(hr)) {
        return hr;
    }
    dw = abs(pbi->biHeight);
    hr = DWordMult(dw, dwWidthBytes, &dw);
    if (FAILED(hr)) {
        return hr;
    }
    *pcbSize = dw;
    return S_OK;
}

extern "C" const GUID DECLSPEC_SELECTANY MEDIASUBTYPE_P010 = {808530000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };
extern "C" const GUID DECLSPEC_SELECTANY MEDIASUBTYPE_P016 = {909193296, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };

#endif

#include <strmif.h>     // Generated IDL header file for streams interfaces
#include <intsafe.h>    // required by amvideo.h

#include "reftime.h"    // Helper class for REFERENCE_TIME management
#include "wxdebug.h"    // Debug support for logging and ASSERTs
#include <amvideo.h>    // ActiveMovie video interfaces and definitions
//include amaudio.h explicitly if you need it.  it requires the DX SDK.
//#include <amaudio.h>    // ActiveMovie audio interfaces and definitions
#include "wxutil.h"     // General helper classes for threads etc
#include "combase.h"    // Base COM classes to support IUnknown
#include "dllsetup.h"   // Filter registration support functions
#include "measure.h"    // Performance measurement
#include <comlite.h>    // Light weight com function prototypes

#include "cache.h"      // Simple cache container class
#include "wxlist.h"     // Non MFC generic list class
#include "msgthrd.h"	// CMsgThread
#include "mtype.h"      // Helper class for managing media types
#include "fourcc.h"     // conversions between FOURCCs and GUIDs
#include <control.h>    // generated from control.odl
#include "ctlutil.h"    // control interface utility classes
#include <evcode.h>     // event code definitions
#include "amfilter.h"   // Main streams architecture class hierachy
#include "transfrm.h"   // Generic transform filter
#include "transip.h"    // Generic transform-in-place filter
#include <uuids.h>      // declaration of type GUIDs and well-known clsids
#include "source.h"	// Generic source filter
#include "outputq.h"    // Output pin queueing
#include <errors.h>     // HRESULT status and error definitions
#include "renbase.h"    // Base class for writing ActiveX renderers
#include "winutil.h"    // Helps with filters that manage windows
#include "winctrl.h"    // Implements the IVideoWindow interface
#include "videoctl.h"   // Specifically video related classes
#include "refclock.h"	// Base clock class
#include "sysclock.h"	// System clock
#include "pstream.h"    // IPersistStream helper class
#include "vtrans.h"     // Video Transform Filter base class
#include "amextra.h"
#include "cprop.h"      // Base property page class
#include "strmctl.h"    // IAMStreamControl support
#include <edevdefs.h>   // External device control interface defines
#include <audevcod.h>   // audio filter device error event codes

#if !defined(STRMBASE_BUILD) && defined(__MINGW32__)

#undef __in
#undef __out
#undef __deref_in
#undef __deref_inout_opt
#undef __in_bcount_opt
#undef __in_ecount_opt
#undef __field_ecount_opt
#undef __deref_out_range
#undef __out_range
#undef __control_entrypoint
#undef __success
#undef AM_NOVTABLE
#undef __AMVIDEO__

#endif

#else
    #ifdef DEBUG
    #pragma message("STREAMS.H included TWICE")
    #endif
#endif // __STREAMS__

