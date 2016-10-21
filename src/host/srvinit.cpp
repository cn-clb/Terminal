/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"
#include "srvinit.h"

#include "dbcs.h"
#include "directio.h"
#include "getset.h"
#include "globals.h"
#include "handle.h"
#include "icon.hpp"
#include "misc.h"
#include "output.h"
#include "registry.hpp"
#include "stream.h"
#include "renderFontDefaults.hpp"
#include "windowdpiapi.hpp"
#include "userprivapi.hpp"

#include "..\server\Entrypoints.h"

#pragma hdrstop

const UINT CONSOLE_EVENT_FAILURE_ID = 21790;
const UINT CONSOLE_LPC_PORT_FAILURE_ID = 21791;

void LoadLinkInfo(_Inout_ Settings* pLinkSettings,
                  _Inout_updates_bytes_(*pdwTitleLength) LPWSTR pwszTitle,
                  _Inout_ PDWORD pdwTitleLength,
                  _In_ PCWSTR pwszCurrDir,
                  _In_ PCWSTR pwszAppName)
{
    WCHAR wszIconLocation[MAX_PATH] = { 0 };
    int iIconIndex = 0;

    pLinkSettings->SetCodePage(g_uiOEMCP);

    // Did we get started from a link?
    if (pLinkSettings->GetStartupFlags() & STARTF_TITLEISLINKNAME)
    {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        {
            const size_t cbTitle = (*pdwTitleLength + 1) * sizeof(WCHAR);
            g_ciConsoleInformation.LinkTitle = (PWSTR) new BYTE[cbTitle];

            NTSTATUS Status = NT_TESTNULL(g_ciConsoleInformation.LinkTitle);
            if (NT_SUCCESS(Status))
            {
                if (FAILED(StringCbCopyNW(g_ciConsoleInformation.LinkTitle, cbTitle, pwszTitle, *pdwTitleLength)))
                {
                    Status = STATUS_UNSUCCESSFUL;
                }

                if (NT_SUCCESS(Status))
                {
                    CONSOLE_STATE_INFO csi = { 0 };
                    csi.LinkTitle = g_ciConsoleInformation.LinkTitle;
                    WCHAR wszShortcutTitle[MAX_PATH];
                    BOOL fReadConsoleProperties;
                    WORD wShowWindow = pLinkSettings->GetShowWindow();
                    DWORD dwHotKey = pLinkSettings->GetHotKey();
                    Status = ShortcutSerialization::s_GetLinkValues(&csi,
                                                                    &fReadConsoleProperties,
                                                                    wszShortcutTitle,
                                                                    ARRAYSIZE(wszShortcutTitle),
                                                                    wszIconLocation,
                                                                    ARRAYSIZE(wszIconLocation),
                                                                    &iIconIndex,
                                                                    (int*) &(wShowWindow),
                                                                    (WORD*)&(dwHotKey));
                    pLinkSettings->SetShowWindow(wShowWindow);
                    pLinkSettings->SetHotKey(dwHotKey);
                    // if we got a title, use it. even on overall link value load failure, the title will be correct if
                    // filled out.
                    if (wszShortcutTitle[0] != L'\0')
                    {
                        // guarantee null termination to make OACR happy.
                        wszShortcutTitle[ARRAYSIZE(wszShortcutTitle) - 1] = L'\0';
                        StringCbCopyW(pwszTitle, *pdwTitleLength, wszShortcutTitle);

                        // OACR complains about the use of a DWORD here, so roundtrip through a size_t
                        size_t cbTitleLength;
                        if (SUCCEEDED(StringCbLengthW(pwszTitle, *pdwTitleLength, &cbTitleLength)))
                        {
                            // don't care about return result -- the buffer is guaranteed null terminated to at least
                            // the length of Title
                            (void)SizeTToDWord(cbTitleLength, pdwTitleLength);
                        }
                    }

                    if (NT_SUCCESS(Status) && fReadConsoleProperties)
                    {
                        // copy settings
                        pLinkSettings->InitFromStateInfo(&csi);

                        // since we were launched via shortcut, make sure we don't let the invoker's STARTUPINFO pollute the
                        // shortcut's settings
                        pLinkSettings->UnsetStartupFlag(STARTF_USESIZE | STARTF_USECOUNTCHARS);
                    }
                    else
                    {
                        // if we didn't find any console properties, or otherwise failed to load link properties, pretend
                        // like we weren't launched from a shortcut -- this allows us to at least try to find registry
                        // settings based on title.
                        pLinkSettings->UnsetStartupFlag(STARTF_TITLEISLINKNAME);
                    }
                }
            }
            CoUninitialize();
        }
    }

    // Go get the icon
    if (wszIconLocation[0] == L'\0')
    {
        // search for the application along the path so that we can load its icons (if we didn't find one explicitly in
        // the shortcut)
        const DWORD dwLinkLen = SearchPathW(pwszCurrDir, pwszAppName, nullptr, ARRAYSIZE(wszIconLocation), wszIconLocation, nullptr);
        if (dwLinkLen <= 0 || dwLinkLen > sizeof(wszIconLocation))
        {
            StringCchCopyW(wszIconLocation, ARRAYSIZE(wszIconLocation), pwszAppName);
        }
    }

    if (wszIconLocation[0] != L'\0')
    {
        Icon::Instance().LoadIconsFromPath(wszIconLocation, iIconIndex);
    }

    if (!IsValidCodePage(pLinkSettings->GetCodePage()))
    {
        // make sure we don't leave this function with an invalid codepage
        pLinkSettings->SetCodePage(g_uiOEMCP);
    }
}

