/*
 * Copyright (c) 2015, Psiphon Inc.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

//==== Includes ===============================================================

#include "stdafx.h"

// This is for Windows XP/Vista+ style controls
#include <Commctrl.h>
#pragma comment (lib, "Comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// This is for COM functions
# pragma comment(lib, "wbemuuid.lib")

#include "psiclient.h"
#include <mCtrl/html.h>
#include "connectionmanager.h"
#include "embeddedvalues.h"
#include "transport.h"
#include "config.h"
#include "usersettings.h"
#include "utilities.h"
#include "webbrowser.h"
#include "limitsingleinstance.h"
#include "htmldlg.h"
#include "stopsignal.h"
#include "diagnostic_info.h"
#include "systemproxysettings.h"


//==== Globals ================================================================

#define MAX_LOADSTRING 100

HINSTANCE g_hInst;
TCHAR g_szTitle[MAX_LOADSTRING];
TCHAR g_szWindowClass[MAX_LOADSTRING];

HWND g_hWnd = NULL;
ConnectionManager g_connectionManager;

LimitSingleInstance g_singleInstanceObject(TEXT("Global\\{B88F6262-9CC8-44EF-887D-FB77DC89BB8C}"));

static HWND g_hHtmlCtrl = NULL;
static bool g_htmlUiReady = false;
// The HTML control has a bad habit of sending messages after we've posted WM_QUIT,
// which leads to a crash on exit. 
static bool g_htmlUiFinished = false;


//==== Controls ================================================================

static void OnResize(HWND hWnd, UINT uWidth, UINT uHeight)
{
    SetWindowPos(g_hHtmlCtrl, NULL, 0, 0, uWidth, uHeight, SWP_NOZORDER);
}

void OnCreate(HWND hWndParent)
{
    Json::Value initJSON, settingsJSON;
    Settings::ToJson(settingsJSON);
    initJSON["Settings"] = settingsJSON;
    initJSON["Cookies"] = Settings::GetCookies();
    initJSON["Config"] = Json::Value();
    initJSON["Config"]["Language"] = TStringToNarrow(GetLocaleName());
    initJSON["Config"]["Banner"] = string("banner.") + BANNER_FILETYPE;
    Json::FastWriter jsonWriter;
    tstring initJsonString = NarrowToTString(jsonWriter.write(initJSON));

    tstring url = ResourceToUrl(_T("main.html"), initJsonString.c_str(), NULL);

    /* Create the html control */
    g_hHtmlCtrl = CreateWindow(
        MC_WC_HTML, 
        url.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | 
            MC_HS_NOCONTEXTMENU |   // don't show context menu
            MC_HS_NOTIFYNAV,        // notify owner window on navigation attempts
        0, 0, 0, 0, 
        hWndParent, 
        (HMENU)IDC_HTML_CTRL,
        g_hInst,
        NULL);
}


//==== my_print (logging) =====================================================

vector<MessageHistoryEntry> g_messageHistory;
HANDLE g_messageHistoryMutex = CreateMutex(NULL, FALSE, 0);

void GetMessageHistory(vector<MessageHistoryEntry>& history)
{
    AutoMUTEX mutex(g_messageHistoryMutex);
    history = g_messageHistory;
}

void AddMessageEntryToHistory(
        LogSensitivity sensitivity, 
        bool bDebugMessage, 
        const TCHAR* formatString,
        const TCHAR* finalString)
{
    AutoMUTEX mutex(g_messageHistoryMutex);

    const TCHAR* historicalMessage = NULL;
    if (sensitivity == NOT_SENSITIVE)
    {
        historicalMessage = finalString;
    }
    else if (sensitivity == SENSITIVE_FORMAT_ARGS)
    {
        historicalMessage = formatString;
    }
    else // SENSITIVE_LOG
    {
        historicalMessage = NULL;
    }

    if (historicalMessage != NULL)
    {
        MessageHistoryEntry entry;
        entry.message = historicalMessage;
        entry.timestamp = GetISO8601DatetimeString();
        entry.debug = bDebugMessage;
        g_messageHistory.push_back(entry);
    }
}


