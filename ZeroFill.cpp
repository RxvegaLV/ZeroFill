#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kChunkSize = 1024u * 1024u;
constexpr int kWindowWidth = 540;
constexpr int kWindowHeight = 260;

constexpr int IDC_DRIVE_LABEL = 1001;
constexpr int IDC_DRIVE_COMBO = 1002;
constexpr int IDC_PROGRESS = 1003;
constexpr int IDC_STATUS = 1004;
constexpr int IDC_WRITE_BUTTON = 1005;
constexpr int IDC_REFRESH_BUTTON = 1006;

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT WM_APP_ERROR = WM_APP + 3;

struct DriveInfo {
    std::wstring rootPath;
    std::wstring displayText;
    ULONGLONG totalBytes = 0;
    ULONGLONG freeBytes = 0;
};

struct ProgressPayload {
    int percent = 0;
    std::wstring status;
};

std::wstring FormatSizeGiB(ULONGLONG bytes) {
    double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%.2f GB", gib);
    return buffer;
}

std::vector<DriveInfo> GetRemovableDrives() {
    std::vector<DriveInfo> drives;
    DWORD length = GetLogicalDriveStringsW(0, nullptr);
    if (length == 0) {
        return drives;
    }

    std::vector<wchar_t> buffer(length + 1, L'\0');
    if (GetLogicalDriveStringsW(length, buffer.data()) == 0) {
        return drives;
    }

    for (const wchar_t* ptr = buffer.data(); *ptr != L'\0'; ptr += wcslen(ptr) + 1) {
        std::wstring root(ptr);
        if (GetDriveTypeW(root.c_str()) != DRIVE_REMOVABLE) {
            continue;
        }

        ULONGLONG freeBytesAvailable = 0;
        ULONGLONG totalBytes = 0;
        ULONGLONG freeBytes = 0;
        if (!GetDiskFreeSpaceExW(root.c_str(), &freeBytesAvailable, &totalBytes, &freeBytes)) {
            totalBytes = 0;
            freeBytes = 0;
        }

        DriveInfo info;
        info.rootPath = root;
        info.totalBytes = totalBytes;
        info.freeBytes = freeBytes;
        info.displayText = root.substr(0, 2) + L" (" +
                           (totalBytes ? FormatSizeGiB(totalBytes) : std::wstring(L"Unknown")) + L")";
        drives.push_back(std::move(info));
    }

    return drives;
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    int Run() {
        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        const wchar_t kClassName[] = L"USBZeroWiperWindow";
        WNDCLASSW wc{};
        wc.lpfnWndProc = &App::WndProcSetup;
        wc.hInstance = instance_;
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

        RegisterClassW(&wc);

        hwnd_ = CreateWindowExW(
            0,
            kClassName,
            L"USB Zero Wiper",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kWindowWidth,
            kWindowHeight,
            nullptr,
            nullptr,
            instance_,
            this
        );

        if (!hwnd_) {
            return 1;
        }

        ShowWindow(hwnd_, SW_SHOWDEFAULT);
        UpdateWindow(hwnd_);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    static LRESULT CALLBACK WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::WndProcThunk));
            app->hwnd_ = hwnd;
            return app->WndProc(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return app ? app->WndProc(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_NCCREATE:
            return TRUE;
        case WM_CREATE:
            CreateUi(hwnd);
            RefreshDrives();
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam));
            return 0;
        case WM_APP_PROGRESS: {
            auto* payload = reinterpret_cast<ProgressPayload*>(lParam);
            if (payload) {
                SendMessageW(progressBar_, PBM_SETPOS, payload->percent, 0);
                SetWindowTextW(statusLabel_, payload->status.c_str());
                delete payload;
            }
            return 0;
        }
        case WM_APP_DONE:
            writing_ = false;
            EnableControls(true);
            SendMessageW(progressBar_, PBM_SETPOS, 100, 0);
            SetWindowTextW(statusLabel_, L"Completed");
            MessageBoxW(hwnd_, L"USB filled with zeroes successfully.", L"Done", MB_OK | MB_ICONINFORMATION);
            return 0;
        case WM_APP_ERROR: {
            writing_ = false;
            EnableControls(true);
            auto* message = reinterpret_cast<std::wstring*>(lParam);
            SetWindowTextW(statusLabel_, L"Error");
            MessageBoxW(hwnd_, message ? message->c_str() : L"An unknown error occurred.", L"Error", MB_OK | MB_ICONERROR);
            delete message;
            return 0;
        }
        case WM_CLOSE:
            if (writing_) {
                MessageBoxW(hwnd_, L"Wait for the zero-fill operation to finish before closing the app.", L"Busy", MB_OK | MB_ICONWARNING);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
    }

    void CreateUi(HWND hwnd) {
        CreateWindowW(L"STATIC", L"Select USB Drive", WS_CHILD | WS_VISIBLE,
                      20, 20, 200, 24, hwnd, reinterpret_cast<HMENU>(IDC_DRIVE_LABEL), instance_, nullptr);

        driveCombo_ = CreateWindowW(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
                                    20, 50, 360, 400, hwnd, reinterpret_cast<HMENU>(IDC_DRIVE_COMBO), instance_, nullptr);

        refreshButton_ = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                       395, 49, 100, 26, hwnd, reinterpret_cast<HMENU>(IDC_REFRESH_BUTTON), instance_, nullptr);

        progressBar_ = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                     20, 100, 475, 24, hwnd, reinterpret_cast<HMENU>(IDC_PROGRESS), instance_, nullptr);
        SendMessageW(progressBar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        statusLabel_ = CreateWindowW(L"STATIC", L"Idle", WS_CHILD | WS_VISIBLE,
                                     20, 135, 475, 24, hwnd, reinterpret_cast<HMENU>(IDC_STATUS), instance_, nullptr);

        writeButton_ = CreateWindowW(L"BUTTON", L"WRITE ZEROES", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     175, 175, 160, 34, hwnd, reinterpret_cast<HMENU>(IDC_WRITE_BUTTON), instance_, nullptr);
    }

    void HandleCommand(int controlId) {
        switch (controlId) {
        case IDC_REFRESH_BUTTON:
            if (!writing_) {
                RefreshDrives();
            }
            break;
        case IDC_WRITE_BUTTON:
            if (!writing_) {
                ConfirmAndStart();
            }
            break;
        default:
            break;
        }
    }

    void RefreshDrives() {
        drives_ = GetRemovableDrives();
        SendMessageW(driveCombo_, CB_RESETCONTENT, 0, 0);

        for (const auto& drive : drives_) {
            SendMessageW(driveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(drive.displayText.c_str()));
        }

        if (!drives_.empty()) {
            SendMessageW(driveCombo_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(statusLabel_, L"Idle");
        } else {
            SetWindowTextW(statusLabel_, L"No removable drives found");
        }

        SendMessageW(progressBar_, PBM_SETPOS, 0, 0);
    }

    void ConfirmAndStart() {
        LRESULT selected = SendMessageW(driveCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR || selected < 0 || static_cast<std::size_t>(selected) >= drives_.size()) {
            MessageBoxW(hwnd_, L"Select a USB drive first.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }

        const int answer = MessageBoxW(
            hwnd_,
            L"THIS WILL FILL THE USB WITH ZEROES.\n\nContinue?",
            L"WARNING",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2
        );

        if (answer != IDYES) {
            return;
        }

        const DriveInfo drive = drives_[static_cast<std::size_t>(selected)];
        writing_ = true;
        EnableControls(false);
        SendMessageW(progressBar_, PBM_SETPOS, 0, 0);
        SetWindowTextW(statusLabel_, (L"Writing to " + drive.rootPath + L"ZERO_FILL.bin").c_str());

        std::thread([this, drive]() {
            WipeDrive(drive);
        }).detach();
    }

    void EnableControls(bool enabled) {
        EnableWindow(driveCombo_, enabled);
        EnableWindow(refreshButton_, enabled);
        EnableWindow(writeButton_, enabled);
    }

    void WipeDrive(const DriveInfo& drive) {
        const std::wstring filePath = drive.rootPath + L"ZERO_FILL.bin";
        HANDLE file = CreateFileW(
            filePath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (file == INVALID_HANDLE_VALUE) {
            PostError(L"Could not create the output file. Try running as administrator or verify the drive is writable.");
            return;
        }

        std::vector<std::uint8_t> chunk(kChunkSize, 0);
        ULONGLONG freeBytesAvailable = 0;
        ULONGLONG totalBytes = 0;
        ULONGLONG freeBytes = 0;
        if (!GetDiskFreeSpaceExW(drive.rootPath.c_str(), &freeBytesAvailable, &totalBytes, &freeBytes)) {
            CloseHandle(file);
            DeleteFileW(filePath.c_str());
            PostError(L"Could not read free space information for the selected drive.");
            return;
        }

        if (freeBytes <= kChunkSize) {
            CloseHandle(file);
            DeleteFileW(filePath.c_str());
            PostError(L"Not enough free space to write a zero-fill file.");
            return;
        }

        ULONGLONG targetBytes = freeBytes - kChunkSize;
        ULONGLONG written = 0;

        while (written < targetBytes) {
            DWORD toWrite = kChunkSize;
            if (targetBytes - written < kChunkSize) {
                toWrite = static_cast<DWORD>(targetBytes - written);
            }

            DWORD actualWritten = 0;
            if (!WriteFile(file, chunk.data(), toWrite, &actualWritten, nullptr)) {
                CloseHandle(file);
                PostError(L"Write failed while filling the drive with zeroes.");
                return;
            }

            written += actualWritten;
            int percent = static_cast<int>((written * 100ULL) / freeBytes);
            auto* payload = new ProgressPayload{
                percent,
                L"Writing zeroes... " + std::to_wstring(percent) + L"%"
            };
            PostMessageW(hwnd_, WM_APP_PROGRESS, 0, reinterpret_cast<LPARAM>(payload));
        }

        FlushFileBuffers(file);
        CloseHandle(file);
        PostMessageW(hwnd_, WM_APP_DONE, 0, 0);
    }

    void PostError(const std::wstring& message) {
        auto* heapMessage = new std::wstring(message);
        PostMessageW(hwnd_, WM_APP_ERROR, 0, reinterpret_cast<LPARAM>(heapMessage));
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND driveCombo_ = nullptr;
    HWND progressBar_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND writeButton_ = nullptr;
    HWND refreshButton_ = nullptr;
    std::atomic<bool> writing_ = false;
    std::vector<DriveInfo> drives_;
};

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    App app(instance);
    return app.Run();
}