HRESULT ConsoleServerInitialization(_In_ HANDLE Server)
{
    try
    {
        g_pDeviceComm = new DeviceComm(Server);
    }
    CATCH_RETURN();

    g_uiOEMCP = GetOEMCP();

    g_pFontDefaultList = new RenderFontDefaults();
    RETURN_IF_NULL_ALLOC(g_pFontDefaultList);

    FontInfo::s_SetFontDefaultList(g_pFontDefaultList);

    // Removed allocation of scroll buffer here.
    return S_OK;
}

NTSTATUS SetUpConsole(_Inout_ Settings* pStartupSettings,
                      _In_ DWORD TitleLength,
                      _In_reads_bytes_(TitleLength) LPWSTR Title,
                      _In_ LPCWSTR CurDir,
                      _In_ LPCWSTR AppName)
{
    // We will find and locate all relevant preference settings and then create the console here.
    // The precedence order for settings is:
    // 1. STARTUPINFO settings
    // 2a. Shortcut/Link settings
    // 2b. Registry specific settings
    // 3. Registry default settings
    // 4. Hardcoded default settings
    // To establish this hierarchy, we will need to load the settings and apply them in reverse order.

    // 4. Initializing Settings will establish hardcoded defaults.
    // Set to reference of global console information since that's the only place we need to hold the settings.
    Settings& settings = g_ciConsoleInformation;

    // 3. Read the default registry values.
    Registry reg(&settings);
    reg.LoadGlobalsFromRegistry();
    reg.LoadDefaultFromRegistry();

    // 2. Read specific settings

    // Link is expecting the flags from the process to be in already, so apply that first
    settings.SetStartupFlags(pStartupSettings->GetStartupFlags());

    // We need to see if we were spawned from a link. If we were, we need to
    // call back into the shell to try to get all the console information from the link.
    LoadLinkInfo(&settings, Title, &TitleLength, CurDir, AppName);

    // If we weren't started from a link, this will already be set.
    // If LoadLinkInfo couldn't find anything, it will remove the flag so we can dig in the registry.
    if (!(settings.IsStartupTitleIsLinkNameSet()))
    {
        reg.LoadFromRegistry(Title);
    }

    // 1. The settings we were passed contains STARTUPINFO structure settings to be applied last.
    settings.ApplyStartupInfo(pStartupSettings);

    // Validate all applied settings for correctness against final rules.
    settings.Validate();

    // As of the graphics refactoring to library based, all fonts are now DPI aware. Scaling is performed at the Blt time for raster fonts.
    // Note that we can only declare our DPI awareness once per process launch.
    // Set the process's default dpi awareness context to PMv2 so that new top level windows
    // inherit their WM_DPICHANGED* broadcast mode (and more, like dialog scaling) from the thread.
    if (!WindowDpiApi::s_SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        // Fallback to per-monitor aware V1 if the API isn't available.
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

        // Allow child dialogs (i.e. Properties and Find) to scale automatically based on DPI if we're currently DPI aware.
        // Note that we don't need to do this if we're PMv2.
        WindowDpiApi::s_EnablePerMonitorDialogScaling();
    }

    //Save initial font name for comparison on exit. We want telemetry when the font has changed
    if (settings.IsFaceNameSet())
    {
        settings.SetLaunchFaceName(settings.GetFaceName(), LF_FACESIZE);
    }

    // Now we need to actually create the console using the settings given.
#pragma prefast(suppress:26018, "PREfast can't detect null termination status of Title.")

// Allocate console will read the global g_ciConsoleInformation for the settings we just set.
    NTSTATUS Status = AllocateConsole(Title, TitleLength);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS RemoveConsole(_In_ ConsoleProcessHandle* ProcessData)
{
    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    ASSERT(NT_SUCCESS(Status));

    FreeCommandHistory((HANDLE)ProcessData);

    bool const fRecomputeOwner = ProcessData->fRootProcess;
    g_ciConsoleInformation.ProcessHandleList.FreeProcessData(ProcessData);

    if (fRecomputeOwner)
    {
        SetConsoleWindowOwner(g_ciConsoleInformation.hWnd, nullptr);
    }

    UnlockConsole();

    return Status;
}

DWORD ConsoleIoThread();

DWORD ConsoleInputThread(LPVOID lpParameter);

void ConsoleCheckDebug()
{
#ifdef DBG
    HKEY hCurrentUser;
    HKEY hConsole;
    NTSTATUS status = RegistrySerialization::s_OpenConsoleKey(&hCurrentUser, &hConsole);

    if (NT_SUCCESS(status))
    {
        DWORD dwData = 0;
        status = RegistrySerialization::s_QueryValue(hConsole, L"DebugLaunch", sizeof(dwData), (BYTE*)&dwData, nullptr);

        if (NT_SUCCESS(status))
        {
            if (dwData != 0)
            {
                DebugBreak();
            }
        }

        RegCloseKey(hConsole);
        RegCloseKey(hCurrentUser);
    }
#endif
}

HRESULT ConsoleCreateIoThreadLegacy(_In_ HANDLE Server)
{
    ConsoleCheckDebug();

    RETURN_IF_FAILED(ConsoleServerInitialization(Server));
    RETURN_IF_FAILED(g_hConsoleInputInitEvent.create(wil::EventOptions::None));

    // Set up and tell the driver about the input available event.
    RETURN_IF_FAILED(g_hInputEvent.create(wil::EventOptions::ManualReset));

    CD_IO_SERVER_INFORMATION ServerInformation;
    ServerInformation.InputAvailableEvent = g_hInputEvent.get();
    RETURN_IF_FAILED(g_pDeviceComm->SetServerInformation(&ServerInformation));

    HANDLE const hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)ConsoleIoThread, 0, 0, nullptr);
    RETURN_IF_HANDLE_NULL(hThread);
    LOG_IF_WIN32_BOOL_FALSE(CloseHandle(hThread)); // The thread will run on its own and close itself. Free the associated handle.

    return S_OK;
}

