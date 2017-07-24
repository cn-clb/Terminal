/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "vtrenderer.hpp"

#pragma hdrstop

using namespace Microsoft::Console::Render;

// Routine Description:
// - Notifies us that the system has requested a particular pixel area of the client rectangle should be redrawn. (On WM_PAINT)
// Arguments:
// - prcDirtyClient - Pointer to pixel area (RECT) of client region the system believes is dirty
// Return Value:
// - HRESULT S_OK, GDI-based error code, or safemath error
HRESULT VtEngine::InvalidateSystem(_In_ const RECT* const prcDirtyClient)
{
    prcDirtyClient;
    return S_OK;
}

// Routine Description:
// - Notifies us that the console is attempting to scroll the existing screen area
// Arguments:
// - pcoordDelta - Pointer to character dimension (COORD) of the distance the console would like us to move while scrolling.
// Return Value:
// - HRESULT S_OK, GDI-based error code, or safemath error
HRESULT VtEngine::InvalidateScroll(_In_ const COORD* const pcoordDelta)
{
    pcoordDelta;
    return S_OK;
}

// Routine Description:
// - Notifies us that the console has changed the selection region and would like it updated
// Arguments:
// - rgsrSelection - Array of character region rectangles (one per line) that represent the selected area
// - cRectangles - Length of the array above.
// Return Value:
// - HRESULT S_OK or GDI-based error code
HRESULT VtEngine::InvalidateSelection(_In_reads_(cRectangles) SMALL_RECT* const rgsrSelection, _In_ UINT const cRectangles)
{
    rgsrSelection;
    cRectangles;
    return S_OK;
}

// Routine Description:
// - Notifies us that the console has changed the character region specified.
// - NOTE: This typically triggers on cursor or text buffer changes
// Arguments:
// - psrRegion - Character region (SMALL_RECT) that has been changed
// Return Value:
// - S_OK, GDI related failure, or safemath failure.
HRESULT VtEngine::Invalidate(const SMALL_RECT* const psrRegion)
{
    psrRegion;
    return S_OK;
}

// Routine Description:
// - Notifies to repaint everything.
// - NOTE: Use sparingly. Only use when something that could affect the entire frame simultaneously occurs.
// Arguments:
// - <none>
// Return Value:
// - S_OK, GDI related failure, or safemath failure.
HRESULT VtEngine::InvalidateAll()
{
    return S_OK;
}