#ifdef _DEBUG
bool g_bShowDebugMessages = true;
#else
bool g_bShowDebugMessages = false;
#endif

void my_print(LogSensitivity sensitivity, bool bDebugMessage, const TCHAR* format, ...)
{
    TCHAR* debugPrefix = _T("DEBUG: ");
    size_t debugPrefixLength = _tcsclen(debugPrefix);
    TCHAR* buffer = NULL;
    va_list args;
    va_start(args, format);
    int length = _vsctprintf(format, args) + 1;
    if (bDebugMessage)
    {
        length += debugPrefixLength;
    }
    buffer = (TCHAR*)malloc(length * sizeof(TCHAR));
    if (!buffer) return;
    if (bDebugMessage)
    {
        _tcscpy_s(buffer, length, debugPrefix);
        _vstprintf_s(buffer + debugPrefixLength, length - debugPrefixLength, format, args);
    }
    else
    {
        _vstprintf_s(buffer, length, format, args);
    }
    va_end(args);

    AddMessageEntryToHistory(sensitivity, bDebugMessage, format, buffer);

    if (!bDebugMessage || g_bShowDebugMessages)
    {
        // NOTE:
        // Main window handles displaying the message. This avoids
        // deadlocks with SendMessage. Main window will deallocate
        // buffer.

        PostMessage(g_hWnd, WM_PSIPHON_MY_PRINT, bDebugMessage ? 0 : 1, (LPARAM)buffer);
    }
}

void my_print(LogSensitivity sensitivity, bool bDebugMessage, const string& message)
{
    my_print(sensitivity, bDebugMessage, NarrowToTString(message).c_str());
}


//==== HTML UI helpers ========================================================

// Many of these helpers (particularly the ones that don't need an immediate 
// response from the page script) come in pairs: one function to receive the
// arguments, create a buffer, and post a message; and one function to receive
// the posted message and actually do the work. 
// We do this so that we won't end up deadlocked between message handling and
// background stuff. For example, the Stop button in the HTML will block the 
// page script until the AppLink is processed; but if ConnectionManager.Stop()
// is called directly, then it will wait for the connection thread to die, but
// that thread calls ConnectionManager.SetState(), which calls HtmlUI_SetState(),
// which tries to talk to the page script, but it can't, because the page script
// is blocked!
// So, we're going to PostMessages to ourself whenever possible.

#define WM_PSIPHON_HTMLUI_APPLINK      WM_USER + 200
#define WM_PSIPHON_HTMLUI_NAVLINK      WM_USER + 201
#define WM_PSIPHON_HTMLUI_SETSTATE     WM_USER + 202
#define WM_PSIPHON_HTMLUI_ADDMESSAGE   WM_USER + 203

static void HtmlUI_AddMessage(int priority, LPCTSTR message)
{
    Json::Value json;
    json["priority"] = priority;
    json["message"] = WStringToUTF8(message);
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());

    size_t bufLen = wJson.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, wJson.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_ADDMESSAGE, (WPARAM)buf, 0);
}

static void HtmlUI_AddMessageHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    (void)SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC, 
        (WPARAM)_T("HtmlCtrlInterface_AddMessage"), (LPARAM)&argStruct);
    delete[] json;
}

static void HtmlUI_SetState(const wstring& json)
{
    size_t bufLen = json.length() + 1;
    wchar_t* buf = new wchar_t[bufLen];
    wcsncpy_s(buf, bufLen, json.c_str(), bufLen);
    buf[bufLen - 1] = L'\0';
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_SETSTATE, (WPARAM)buf, 0);
}

static void HtmlUI_SetStateHandler(LPCWSTR json)
{
    if (!g_htmlUiReady)
    {
        delete[] json;
        return;
    }

    MC_HMCALLSCRIPTFUNC argStruct = { 0 };
    argStruct.cbSize = sizeof(MC_HMCALLSCRIPTFUNC);
    argStruct.cArgs = 1;
    argStruct.pszArg1 = json;
    (void)SendMessage(
        g_hHtmlCtrl, MC_HM_CALLSCRIPTFUNC, 
        (WPARAM)_T("HtmlCtrlInterface_SetState"), (LPARAM)&argStruct);
    delete[] json;
}