HRESULT ConsoleCreateIoThread(_In_ HANDLE Server)
{
    return Entrypoints::StartConsoleForServerHandle(Server);
}

#define SYSTEM_ROOT         (L"%SystemRoot%")
#define SYSTEM_ROOT_LENGTH  (sizeof(SYSTEM_ROOT) - sizeof(WCHAR))

// Routine Description:
// - This routine translates path characters into '_' characters because the NT registry apis do not allow the creation of keys with
//   names that contain path characters. It also converts absolute paths into %SystemRoot% relative ones. As an example, if both behaviors were
//   specified it would convert a title like C:\WINNT\System32\cmd.exe to %SystemRoot%_System32_cmd.exe.
// Arguments:
// - ConsoleTitle - Pointer to string to translate.
// - Unexpand - Convert absolute path to %SystemRoot% relative one.
// - Substitute - Whether string-substitution ('_' for '\') should occur.
// Return Value:
// - Pointer to translated title or nullptr.
// Note:
// - This routine allocates a buffer that must be freed.
PWSTR TranslateConsoleTitle(_In_ PCWSTR pwszConsoleTitle, _In_ const BOOL fUnexpand, _In_ const BOOL fSubstitute)
{
    LPWSTR Tmp = nullptr;

    size_t cbConsoleTitle;
    size_t cbSystemRoot;

    LPWSTR pwszSysRoot = new wchar_t[MAX_PATH];
    if (nullptr != pwszSysRoot)
    {
        if (0 != GetSystemDirectoryW(pwszSysRoot, MAX_PATH))
        {
            if (SUCCEEDED(StringCbLengthW(pwszConsoleTitle, STRSAFE_MAX_CCH, &cbConsoleTitle)) &&
                SUCCEEDED(StringCbLengthW(pwszSysRoot, MAX_PATH, &cbSystemRoot)))
            {
                int const cchSystemRoot = (int)(cbSystemRoot / sizeof(WCHAR));
                int const cchConsoleTitle = (int)(cbConsoleTitle / sizeof(WCHAR));
                cbConsoleTitle += sizeof(WCHAR); // account for nullptr terminator

                if (fUnexpand &&
                    cchConsoleTitle >= cchSystemRoot &&
#pragma prefast(suppress:26018, "We've guaranteed that cchSystemRoot is equal to or smaller than cchConsoleTitle in size.")
                    (CSTR_EQUAL == CompareStringOrdinal(pwszConsoleTitle, cchSystemRoot, pwszSysRoot, cchSystemRoot, TRUE)))
                {
                    cbConsoleTitle -= cbSystemRoot;
                    pwszConsoleTitle += cchSystemRoot;
                    cbSystemRoot = SYSTEM_ROOT_LENGTH;
                }
                else
                {
                    cbSystemRoot = 0;
                }

                LPWSTR TranslatedConsoleTitle;
                Tmp = TranslatedConsoleTitle = (PWSTR)new BYTE[cbSystemRoot + cbConsoleTitle];
                if (TranslatedConsoleTitle == nullptr)
                {
                    return nullptr;
                }

                memmove(TranslatedConsoleTitle, SYSTEM_ROOT, cbSystemRoot);
                TranslatedConsoleTitle += (cbSystemRoot / sizeof(WCHAR));   // skip by characters -- not bytes

                for (UINT i = 0; i < cbConsoleTitle; i += sizeof(WCHAR))
                {
#pragma prefast(suppress:26018, "We are reading the null portion of the buffer on purpose and will escape on reaching it below.")
                    if (fSubstitute && *pwszConsoleTitle == '\\')
                    {
#pragma prefast(suppress:26019, "Console title must contain system root if this path was followed.")
                        *TranslatedConsoleTitle++ = (WCHAR)'_';
                    }
                    else
                    {
                        *TranslatedConsoleTitle++ = *pwszConsoleTitle;
                        if (*pwszConsoleTitle == L'\0')
                        {
                            break;
                        }
                    }

                    pwszConsoleTitle++;
                }
            }
        }
        delete[] pwszSysRoot;
    }

    return Tmp;
}

