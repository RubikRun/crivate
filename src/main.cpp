#include "cri.h"
#include "folder.h"
#include "image_load.h"
#include "viewer.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

#include <windows.h>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

constexpr DWORD kPasswordWideCap = 1024;
constexpr size_t kPasswordUtf8Cap = 4096;

struct SecretBuf {
    std::vector<uint8_t> bytes;

    SecretBuf() = default;
    SecretBuf(const SecretBuf&) = delete;
    SecretBuf& operator=(const SecretBuf&) = delete;

    ~SecretBuf() { wipe(); }

    void wipe() {
        if (!bytes.empty()) {
            crypto_wipe(bytes.data(), bytes.size());
            bytes.clear();
        }
    }
};

enum class ReadPasswordResult {
    Ok,
    Empty,
    TooLong,
    IoError,
};

struct ConsoleEchoRestorer {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD mode = 0;
    bool active = false;

    void arm(HANDLE h, DWORD original_mode) {
        handle = h;
        mode = original_mode;
        active = true;
    }

    void restore() {
        if (active) {
            SetConsoleMode(handle, mode);
            active = false;
        }
    }

    ~ConsoleEchoRestorer() { restore(); }
};

void print_usage() {
    fprintf(stderr,
            "crivate init\n"
            "crivate add <image-path>\n"
            "crivate count\n"
            "crivate view <index>\n"
            "crivate del <index>\n");
}

void warn_if_exe_in_cwd() {
    if (folder_exe_in_cwd()) {
        fprintf(stderr,
                "WARNING: The file crivate.exe should not live in a crivate folder.\n");
    }
}

bool passwords_equal(const SecretBuf& a, const SecretBuf& b) {
    if (a.bytes.size() != b.bytes.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.bytes.size(); ++i) {
        diff |= static_cast<unsigned char>(a.bytes[i] ^ b.bytes[i]);
    }
    return diff == 0;
}

bool drain_console_line(HANDLE h_in) {
    wchar_t extra[256];
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleW(h_in, extra, 255, &n, nullptr) || n == 0) {
            SecureZeroMemory(extra, sizeof(extra));
            return false;
        }
        const bool done = extra[n - 1] == L'\n' || extra[n - 1] == L'\r';
        SecureZeroMemory(extra, sizeof(extra));
        if (done) {
            return true;
        }
    }
}

bool drain_pipe_line(HANDLE h_in) {
    unsigned char c = 0;
    DWORD n = 0;
    while (ReadFile(h_in, &c, 1, &n, nullptr) && n == 1) {
        if (c == '\n') {
            return true;
        }
    }
    return true;
}

ReadPasswordResult utf16_to_secret(wchar_t* wbuf, DWORD nchars, SecretBuf* out) {
    while (nchars > 0 && (wbuf[nchars - 1] == L'\n' || wbuf[nchars - 1] == L'\r')) {
        --nchars;
    }

    if (nchars == 0) {
        out->bytes.clear();
        return ReadPasswordResult::Empty;
    }
    if (nchars > kPasswordWideCap) {
        return ReadPasswordResult::TooLong;
    }

    const int nbytes = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(nchars), nullptr, 0,
                                           nullptr, nullptr);
    if (nbytes <= 0) {
        return ReadPasswordResult::IoError;
    }
    if (static_cast<size_t>(nbytes) > kPasswordUtf8Cap) {
        return ReadPasswordResult::TooLong;
    }

    out->bytes.resize(static_cast<size_t>(nbytes));
    if (WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(nchars),
                            reinterpret_cast<char*>(out->bytes.data()), nbytes, nullptr,
                            nullptr) != nbytes) {
        out->wipe();
        return ReadPasswordResult::IoError;
    }
    return ReadPasswordResult::Ok;
}

ReadPasswordResult read_secret_line(SecretBuf* out) {
    out->wipe();

    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in == nullptr || h_in == INVALID_HANDLE_VALUE) {
        return ReadPasswordResult::IoError;
    }

    DWORD orig_mode = 0;
    const bool is_console = GetConsoleMode(h_in, &orig_mode) != 0;
    ConsoleEchoRestorer echo;

    if (is_console) {
        const DWORD no_echo = orig_mode & ~ENABLE_ECHO_INPUT;
        if (!SetConsoleMode(h_in, no_echo)) {
            return ReadPasswordResult::IoError;
        }
        echo.arm(h_in, orig_mode);

        wchar_t wbuf[kPasswordWideCap + 2];
        DWORD nchars = 0;
        const DWORD wbuf_n = static_cast<DWORD>(sizeof(wbuf) / sizeof(wbuf[0]));
        const BOOL ok = ReadConsoleW(h_in, wbuf, wbuf_n, &nchars, nullptr);
        if (!ok) {
            SecureZeroMemory(wbuf, sizeof(wbuf));
            echo.restore();
            fputc('\n', stderr);
            return ReadPasswordResult::IoError;
        }

        const bool got_eol =
            nchars > 0 && (wbuf[nchars - 1] == L'\n' || wbuf[nchars - 1] == L'\r');
        if (!got_eol && nchars == wbuf_n) {
            drain_console_line(h_in);
            SecureZeroMemory(wbuf, sizeof(wbuf));
            echo.restore();
            fputc('\n', stderr);
            return ReadPasswordResult::TooLong;
        }

        const ReadPasswordResult result = utf16_to_secret(wbuf, nchars, out);
        SecureZeroMemory(wbuf, sizeof(wbuf));
        echo.restore();
        fputc('\n', stderr);
        return result;
    }

    std::vector<uint8_t> raw;
    raw.reserve(64);
    for (;;) {
        unsigned char c = 0;
        DWORD n = 0;
        if (!ReadFile(h_in, &c, 1, &n, nullptr)) {
            crypto_wipe(raw.data(), raw.size());
            fputc('\n', stderr);
            return ReadPasswordResult::IoError;
        }
        if (n == 0) {
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (raw.size() >= kPasswordUtf8Cap) {
            drain_pipe_line(h_in);
            crypto_wipe(raw.data(), raw.size());
            fputc('\n', stderr);
            return ReadPasswordResult::TooLong;
        }
        raw.push_back(c);
    }

    fputc('\n', stderr);
    if (raw.empty()) {
        return ReadPasswordResult::Empty;
    }
    out->bytes.swap(raw);
    return ReadPasswordResult::Ok;
}