static void HtmlUI_AppLink(MC_NMHTMLURL* nmHtmlUrl)
{
    size_t bufLen = _tcslen(nmHtmlUrl->pszUrl) + 1;
    TCHAR* buf = new TCHAR[bufLen];
    _tcsncpy_s(buf, bufLen, nmHtmlUrl->pszUrl, bufLen);
    buf[bufLen - 1] = _T('\0');
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_APPLINK, (WPARAM)buf, 0);
}

static void HtmlUI_AppLinkHandler(LPCTSTR url)
{
    // NOTE: Incoming query parameters will be URI-encoded

    const LPCTSTR appStart = _T("app:start");
    const LPCTSTR appStop = _T("app:stop");
    const LPCTSTR appUpdateSettings = _T("app:updatesettings?");
    const size_t appUpdateSettingsLen = _tcslen(appUpdateSettings);
    const LPCTSTR appSendFeedback = _T("app:sendfeedback?");
    const size_t appSendFeedbackLen = _tcslen(appSendFeedback);
    const LPCTSTR appSetCookies = _T("app:setcookies?");
    const size_t appSetCookiesLen = _tcslen(appSetCookies);

    if (_tcscmp(url, appStart) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Start requested"), __TFUNCTION__);
        g_connectionManager.Start();
    }
    else if (_tcscmp(url, appStop) == 0)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Stop requested"), __TFUNCTION__);
        g_connectionManager.Stop(STOP_REASON_USER_DISCONNECT);
    }
    else if (_tcsncmp(url, appUpdateSettings, appUpdateSettingsLen) == 0
             && _tcslen(url) > appUpdateSettingsLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Update settings requested"), __TFUNCTION__);
        tstring uriEncoded(url + appUpdateSettingsLen);
        string stringJSON = UriDecode(TStringToNarrow(uriEncoded));
        bool settingsChanged = false;
        if (Settings::FromJson(stringJSON, settingsChanged) && settingsChanged
            && (g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_CONNECTED
                || g_connectionManager.GetState() == CONNECTION_MANAGER_STATE_STARTING))
        {
            my_print(NOT_SENSITIVE, false, _T("Settings change detected. Reconnecting."));
            g_connectionManager.Stop(STOP_REASON_USER_DISCONNECT);
            g_connectionManager.Start();
        }
    }
    else if (_tcsncmp(url, appSendFeedback, appSendFeedbackLen) == 0
        && _tcslen(url) > appSendFeedbackLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Send feedback requested"), __TFUNCTION__);
        tstring uriEncoded(url + appSendFeedbackLen);
        string stringJSON = UriDecode(TStringToNarrow(uriEncoded));
        my_print(NOT_SENSITIVE, false, _T("Sending feedback..."));
        g_connectionManager.SendFeedback(NarrowToTString(stringJSON).c_str());
    }
    else if (_tcsncmp(url, appSetCookies, appSetCookiesLen) == 0
        && _tcslen(url) > appSetCookiesLen)
    {
        my_print(NOT_SENSITIVE, true, _T("%s: Set cookies requested"), __TFUNCTION__);
        tstring uriEncoded(url + appSetCookiesLen);
        string stringJSON = UriDecode(TStringToNarrow(uriEncoded));
        Settings::SetCookies(stringJSON);
    }
    delete[] url;
}

static void HtmlUI_NavLink(MC_NMHTMLURL* nmHtmlUrl)
{
    size_t bufLen = _tcslen(nmHtmlUrl->pszUrl) + 1;
    TCHAR* buf = new TCHAR[bufLen];
    _tcsncpy_s(buf, bufLen, nmHtmlUrl->pszUrl, bufLen);
    buf[bufLen - 1] = _T('\0');
    PostMessage(g_hWnd, WM_PSIPHON_HTMLUI_NAVLINK, (WPARAM)buf, 0);
}

static void HtmlUI_NavLinkHandler(LPCTSTR url)
{
    OpenBrowser(url);
    delete[] url;
}

//==== Exported functions ========================================================