NTSTATUS GetConsoleLangId(_In_ const UINT uiOutputCP, _Out_ LANGID * const pLangId)
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    if (pLangId != nullptr)
    {
        switch (uiOutputCP)
        {
        case CP_JAPANESE:
            *pLangId = MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT);
            break;
        case CP_KOREAN:
            *pLangId = MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN);
            break;
        case CP_CHINESE_SIMPLIFIED:
            *pLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
            break;
        case CP_CHINESE_TRADITIONAL:
            *pLangId = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
            break;
        default:
            *pLangId = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
            break;
        }
    }
    Status = STATUS_SUCCESS;

    return Status;
}

NTSTATUS SrvGetConsoleLangId(_Inout_ PCONSOLE_API_MSG m, _Inout_ PBOOL /*ReplyPending*/)
{
    PCONSOLE_LANGID_MSG const a = &m->u.consoleMsgL1.GetConsoleLangId;

    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::GetConsoleLangId);

    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = GetConsoleLangId(g_ciConsoleInformation.OutputCP, &a->LangId);

    UnlockConsole();

    return Status;
}

// Routine Description:
// - This routine reads the connection information from a 'connect' IO, validates it and stores them in an internal format.
// - N.B. The internal informat contains information not sent by clients in their connect IOs and intialized by other routines.
// Arguments:
// - Server - Supplies a handle to the console server.
// - Message - Supplies the message representing the connect IO.
// - Cac - Receives the connection information.
// Return Value:
// - NTSTATUS indicating if the connection information was successfully initialized.
NTSTATUS ConsoleInitializeConnectInfo(_In_ PCONSOLE_API_MSG Message, _Out_ PCONSOLE_API_CONNECTINFO Cac)
{
    CONSOLE_SERVER_MSG Data = { 0 };

    // Try to receive the data sent by the client.
    NTSTATUS Status = NTSTATUS_FROM_HRESULT(Message->ReadMessageInput(0, &Data, sizeof Data));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // Validate that strings are within the buffers and null-terminated.
    if ((Data.ApplicationNameLength > (sizeof(Data.ApplicationName) - sizeof(WCHAR))) ||
        (Data.TitleLength > (sizeof(Data.Title) - sizeof(WCHAR))) ||
        (Data.CurrentDirectoryLength > (sizeof(Data.CurrentDirectory) - sizeof(WCHAR))) ||
        (Data.ApplicationName[Data.ApplicationNameLength / sizeof(WCHAR)] != UNICODE_NULL) ||
        (Data.Title[Data.TitleLength / sizeof(WCHAR)] != UNICODE_NULL) || (Data.CurrentDirectory[Data.CurrentDirectoryLength / sizeof(WCHAR)] != UNICODE_NULL))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    // Initialize (partially) the connect info with the received data.
    ASSERT(sizeof(Cac->AppName) == sizeof(Data.ApplicationName));
    ASSERT(sizeof(Cac->Title) == sizeof(Data.Title));
    ASSERT(sizeof(Cac->CurDir) == sizeof(Data.CurrentDirectory));

    // unused(Data.IconId)
    Cac->ConsoleInfo.SetHotKey(Data.HotKey);
    Cac->ConsoleInfo.SetStartupFlags(Data.StartupFlags);
    Cac->ConsoleInfo.SetFillAttribute(Data.FillAttribute);
    Cac->ConsoleInfo.SetShowWindow(Data.ShowWindow);
    Cac->ConsoleInfo.SetScreenBufferSize(Data.ScreenBufferSize);
    Cac->ConsoleInfo.SetWindowSize(Data.WindowSize);
    Cac->ConsoleInfo.SetWindowOrigin(Data.WindowOrigin);
    Cac->ProcessGroupId = Data.ProcessGroupId;
    Cac->ConsoleApp = Data.ConsoleApp;
    Cac->WindowVisible = Data.WindowVisible;
    Cac->TitleLength = Data.TitleLength;
    Cac->AppNameLength = Data.ApplicationNameLength;
    Cac->CurDirLength = Data.CurrentDirectoryLength;

    memmove(Cac->AppName, Data.ApplicationName, sizeof(Cac->AppName));
    memmove(Cac->Title, Data.Title, sizeof(Cac->Title));
    memmove(Cac->CurDir, Data.CurrentDirectory, sizeof(Cac->CurDir));

    return STATUS_SUCCESS;
}