ReadPasswordResult prompt_password(const char* label, SecretBuf* out) {
    fprintf(stderr, "%s", label);
    fflush(stderr);
    return read_secret_line(out);
}

int report_password_read(ReadPasswordResult result) {
    switch (result) {
        case ReadPasswordResult::Ok:
            return kExitOk;
        case ReadPasswordResult::Empty:
            fprintf(stderr, "ERROR: The password cannot be empty.\n");
            return kExitError;
        case ReadPasswordResult::TooLong:
            fprintf(stderr, "ERROR: The password is too long.\n");
            return kExitError;
        case ReadPasswordResult::IoError:
        default:
            fprintf(stderr, "ERROR: Failed to read the password.\n");
            return kExitError;
    }
}

int require_folder() {
    if (!folder_is_initialized()) {
        fprintf(stderr, "ERROR: This is not a crivate folder.\n");
        return kExitError;
    }
    return kExitOk;
}

int prompt_unlock(AesKey* key) {
    SecretBuf password;
    const int read_status = report_password_read(prompt_password("Password: ", &password));
    if (read_status != kExitOk) {
        return read_status;
    }

    const bool ok = folder_unlock(password.bytes.data(), password.bytes.size(), key);
    password.wipe();
    if (!ok) {
        crypto_wipe_key(key);
        fprintf(stderr, "ERROR: Wrong password or corrupted file.\n");
        return kExitError;
    }
    return kExitOk;
}

int cmd_init() {
    if (folder_marker_exists()) {
        fprintf(stderr, "ERROR: Folder is already initialized. A .crivate file exists.\n");
        return kExitError;
    }

    DirState dir = folder_dir_state();
    if (dir == DirState::Error) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }
    if (dir == DirState::NotEmpty) {
        fprintf(stderr, "ERROR: The directory is not empty.\n");
        return kExitError;
    }

    SecretBuf password;
    SecretBuf confirm;
    const int pw_status = report_password_read(prompt_password("Password: ", &password));
    if (pw_status != kExitOk) {
        return pw_status;
    }
    const int confirm_status =
        report_password_read(prompt_password("Confirm password: ", &confirm));
    if (confirm_status != kExitOk) {
        return confirm_status;
    }
    if (!passwords_equal(password, confirm)) {
        fprintf(stderr, "ERROR: The passwords do not match.\n");
        return kExitError;
    }
    confirm.wipe();

    if (folder_marker_exists()) {
        fprintf(stderr, "ERROR: Folder is already initialized. A .crivate file exists.\n");
        return kExitError;
    }
    dir = folder_dir_state();
    if (dir == DirState::Error) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }
    if (dir == DirState::NotEmpty) {
        fprintf(stderr, "ERROR: The directory is not empty.\n");
        return kExitError;
    }

    const bool ok = folder_init(password.bytes.data(), password.bytes.size());
    password.wipe();
    if (!ok) {
        fprintf(stderr, "ERROR: Failed to create the .crivate file.\n");
        return kExitError;
    }
    return kExitOk;
}