void UI_SetStateStopped()
{
    Json::Value json;
    json["state"] = "stopped";
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateStopping()
{
    Json::Value json;
    json["state"] = "stopping";
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateStarting(const tstring& transportProtocolName)
{
    Json::Value json;
    json["state"] = "starting";
    json["transport"] = WStringToUTF8(transportProtocolName.c_str());
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}

void UI_SetStateConnected(const tstring& transportProtocolName, int socksPort, int httpPort)
{
    Json::Value json;
    json["state"] = "connected";
    json["transport"] = WStringToUTF8(transportProtocolName.c_str());
    json["socksPort"] = socksPort;
    json["socksPortAuto"] = Settings::LocalSocksProxyPort() == 0;
    json["httpPort"] = httpPort;
    json["httpPortAuto"] = Settings::LocalHttpProxyPort() == 0;
    Json::FastWriter jsonWriter;
    wstring wJson = UTF8ToWString(jsonWriter.write(json).c_str());
    HtmlUI_SetState(wJson);
}


//==== Win32 boilerplate ======================================================

ATOM MyRegisterClass(HINSTANCE hInstance);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY _tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpCmdLine,
    int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    LoadString(hInstance, IDS_APP_TITLE, g_szTitle, MAX_LOADSTRING);
    LoadString(hInstance, IDC_PSICLIENT, g_szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    /* Register mCtrl and its HTML control. */
    mc_StaticLibInitialize();
    mcHtml_Initialize();

    // Perform application initialization

    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable;
    hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_PSICLIENT));

    // If this set of calls gets any longer, we may want to do something generic.
    DoStartupSystemProxyWork();
    DoStartupDiagnosticCollection();

    // Main message loop

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            // Bit of a dirty hack to prevent the HTML control code from crashing
            // on exit. WM_APP+2 is the message used for MC_HN_STATUSTEXT. 
            if (msg.message == (WM_APP+2) && g_htmlUiFinished)
                continue;

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    mcHtml_Terminate();
    mc_StaticLibTerminate();

    return (int) msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEX wcex = {0};

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PSICLIENT));
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszMenuName = 0;
    wcex.lpszClassName = g_szWindowClass;
    wcex.hIconSm = NULL;

    return RegisterClassEx(&wcex);
}


//==== Main window functions ==================================================

static bool g_documentCompleted = false;

static LRESULT HandleNotify(HWND hWnd, NMHDR* hdr)
{
    if (hdr->idFrom == IDC_HTML_CTRL) 
    {
        if (hdr->code == MC_HN_APPLINK)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
            HtmlUI_AppLink(nmHtmlUrl);
        }
        else if (hdr->code == MC_HN_NAVLINK)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
            // We should not interfere with the initial page load
            static bool s_firstNav = true;
            if (s_firstNav) {
                s_firstNav = false;
                return 0;
            }

            HtmlUI_NavLink(nmHtmlUrl);            
            return -1; // Prevent navigation
        }
        else if (hdr->code == MC_HN_DOCUMENTCOMPLETE)
        {
            // Note that this message may be received more than once.
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;

            // The UI is ready to function now.
            if (!g_documentCompleted)
            {
                g_documentCompleted = true;
                g_htmlUiReady = true;
                PostMessage(hWnd, WM_PSIPHON_CREATED, 0, 0);
            }
        }
        else if (hdr->code == MC_HN_PROGRESS)
        {
            MC_NMHTMLPROGRESS* nmHtmlProgress = (MC_NMHTMLPROGRESS*)hdr;
        }
        else if (hdr->code == MC_HN_STATUSTEXT)
        {
            MC_NMHTMLTEXT* nmHtmlText = (MC_NMHTMLTEXT*)hdr;
        }
        else if (hdr->code == MC_HN_TITLETEXT)
        {
            MC_NMHTMLTEXT* nmHtmlText = (MC_NMHTMLTEXT*)hdr;
        }
        else if (hdr->code == MC_HN_HISTORY)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
        }
        else if (hdr->code == MC_HN_NEWWINDOW)
        {
            MC_NMHTMLURL* nmHtmlUrl = (MC_NMHTMLURL*)hdr;
            // Prevent new window from opening
            return 0;
        }
        else if (hdr->code == MC_HN_HTTPERROR)
        {
            MC_NMHTTPERROR* nmHttpError = (MC_NMHTTPERROR*)hdr;
            assert(false);
            // Prevent HTTP error from being shown.
            return 0;
        }
        else
        {
            assert(false);
        }
    }

    return 0;
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    // Don't allow multiple instances of this application to run
    if (g_singleInstanceObject.IsAnotherInstanceRunning())
    {
        HWND otherWindow = FindWindow(g_szWindowClass, g_szTitle);
        if (otherWindow)
        {
            SetForegroundWindow(otherWindow);
            ShowWindow(otherWindow, SW_SHOW);
        }
        return FALSE;
    }

    g_hInst = hInstance;

    g_hWnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        g_szWindowClass,
        g_szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 
        780, 580,
        NULL, NULL, hInstance, NULL);

    // Don't show the window until the content loads.

    return TRUE;
}


LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        OnCreate(hWnd);
        break;

    case WM_PSIPHON_CREATED:
        // Display client version number 
        my_print(NOT_SENSITIVE, false, (tstring(_T("Client Version: ")) + NarrowToTString(CLIENT_VERSION)).c_str());

        // Content is loaded, so show the window.
        ShowWindow(g_hWnd, SW_SHOW);

        // Start a connection
        if (!Settings::SkipAutoConnect())
        {
            g_connectionManager.Toggle();
        }
        break;

    case WM_PSIPHON_HTMLUI_APPLINK:
        HtmlUI_AppLinkHandler((LPCTSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_NAVLINK:
        HtmlUI_NavLinkHandler((LPCTSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_SETSTATE:
        HtmlUI_SetStateHandler((LPCWSTR)wParam);
        break;
    case WM_PSIPHON_HTMLUI_ADDMESSAGE:
        HtmlUI_AddMessageHandler((LPCWSTR)wParam);
        break;

    case WM_SIZE:
        if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED)
        {
            OnResize(hWnd, LOWORD(lParam), HIWORD(lParam));
        }
        break;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 680;
        mmi->ptMinTrackSize.y = 410;
        break;
    }

    case WM_SETFOCUS:
        SetFocus(g_hHtmlCtrl);
        break;

    case WM_NOTIFY:
        return HandleNotify(hWnd, (NMHDR*)lParam);
        break;

    case WM_COMMAND:
        /*
        wmId = LOWORD(wParam);
        wmEvent = HIWORD(wParam);

        // lParam == 0: menu or accelerator event

        if (lParam == 0)
        {
            switch (wmId)
            {
            case IDM_SHOW_DEBUG_MESSAGES:
                g_bShowDebugMessages = !g_bShowDebugMessages;
                my_print(NOT_SENSITIVE, false, _T("Show debug messages: %s"), g_bShowDebugMessages ? _T("Yes") : _T("No"));
                break;
            // TODO: remove about, and exit?  The menu is currently hidden
            case IDM_ABOUT:
                DialogBox(g_hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        // lParam != 0: control notifications

        // Toggle button clicked

        else if (lParam == (LPARAM)g_hToggleButton && wmEvent == BN_CLICKED)
        {
            my_print(NOT_SENSITIVE, true, _T("%s: Button pressed, Toggle called"), __TFUNCTION__);

            // See comment below about Stop() blocking the UI
            SetCursor(LoadCursor(0, IDC_WAIT));

            g_connectionManager.Toggle();
        }

        // Banner clicked
        
        else if (lParam == (LPARAM)g_hBannerStatic && wmEvent == STN_CLICKED)
        {
            // If connected, sponsor open home pages, or info link if
            // no sponsor pages. If not connected, open info link.

            int state = g_connectionManager.GetState();
            if (CONNECTION_MANAGER_STATE_CONNECTED == state)
            {
                g_connectionManager.OpenHomePages(INFO_LINK_URL);
            }
            else
            {
                OpenBrowser(INFO_LINK_URL);
            }
        }

        // Info link clicked
        
        else if (lParam == (LPARAM)g_hInfoLinkStatic && wmEvent == STN_CLICKED)
        {
            // Info link static control was clicked, so open Psiphon 3 page
            // NOTE: Info link may be opened when not tunneled
            
            OpenBrowser(INFO_LINK_URL);
        }

        // Settings button clicked

        else if (lParam == (LPARAM)g_hSettingsButton && wmEvent == BN_CLICKED)
        {
            my_print(NOT_SENSITIVE, true, _T("%s: Button pressed, Settings called"), __TFUNCTION__);
            if (Settings::Show(g_hInst, hWnd))
            {
                // If the settings changed and we're connected, reconnect.
                ConnectionManagerState state = g_connectionManager.GetState();
                if (state == ConnectionManagerState::CONNECTION_MANAGER_STATE_CONNECTED
                    || state == ConnectionManagerState::CONNECTION_MANAGER_STATE_STARTING)
                {
                    g_connectionManager.Stop(STOP_REASON_USER_DISCONNECT);
                    g_connectionManager.Start();
                }
            }
        }

        // Feedback button clicked

        else if (lParam == (LPARAM)g_hFeedbackButton && wmEvent == BN_CLICKED)
        {
            my_print(NOT_SENSITIVE, true, _T("%s: Button pressed, Feedback called"), __TFUNCTION__);

            tstringstream feedbackArgs;
            feedbackArgs << "{ \"newVersionURL\": \"" << GET_NEW_VERSION_URL << "\", ";
            feedbackArgs << "\"newVersionEmail\": \"" << GET_NEW_VERSION_EMAIL << "\", ";
            feedbackArgs << "\"faqURL\": \"" << FAQ_URL << "\", ";
            feedbackArgs << "\"dataCollectionInfoURL\": \"" << DATA_COLLECTION_INFO_URL << "\" }";

            tstring feedbackResult;
            if (ShowHTMLDlg(
                    hWnd, 
                    _T("FEEDBACK_HTML_RESOURCE"), 
                    GetLocaleName().c_str(),
                    feedbackArgs.str().c_str(),
                    feedbackResult) == 1)
            {
                my_print(NOT_SENSITIVE, false, _T("Sending feedback..."));

                g_connectionManager.SendFeedback(feedbackResult.c_str());

                SendMessage(
                    g_hFeedbackButton,
                    BM_SETIMAGE,
                    IMAGE_ICON,
                    (LPARAM)g_hFeedbackButtonIcons[1]);
                EnableWindow(g_hFeedbackButton, FALSE);
            }
            // else error or user cancelled
        }
        */
        break;

    case WM_PSIPHON_MY_PRINT:
    {
        int priority = (int)wParam;
        TCHAR* message = (TCHAR*)lParam;
        HtmlUI_AddMessage(priority, message);
        OutputDebugString(message);
        OutputDebugString(L"\n");
        free(message);
        break;
    }

    case WM_PSIPHON_FEEDBACK_SUCCESS:
        /*
        SendMessage(
            g_hFeedbackButton,
            BM_SETIMAGE,
            IMAGE_ICON,
            (LPARAM)g_hFeedbackButtonIcons[0]);
        EnableWindow(g_hFeedbackButton, TRUE);
        my_print(NOT_SENSITIVE, false, _T("Feedback sent. Thank you!"));
        */
        break;

    case WM_PSIPHON_FEEDBACK_FAILED:
        /*
        SendMessage(
            g_hFeedbackButton,
            BM_SETIMAGE,
            IMAGE_ICON,
            (LPARAM)g_hFeedbackButtonIcons[0]);
        EnableWindow(g_hFeedbackButton, TRUE);
        my_print(NOT_SENSITIVE, false, _T("Failed to send feedback."));
        */
        break;

    case WM_ENDSESSION:
        // Stop the tunnel -- particularly to ensure system proxy settings are reverted -- on OS shutdown
        // Note: due to the following bug, the system proxy settings revert may silently fail:
        // https://connect.microsoft.com/IE/feedback/details/838086/internet-explorer-10-11-wininet-api-drops-proxy-change-events-during-system-shutdown
    case WM_DESTROY:
        // Stop transport if running
        g_connectionManager.Stop(STOP_REASON_EXIT);
        g_htmlUiFinished = true;
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}