NTSTATUS ConsoleAllocateConsole(PCONSOLE_API_CONNECTINFO p)
{
    // AllocConsole is outside our codebase, but we should be able to mostly track the call here.
    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::AllocConsole);

    NTSTATUS Status = SetUpConsole(&p->ConsoleInfo, p->TitleLength, p->Title, p->CurDir, p->AppName);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if (NT_SUCCESS(Status) && p->WindowVisible)
    {
        HANDLE Thread;

        Thread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)ConsoleInputThread, nullptr, 0, &g_dwInputThreadId);
        if (Thread == nullptr)
        {
            Status = STATUS_NO_MEMORY;
        }
        else
        {
            // The ConsoleInputThread needs to lock the console so we must first unlock it ourselves.
            UnlockConsole();
            g_hConsoleInputInitEvent.wait();
            LockConsole();

            CloseHandle(Thread);
            g_hConsoleInputInitEvent.release();

            if (!NT_SUCCESS(g_ntstatusConsoleInputInitStatus))
            {
                Status = g_ntstatusConsoleInputInitStatus;
            }
            else
            {
                Status = STATUS_SUCCESS;
            }

            /*
             * Tell driver to allow clients with UIAccess to connect
             * to this server even if the security descriptor doesn't
             * allow it.
             *
             * N.B. This allows applications like narrator.exe to have
             *      access to the console. This is ok because they already
             *      have access to the console window anyway - this function
             *      is only called when a window is created.
             */

            LOG_IF_FAILED(g_pDeviceComm->AllowUIAccess());
        }
    }
    else
    {
        g_ciConsoleInformation.Flags |= CONSOLE_NO_WINDOW;
    }

    return Status;
}

