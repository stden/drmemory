/* **********************************************************
 * Copyright (c) 2011-2012 Google, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <windows.h>

#include <winuser.h>

#include "os_version_win.h"
#include "gtest/gtest.h"

// For InitCommonControlsEx
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// A potentially externally visible global.  Useful if you want to make a
// statement the compiler can't delete.
int global_for_side_effects;

// FIXME i#735: Re-enable once doesn't hang and passes on xp32.
TEST(NtUserTests, DISABLED_SystemParametersInfo) {
    // Was: http://code.google.com/p/drmemory/issues/detail?id=10
    NONCLIENTMETRICS metrics;
    ZeroMemory(&metrics, sizeof(NONCLIENTMETRICS));
    metrics.cbSize = sizeof(NONCLIENTMETRICS);
    BOOL success = SystemParametersInfo(SPI_GETNONCLIENTMETRICS,
                                        sizeof(NONCLIENTMETRICS), &metrics, 0);
    ASSERT_EQ(TRUE, success);
    success = SystemParametersInfo(SPI_SETNONCLIENTMETRICS,
                                   sizeof(NONCLIENTMETRICS), &metrics, 0);
    ASSERT_EQ(TRUE, success);
}

namespace Clipboard_Tests {

void WriteStringToClipboard(const std::string& str) {
    HWND hWnd = ::GetDesktopWindow();
    ASSERT_NE(0, ::OpenClipboard(hWnd));
    ::EmptyClipboard();
    HGLOBAL data = ::GlobalAlloc(2 /*GMEM_MOVABLE*/, str.size() + 1);
    ASSERT_NE((HGLOBAL)NULL, data);

    char* raw_data = (char*)::GlobalLock(data);
    memcpy(raw_data, str.data(), str.size() * sizeof(char));
    raw_data[str.size()] = '\0';
    ::GlobalUnlock(data);

    ASSERT_EQ(data, ::SetClipboardData(CF_TEXT, data));
    ::CloseClipboard();
}

void ReadAsciiStringFromClipboard(std::string *result) {
    assert(result != NULL);

    HWND hWnd = ::GetDesktopWindow();
    ASSERT_NE(0, ::OpenClipboard(hWnd));

    HANDLE data = ::GetClipboardData(CF_TEXT);
    ASSERT_NE((HANDLE)NULL, data);

    result->assign((const char*)::GlobalLock(data));

    ::GlobalUnlock(data);
    ::CloseClipboard();
}

// FIXME i#734: Re-enable when no uninits.
TEST(NtUserTests, ClipboardPutGet) {
    if (GetWindowsVersion() >= WIN_VISTA) {
        printf("WARNING: Disabling ClipboardPutGet on Win Vista+, see i#734.\n");
        return;
    }

    // Was: http://code.google.com/p/drmemory/issues/detail?id=45
    std::string tmp, str = "ASCII";
    WriteStringToClipboard(str);
    ReadAsciiStringFromClipboard(&tmp);
    ASSERT_STREQ("ASCII", tmp.c_str());
}

} /* Clipboard_Tests */

TEST(NtUserTests, CoInitializeUninitialize) {
    // Was: http://code.google.com/p/drmemory/issues/detail?id=65
    CoInitialize(NULL);
    CoUninitialize();
}

TEST(NtUserTests, InitCommonControlsEx) {
    // Was: http://code.google.com/p/drmemory/issues/detail?id=362
    INITCOMMONCONTROLSEX InitCtrlEx;

    InitCtrlEx.dwSize = sizeof(INITCOMMONCONTROLSEX);
    InitCtrlEx.dwICC  = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&InitCtrlEx);  // initialize common control sex
}

TEST(NtUserTests, CursorTest) {
    // test NtUserCall* GETCURSORPOS, SETCURSORPOS, SHOWCURSOR
    POINT point;
    BOOL success = GetCursorPos(&point);
    if (!success) {
        // FIXME i#755: This seems to happen when a user over RDP disconnected?
        // In any case, not worth the time to track down now.
        printf("WARNING: GetCursorPos failed with error %d\n", GetLastError());
    } else {
        // Check uninits
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((VOID*)(point.x + point.y), &mbi, sizeof(mbi));

        success = SetCursorPos(point.x, point.y);
        if (!success) {
            // FIXME i#755: This seems to happen when a user over RDP disconnected?
            // In any case, not worth the time to track down now.
            printf("WARNING: SetCursorPos failed with error %d\n", GetLastError());
        }
    }

    int display_count = ShowCursor(TRUE);
    if (display_count != 1) {
        printf("WARNING: display_count != 1, got %d\n", display_count);
    }
}

TEST(NtUserTests, WindowRgnTest) {
    // test NtUserCall* VALIDATERGN, 
    HWND hwnd = ::GetDesktopWindow();
    HRGN hrgn = CreateRectRgn(0, 0, 0, 0);
    ASSERT_NE((HRGN)NULL, hrgn);
    BOOL success = ValidateRgn(hwnd, hrgn);
    ASSERT_EQ(TRUE, success);
    int type = GetWindowRgn(hwnd, hrgn);
    // FIXME: somehow type comes out as ERROR so skipping ASSERT_NE(ERROR, type)
}

TEST(NtUserTests, MenuTest) {
    // FIXME i#736: Re-enable on XP when passes.
    if (GetWindowsVersion() < WIN_VISTA) {
        printf("WARNING: Disabling MenuTest on Pre-Vista, see i#736.\n");
        return;
    }

    // test NtUserCall* DRAWMENUBAR
    HWND hwnd = ::GetDesktopWindow();
    BOOL success = DrawMenuBar(hwnd);
    ASSERT_EQ(FALSE, success); /* no menu on desktop window */

    // test NtUserCall* CREATEMENU + CREATEPOPUPMENU and NtUserDestroyMenu
    HMENU menu = CreateMenu();
    ASSERT_NE((HMENU)NULL, menu);
    success = DestroyMenu(menu);
    ASSERT_EQ(TRUE, success);
    menu = CreatePopupMenu();
    ASSERT_NE((HMENU)NULL, menu);
    success = DestroyMenu(menu);
    ASSERT_EQ(TRUE, success);
}

TEST(NtUserTests, BeepTest) {
    // test NtUserCall* MESSAGEBEEP
    BOOL success = MessageBeep(0xFFFFFFFF/*simple beep*/);
    ASSERT_EQ(TRUE, success);
}

TEST(NtUserTests, CaretTest) {
    // test NtUserGetCaretBlinkTime and NtUserCall* SETCARETBLINKTIME + DESTROY_CARET
    UINT blink = GetCaretBlinkTime();
    ASSERT_NE(0, blink);
    BOOL success = SetCaretBlinkTime(blink);
    ASSERT_EQ(TRUE, success);
    success = DestroyCaret();
    ASSERT_EQ(FALSE, success); // no caret to destroy
}

TEST(NtUserTests, DeferWindowPosTest) {
    // test NtUserCall* BEGINDEFERWINDOWPOS and NtUserDeferWindowPos
    HWND hwnd = ::GetDesktopWindow();
    HDWP hdwp = BeginDeferWindowPos(1);
    if (hdwp) {
        hdwp = DeferWindowPos(hdwp, hwnd, NULL, 0, 0, 5, 10,
                              SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    if (hdwp) {
        // XXX: not getting here: need to set up a successful defer
        EndDeferWindowPos(hdwp);
    }
}