int cmd_add(const wchar_t* image_path) {
    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    std::wstring dest;
    if (!cri_dest_name(image_path, &dest)) {
        fprintf(stderr, "ERROR: The image path is invalid.\n");
        return kExitError;
    }
    if (cri_exists(dest.c_str())) {
        fprintf(stderr, "ERROR: A .cri file with this name already exists.\n");
        return kExitError;
    }

    const DWORD src_attr = GetFileAttributesW(image_path);
    if (src_attr == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "ERROR: The image file was not found.\n");
        return kExitError;
    }
    if ((src_attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        fprintf(stderr, "ERROR: The file is not an image that can be decoded.\n");
        return kExitError;
    }

    AesKey key{};
    const int unlock_status = prompt_unlock(&key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }

    RgbImage image;
    const ImageLoadStatus load = image_load_rgb(image_path, &image);
    if (load != ImageLoadStatus::Ok) {
        crypto_wipe_key(&key);
        switch (load) {
            case ImageLoadStatus::NotFound:
                fprintf(stderr, "ERROR: The image file was not found.\n");
                break;
            case ImageLoadStatus::TooLarge:
                fprintf(stderr, "ERROR: The image is too large.\n");
                break;
            case ImageLoadStatus::NotAnImage:
                fprintf(stderr, "ERROR: The file is not an image that can be decoded.\n");
                break;
            case ImageLoadStatus::IoError:
            default:
                fprintf(stderr, "ERROR: Failed to read the image.\n");
                break;
        }
        return kExitError;
    }

    const CriWriteStatus written =
        cri_write(dest.c_str(), key, image.width, image.height, image.rgb.data(), image.rgb.size());
    image.wipe();
    crypto_wipe_key(&key);

    switch (written) {
        case CriWriteStatus::Ok:
            fprintf(stderr, "INFO: Image was added successfully under %ls.\n", dest.c_str());
            return kExitOk;
        case CriWriteStatus::Exists:
            fprintf(stderr, "ERROR: A .cri file with this name already exists.\n");
            return kExitError;
        default:
            fprintf(stderr, "ERROR: Failed to add the image.\n");
            return kExitError;
    }
}

int cmd_count() {
    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    AesKey key{};
    const int unlock_status = prompt_unlock(&key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }
    crypto_wipe_key(&key);

    std::vector<std::wstring> names;
    if (!folder_list_cri(&names)) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }

    fprintf(stdout, "%zu\n", names.size());
    return kExitOk;
}

bool parse_index(const wchar_t* s, uint32_t* out) {
    if (s == nullptr || s[0] == L'\0' || out == nullptr) {
        return false;
    }

    uint32_t value = 0;
    for (const wchar_t* p = s; *p != L'\0'; ++p) {
        if (*p < L'0' || *p > L'9') {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(*p - L'0');
        if (value > ((std::numeric_limits<uint32_t>::max)() - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

void wipe_rgb(std::vector<uint8_t>* rgb) {
    if (rgb == nullptr || rgb->empty()) {
        return;
    }
    crypto_wipe(rgb->data(), rgb->size());
    rgb->clear();
    rgb->shrink_to_fit();
}

int cmd_view(const wchar_t* index_arg) {
    uint32_t index = 0;
    if (!parse_index(index_arg, &index)) {
        print_usage();
        return kExitUsage;
    }

    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    AesKey key{};
    const int unlock_status = prompt_unlock(&key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }

    std::vector<std::wstring> names;
    if (!folder_list_cri(&names)) {
        crypto_wipe_key(&key);
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }
    if (names.empty()) {
        crypto_wipe_key(&key);
        fprintf(stderr, "ERROR: There are no images in the folder.\n");
        return kExitError;
    }
    if (index < 1 || static_cast<size_t>(index) > names.size()) {
        crypto_wipe_key(&key);
        fprintf(stderr, "ERROR: The image index is out of range.\n");
        return kExitError;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
    const CriReadStatus read = cri_read(names[index - 1].c_str(), key, &width, &height, &rgb);
    crypto_wipe_key(&key);

    if (read != CriReadStatus::Ok) {
        wipe_rgb(&rgb);
        switch (read) {
            case CriReadStatus::AuthFailed:
                fprintf(stderr, "ERROR: Wrong password or corrupted file.\n");
                break;
            case CriReadStatus::OutOfMemory:
                fprintf(stderr, "ERROR: The image is too large.\n");
                break;
            case CriReadStatus::InvalidFile:
                fprintf(stderr, "ERROR: The image file is invalid.\n");
                break;
            default:
                fprintf(stderr, "ERROR: Failed to read the image.\n");
                break;
        }
        return kExitError;
    }

    const ViewerStatus shown = viewer_show(width, height, rgb.data(), rgb.size());
    wipe_rgb(&rgb);

    if (shown != ViewerStatus::Ok) {
        fprintf(stderr, "ERROR: Failed to open the viewer.\n");
        return kExitError;
    }
    return kExitOk;
}

int cmd_not_implemented() {
    fprintf(stderr, "ERROR: This command is not implemented yet.\n");
    return kExitError;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    warn_if_exe_in_cwd();

    if (argc < 2) {
        print_usage();
        return kExitUsage;
    }

    const wchar_t* cmd = argv[1];

    if (wcscmp(cmd, L"init") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_init();
    }

    if (wcscmp(cmd, L"add") == 0) {
        if (argc != 3) {
            print_usage();
            return kExitUsage;
        }
        return cmd_add(argv[2]);
    }

    if (wcscmp(cmd, L"count") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_count();
    }

    if (wcscmp(cmd, L"view") == 0) {
        if (argc != 3) {
            print_usage();
            return kExitUsage;
        }
        return cmd_view(argv[2]);
    }

    if (wcscmp(cmd, L"del") == 0) {
        if (argc != 3) {
            print_usage();
            return kExitUsage;
        }
        return cmd_not_implemented();
    }

    print_usage();
    return kExitUsage;
}