// Routine Description:
// - This routine is the main one in the console server IO thread.
// - It reads IO requests submitted by clients through the driver, services and completes them in a loop.
// Arguments:
// - <none>
// Return Value:
// - This routine never returns. The process exits when no more references or clients exist.
#include "..\server\IoSorter.h"
DWORD ConsoleIoThread()
{
    CONSOLE_API_MSG ReceiveMsg;
    ReceiveMsg._pDeviceComm = g_pDeviceComm;
    PCONSOLE_API_MSG ReplyMsg = nullptr;

    bool fShouldExit = false;
    while (!fShouldExit)
    {
        if (ReplyMsg != nullptr)
        {
            ReplyMsg->ReleaseMessageBuffers();
        }

        // TODO: correct mixed NTSTATUS/HRESULT
        HRESULT hr = g_pDeviceComm->ReadIo(&ReplyMsg->Complete, &ReceiveMsg);
        if (FAILED(hr))
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED))
            {
                fShouldExit = true;

                // This will not return. Terminate immediately when disconnected.
                TerminateProcess(GetCurrentProcess(), STATUS_SUCCESS);
            }
            RIPMSG1(RIP_WARNING, "DeviceIoControl failed with Result 0x%x", hr);
            ReplyMsg = nullptr;
            continue;
        }

        IoSorter::ServiceIoOperation(&ReceiveMsg, &ReplyMsg);
    }

    return 0;
}

NTSTATUS SrvDeprecatedAPI(_Inout_ PCONSOLE_API_MSG /*m*/, _Inout_ PBOOL /*ReplyPending*/)
{
    // assert if we hit a deprecated API.
    ASSERT(TRUE);

    // One common aspect of the functions we deprecate is that they all RevalidateConsole and then UnlockConsole at the
    // end. Here we do the same thing to more closely emulate the old functions.
    CONSOLE_INFORMATION *Console;
    NTSTATUS Status = RevalidateConsole(&Console);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    UnlockConsole();
    return STATUS_UNSUCCESSFUL;
}
