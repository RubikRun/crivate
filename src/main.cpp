#include "cri.h"
#include "folder.h"
#include "image_load.h"
#include "viewer.h"

#include <algorithm>
#include <cstdint>
#include <new>
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
            "crivate add <image-path> [image-path ...]\n"
            "crivate count\n"
            "crivate list\n"
            "crivate view\n"
            "crivate view <index>\n"
            "crivate view <first> <last>\n"
            "crivate del\n"
            "crivate del <index>\n"
            "crivate del <first> <last>\n"
            "crivate password\n"
            "\n"
            "Indices are 1-based from the sorted .cri names in this folder.\n"
            "Adding or deleting an image can change later indices.\n"
            "A directory passed to add contributes the image files directly inside it, "
            "not nested folders.\n");
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
        fprintf(stderr, "ERROR: Wrong password.\n");
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

struct FindHandle {
    HANDLE h = INVALID_HANDLE_VALUE;

    explicit FindHandle(HANDLE handle) : h(handle) {}
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;

    ~FindHandle() {
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            h = INVALID_HANDLE_VALUE;
        }
    }

    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

const wchar_t* path_file_name(const wchar_t* path) {
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }
    return base;
}

bool is_image_file_name(const wchar_t* name) {
    if (name == nullptr || name[0] == L'\0') {
        return false;
    }
    const wchar_t* dot = wcsrchr(name, L'.');
    if (dot == nullptr || dot[1] == L'\0') {
        return false;
    }

    static const wchar_t* kExtensions[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".webp",
    };
    for (const wchar_t* ext : kExtensions) {
        if (CompareStringOrdinal(dot, -1, ext, -1, TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

bool image_file_less(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(path_file_name(a.c_str()), -1, path_file_name(b.c_str()), -1,
                                TRUE) == CSTR_LESS_THAN;
}

// False means this file had a hard failure. A skipped existing dest is success.
bool add_one_image(const wchar_t* image_path, const AesKey& key) {
    std::wstring dest;
    if (!cri_dest_name(image_path, &dest)) {
        fprintf(stderr, "ERROR: The image path is invalid: %ls.\n", image_path);
        return false;
    }
    if (cri_exists(dest.c_str())) {
        fprintf(stderr, "WARNING: Skipping %ls. A .cri file named %ls already exists.\n",
                image_path, dest.c_str());
        return true;
    }

    RgbImage image;
    const ImageLoadStatus load = image_load_rgb(image_path, &image);
    if (load != ImageLoadStatus::Ok) {
        switch (load) {
            case ImageLoadStatus::NotFound:
                fprintf(stderr, "ERROR: The image file was not found: %ls.\n", image_path);
                break;
            case ImageLoadStatus::TooLarge:
                fprintf(stderr, "ERROR: The image is too large: %ls.\n", image_path);
                break;
            case ImageLoadStatus::NotAnImage:
                fprintf(stderr, "ERROR: The file is not an image that can be decoded: %ls.\n",
                        image_path);
                break;
            case ImageLoadStatus::IoError:
            default:
                fprintf(stderr, "ERROR: Failed to read the image: %ls.\n", image_path);
                break;
        }
        return false;
    }

    const CriWriteStatus written =
        cri_write(dest.c_str(), key, image.width, image.height, image.rgb.data(), image.rgb.size());
    image.wipe();

    switch (written) {
        case CriWriteStatus::Ok:
            fprintf(stderr, "INFO: Image was added successfully under %ls.\n", dest.c_str());
            return true;
        case CriWriteStatus::Exists:
            fprintf(stderr, "WARNING: Skipping %ls. A .cri file named %ls already exists.\n",
                    image_path, dest.c_str());
            return true;
        default:
            fprintf(stderr, "ERROR: Failed to add the image: %ls.\n", image_path);
            return false;
    }
}

// False means this directory argument had a hard failure.
bool add_directory(const wchar_t* dir, const AesKey& key) {
    std::wstring prefix(dir);
    if (!prefix.empty() && prefix.back() != L'\\' && prefix.back() != L'/') {
        prefix.push_back(L'\\');
    }
    const std::wstring pattern = prefix + L"*";

    WIN32_FIND_DATAW fd{};
    FindHandle find(FindFirstFileW(pattern.c_str(), &fd));
    if (!find.valid()) {
        fprintf(stderr, "ERROR: Failed to list the directory: %ls.\n", dir);
        return false;
    }

    std::vector<std::wstring> images;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (!is_image_file_name(fd.cFileName)) {
            continue;
        }
        images.push_back(prefix + fd.cFileName);
    } while (FindNextFileW(find.h, &fd));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        fprintf(stderr, "ERROR: Failed to list the directory: %ls.\n", dir);
        return false;
    }

    if (images.empty()) {
        fprintf(stderr, "WARNING: %ls contains no image files.\n", dir);
        return true;
    }

    std::sort(images.begin(), images.end(), image_file_less);

    bool ok = true;
    for (const std::wstring& path : images) {
        if (!add_one_image(path.c_str(), key)) {
            ok = false;
        }
    }
    return ok;
}

// False means this argument had a hard failure. Skipped files are not failures.
bool add_argument(const wchar_t* path, const AesKey& key) {
    if (path == nullptr || path[0] == L'\0') {
        fprintf(stderr, "ERROR: The image path is invalid.\n");
        return false;
    }

    const DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            fprintf(stderr, "ERROR: The path was not found: %ls.\n", path);
        } else if (err == ERROR_INVALID_NAME) {
            fprintf(stderr, "ERROR: The image path is invalid: %ls.\n", path);
        } else {
            fprintf(stderr, "ERROR: Failed to read the path: %ls.\n", path);
        }
        return false;
    }

    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return add_directory(path, key);
    }
    return add_one_image(path, key);
}

struct KeyGuard {
    AesKey* key = nullptr;

    ~KeyGuard() { crypto_wipe_key(key); }
};

int cmd_add(int path_count, wchar_t** paths) {
    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    AesKey key{};
    KeyGuard key_guard{&key};
    const int unlock_status = prompt_unlock(&key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }

    bool hard_failure = false;
    for (int i = 0; i < path_count; ++i) {
        if (!add_argument(paths[i], key)) {
            hard_failure = true;
        }
    }
    crypto_wipe_key(&key);
    return hard_failure ? kExitError : kExitOk;
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

    fprintf(stdout, "Number of images in crivate folder:\n%zu\n", names.size());
    return kExitOk;
}

int cmd_list() {
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

    if (names.empty()) {
        fprintf(stdout, "There are no images in the folder.\n");
        return kExitOk;
    }

    for (size_t i = 0; i < names.size(); ++i) {
        fprintf(stdout, "%zu  %ls\n", i + 1, names[i].c_str());
    }
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

struct RgbGuard {
    std::vector<uint8_t>* rgb = nullptr;

    ~RgbGuard() { wipe_rgb(rgb); }
};

void print_cri_read_error(CriReadStatus status) {
    switch (status) {
        case CriReadStatus::AuthFailed:
            fprintf(stderr, "ERROR: The image file is corrupted.\n");
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
}

struct ViewSession {
    AesKey* key = nullptr;
    std::vector<std::wstring> set;
    size_t index = 0;
    std::vector<uint8_t> rgb;
};

bool view_load_delta(void* user, int delta, uint32_t* width, uint32_t* height,
                     const uint8_t** rgb_out, size_t* rgb_len) {
    auto* session = static_cast<ViewSession*>(user);
    if (session == nullptr || session->key == nullptr || width == nullptr || height == nullptr ||
        rgb_out == nullptr || rgb_len == nullptr) {
        return false;
    }

    const size_t n = session->set.size();
    if (n <= 1) {
        return false;
    }

    size_t next = session->index;
    if (delta > 0) {
        next = (session->index + 1) % n;
    } else if (delta < 0) {
        next = (session->index + n - 1) % n;
    } else {
        return false;
    }

    uint32_t loaded_w = 0;
    uint32_t loaded_h = 0;
    std::vector<uint8_t> fresh;
    RgbGuard fresh_guard{&fresh};
    const CriReadStatus read =
        cri_read(session->set[next].c_str(), *session->key, &loaded_w, &loaded_h, &fresh);
    if (read != CriReadStatus::Ok) {
        print_cri_read_error(read);
        return false;
    }

    wipe_rgb(&session->rgb);
    session->rgb.swap(fresh);
    session->index = next;
    *width = loaded_w;
    *height = loaded_h;
    *rgb_out = session->rgb.data();
    *rgb_len = session->rgb.size();
    return true;
}

int cmd_view(int index_count, wchar_t** index_args) {
    if (index_count < 0 || index_count > 2) {
        print_usage();
        return kExitUsage;
    }

    uint32_t first = 0;
    uint32_t last = 0;
    if (index_count >= 1 && !parse_index(index_args[0], &first)) {
        print_usage();
        return kExitUsage;
    }
    if (index_count == 2 && !parse_index(index_args[1], &last)) {
        print_usage();
        return kExitUsage;
    }

    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    AesKey key{};
    KeyGuard key_guard{&key};
    const int unlock_status = prompt_unlock(&key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }

    std::vector<std::wstring> names;
    if (!folder_list_cri(&names)) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }
    if (names.empty()) {
        fprintf(stderr, "ERROR: There are no images in the folder.\n");
        return kExitError;
    }
    if (names.size() > (std::numeric_limits<uint32_t>::max)()) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }

    uint32_t begin = 1;
    uint32_t end = static_cast<uint32_t>(names.size());
    if (index_count >= 1) {
        begin = first;
        end = index_count == 2 ? last : first;
    }
    if (begin < 1 || end < 1 || static_cast<size_t>(begin) > names.size() ||
        static_cast<size_t>(end) > names.size()) {
        fprintf(stderr, "ERROR: The image index is out of range.\n");
        return kExitError;
    }
    if (begin > end) {
        fprintf(stderr, "ERROR: The first index is greater than the last index.\n");
        return kExitError;
    }

    ViewSession session;
    RgbGuard rgb_guard{&session.rgb};
    session.key = &key;
    session.index = 0;
    session.set.reserve(static_cast<size_t>(end) - static_cast<size_t>(begin) + 1);
    for (size_t i = begin; i <= end; ++i) {
        session.set.push_back(std::move(names[i - 1]));
    }

    uint32_t width = 0;
    uint32_t height = 0;
    const CriReadStatus read =
        cri_read(session.set[0].c_str(), key, &width, &height, &session.rgb);
    if (read != CriReadStatus::Ok) {
        print_cri_read_error(read);
        return kExitError;
    }

    const ViewerStatus shown = viewer_show(width, height, session.rgb.data(), session.rgb.size(),
                                           view_load_delta, &session);

    if (shown == ViewerStatus::Ok) {
        return kExitOk;
    }
    if (shown == ViewerStatus::OutOfMemory) {
        fprintf(stderr, "ERROR: Not enough memory to display the image.\n");
        return kExitError;
    }
    fprintf(stderr, "ERROR: Failed to open the viewer.\n");
    return kExitError;
}

enum class ConfirmResult {
    Yes,
    No,
    IoError,
};

bool wide_line_is_yes(const wchar_t* line, size_t n) {
    return n == 3 && line[0] == L'Y' && line[1] == L'E' && line[2] == L'S';
}

ConfirmResult read_console_confirm(HANDLE h_in) {
    wchar_t buf[256];
    DWORD nchars = 0;
    const DWORD buf_n = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    if (!ReadConsoleW(h_in, buf, buf_n, &nchars, nullptr)) {
        SecureZeroMemory(buf, sizeof(buf));
        return ConfirmResult::IoError;
    }

    const bool got_eol = nchars > 0 && (buf[nchars - 1] == L'\n' || buf[nchars - 1] == L'\r');
    if (!got_eol && nchars == buf_n) {
        drain_console_line(h_in);
        SecureZeroMemory(buf, sizeof(buf));
        return ConfirmResult::No;
    }

    while (nchars > 0 && (buf[nchars - 1] == L'\n' || buf[nchars - 1] == L'\r')) {
        --nchars;
    }
    const bool yes = wide_line_is_yes(buf, nchars);
    SecureZeroMemory(buf, sizeof(buf));
    return yes ? ConfirmResult::Yes : ConfirmResult::No;
}

ConfirmResult read_pipe_confirm(HANDLE h_in) {
    char line[3]{};
    size_t n = 0;
    bool too_long = false;
    for (;;) {
        unsigned char c = 0;
        DWORD got = 0;
        if (!ReadFile(h_in, &c, 1, &got, nullptr)) {
            return ConfirmResult::IoError;
        }
        if (got == 0 || c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (n < 3) {
            line[n++] = static_cast<char>(c);
        } else {
            too_long = true;
            drain_pipe_line(h_in);
            break;
        }
    }

    const bool yes = !too_long && n == 3 && line[0] == 'Y' && line[1] == 'E' && line[2] == 'S';
    return yes ? ConfirmResult::Yes : ConfirmResult::No;
}

// Echo stays on. The reply must be exactly YES.
ConfirmResult read_confirm() {
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in == nullptr || h_in == INVALID_HANDLE_VALUE) {
        return ConfirmResult::IoError;
    }

    DWORD mode = 0;
    const bool is_console = GetConsoleMode(h_in, &mode) != 0;
    if (is_console) {
        return read_console_confirm(h_in);
    }

    const ConfirmResult result = read_pipe_confirm(h_in);
    fputc('\n', stderr);
    return result;
}

int confirm_deletion() {
    fflush(stderr);
    switch (read_confirm()) {
        case ConfirmResult::Yes:
            return kExitOk;
        case ConfirmResult::No:
            fprintf(stderr, "ERROR: Deletion was not confirmed.\n");
            return kExitError;
        case ConfirmResult::IoError:
        default:
            fprintf(stderr, "ERROR: Failed to read the confirmation.\n");
            return kExitError;
    }
}

int cmd_del(int index_count, wchar_t** index_args) {
    if (index_count < 0 || index_count > 2) {
        print_usage();
        return kExitUsage;
    }

    uint32_t first = 0;
    uint32_t last = 0;
    if (index_count >= 1 && !parse_index(index_args[0], &first)) {
        print_usage();
        return kExitUsage;
    }
    if (index_count == 2 && !parse_index(index_args[1], &last)) {
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
    crypto_wipe_key(&key);

    std::vector<std::wstring> names;
    if (!folder_list_cri(&names)) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }
    if (names.empty()) {
        fprintf(stderr, "ERROR: There are no images in the folder.\n");
        return kExitError;
    }
    if (names.size() > (std::numeric_limits<uint32_t>::max)()) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }

    uint32_t begin = 1;
    uint32_t end = static_cast<uint32_t>(names.size());
    if (index_count >= 1) {
        begin = first;
        end = index_count == 2 ? last : first;
    }

    if (begin < 1 || end < 1 || static_cast<size_t>(begin) > names.size() ||
        static_cast<size_t>(end) > names.size()) {
        fprintf(stderr, "ERROR: The image index is out of range.\n");
        return kExitError;
    }
    if (begin > end) {
        fprintf(stderr, "ERROR: The first index is greater than the last index.\n");
        return kExitError;
    }

    const size_t remove_count = static_cast<size_t>(end) - static_cast<size_t>(begin) + 1;
    if (index_count == 0) {
        fprintf(stderr, "Delete all %zu images? Type YES to confirm: ", names.size());
    } else if (begin == end) {
        fprintf(stderr, "Delete image %u (%ls)? Type YES to confirm: ", begin,
                names[begin - 1].c_str());
    } else {
        fprintf(stderr, "Delete images %u through %u (%zu images)? Type YES to confirm: ", begin,
                end, remove_count);
    }

    const int confirmed = confirm_deletion();
    if (confirmed != kExitOk) {
        return confirmed;
    }

    for (size_t index = begin; index <= end; ++index) {
        const wchar_t* name = names[index - 1].c_str();
        if (!folder_delete_cri(name)) {
            fprintf(stderr, "ERROR: Failed to delete the image: %ls.\n", name);
            return kExitError;
        }
        fprintf(stderr, "INFO: Image was deleted successfully: %ls.\n", name);
    }
    return kExitOk;
}

struct RekeyFile {
    std::wstring temp;
    std::wstring dest;
    // Original ciphertext, renamed aside until .crivate is replaced.
    // TempCleaner must not delete this: after parking it is the only old copy.
    std::wstring backup;
};

struct TempCleaner {
    std::vector<RekeyFile>* files = nullptr;
    std::wstring* header = nullptr;

    ~TempCleaner() {
        if (files != nullptr) {
            for (const RekeyFile& file : *files) {
                if (!file.temp.empty()) {
                    DeleteFileW(file.temp.c_str());
                }
            }
        }
        if (header != nullptr && !header->empty()) {
            DeleteFileW(header->c_str());
        }
    }
};

bool replace_with_temp(const wchar_t* temp, const wchar_t* dest) {
    if (temp == nullptr || temp[0] == L'\0' || dest == nullptr || dest[0] == L'\0') {
        return false;
    }
    if (MoveFileExW(temp, dest, MOVEFILE_REPLACE_EXISTING) != 0) {
        return true;
    }

    const DWORD attr = GetFileAttributesW(dest);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    if (SetFileAttributesW(dest, attr & ~FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    return MoveFileExW(temp, dest, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool ends_with_cri_name(const wchar_t* path) {
    if (path == nullptr) {
        return false;
    }
    const size_t n = wcslen(path);
    if (n < 4) {
        return false;
    }
    return CompareStringOrdinal(path + (n - 4), 4, L".cri", 4, TRUE) == CSTR_EQUAL;
}

bool delete_allowing_readonly(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    if (DeleteFileW(path) != 0) {
        return true;
    }
    const DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    if (SetFileAttributesW(path, attr & ~FILE_ATTRIBUTE_READONLY) == 0) {
        return false;
    }
    return DeleteFileW(path) != 0;
}

// Move dest onto a new temp name. The original bytes stay on disk under backup.
bool park_original(RekeyFile* file) {
    if (file == nullptr || file->dest.empty() || !file->backup.empty()) {
        return false;
    }

    wchar_t temp_path[MAX_PATH];
    temp_path[0] = L'\0';
    if (GetTempFileNameW(L".", L"crb", 0, temp_path) == 0) {
        return false;
    }
    if (ends_with_cri_name(temp_path)) {
        DeleteFileW(temp_path);
        return false;
    }

    std::wstring parked;
    try {
        parked.assign(temp_path);
    } catch (const std::bad_alloc&) {
        DeleteFileW(temp_path);
        return false;
    }

    if (MoveFileExW(file->dest.c_str(), parked.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        DeleteFileW(temp_path);
        return false;
    }

    file->backup.swap(parked);
    return true;
}

bool install_rekeyed_image(RekeyFile* file) {
    if (file == nullptr || file->temp.empty() || file->dest.empty()) {
        return false;
    }
    if (MoveFileW(file->temp.c_str(), file->dest.c_str()) == 0) {
        return false;
    }
    file->temp.clear();
    return true;
}

// Put parked originals back on their .cri names. Backups that could not be moved stay set.
bool restore_parked_originals(std::vector<RekeyFile>* files) {
    if (files == nullptr) {
        return false;
    }
    bool all_ok = true;
    for (RekeyFile& file : *files) {
        if (file.backup.empty()) {
            continue;
        }
        if (!replace_with_temp(file.backup.c_str(), file.dest.c_str())) {
            all_ok = false;
            continue;
        }
        file.backup.clear();
    }
    return all_ok;
}

// Restores parked images unless committed. Declare it after TempCleaner so a failure
// puts originals back before new temps are deleted.
struct OriginalGuard {
    std::vector<RekeyFile>* files = nullptr;
    bool committed = false;
    bool rolled_back = false;

    explicit OriginalGuard(std::vector<RekeyFile>* parked) : files(parked) {}
    OriginalGuard(const OriginalGuard&) = delete;
    OriginalGuard& operator=(const OriginalGuard&) = delete;

    ~OriginalGuard() {
        if (!committed && !rolled_back && files != nullptr) {
            restore_parked_originals(files);
        }
    }
};

void print_password_read_error(CriReadStatus status, const wchar_t* name) {
    switch (status) {
        case CriReadStatus::AuthFailed:
            fprintf(stderr, "ERROR: Failed to change the password. The image file is corrupted: %ls.\n",
                    name);
            break;
        case CriReadStatus::OutOfMemory:
            fprintf(stderr, "ERROR: Failed to change the password. The image is too large: %ls.\n",
                    name);
            break;
        case CriReadStatus::InvalidFile:
            fprintf(stderr, "ERROR: Failed to change the password. The image file is invalid: %ls.\n",
                    name);
            break;
        default:
            fprintf(stderr, "ERROR: Failed to change the password. Failed to read the image: %ls.\n",
                    name);
            break;
    }
}

int cmd_password() {
    const int folder_status = require_folder();
    if (folder_status != kExitOk) {
        return folder_status;
    }

    AesKey old_key{};
    FolderRekey rekey{};
    struct KeyGuard {
        AesKey* old_key;
        FolderRekey* rekey;
        ~KeyGuard() {
            crypto_wipe_key(old_key);
            folder_wipe_rekey(rekey);
        }
    } keys{&old_key, &rekey};

    const int unlock_status = prompt_unlock(&old_key);
    if (unlock_status != kExitOk) {
        return unlock_status;
    }

    SecretBuf next_password;
    SecretBuf confirm;
    const int pw_status = report_password_read(prompt_password("New password: ", &next_password));
    if (pw_status != kExitOk) {
        return pw_status;
    }
    const int confirm_status =
        report_password_read(prompt_password("Confirm password: ", &confirm));
    if (confirm_status != kExitOk) {
        return confirm_status;
    }
    if (!passwords_equal(next_password, confirm)) {
        fprintf(stderr, "ERROR: The passwords do not match.\n");
        return kExitError;
    }
    confirm.wipe();

    uint32_t iters = 0;
    if (!folder_kdf_iters(&iters) ||
        !folder_prepare_rekey(next_password.bytes.data(), next_password.bytes.size(), iters,
                              &rekey)) {
        next_password.wipe();
        fprintf(stderr, "ERROR: Failed to change the password.\n");
        return kExitError;
    }
    next_password.wipe();

    std::vector<std::wstring> names;
    if (!folder_list_cri(&names)) {
        fprintf(stderr, "ERROR: Failed to list the directory.\n");
        return kExitError;
    }

    std::vector<RekeyFile> files;
    std::wstring header_temp;
    TempCleaner temps{&files, &header_temp};

    try {
        files.reserve(names.size());
        for (const std::wstring& name : names) {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> rgb;
            RgbGuard rgb_guard{&rgb};
            const CriReadStatus read = cri_read(name.c_str(), old_key, &width, &height, &rgb);
            if (read != CriReadStatus::Ok) {
                print_password_read_error(read, name.c_str());
                return kExitError;
            }

            files.push_back(RekeyFile{});
            files.back().dest = name;
            const CriWriteStatus written = cri_write_temp(rekey.key, width, height, rgb.data(),
                                                          rgb.size(), &files.back().temp);
            if (written != CriWriteStatus::Ok || files.back().temp.empty()) {
                files.pop_back();
                fprintf(stderr, "ERROR: Failed to change the password.\n");
                return kExitError;
            }
        }
    } catch (const std::bad_alloc&) {
        fprintf(stderr, "ERROR: Failed to change the password.\n");
        return kExitError;
    }

    if (!folder_write_rekey_temp(rekey, &header_temp)) {
        fprintf(stderr, "ERROR: Failed to change the password.\n");
        return kExitError;
    }

    // Park every original before installing a replacement. If a later rename fails,
    // the old ciphertext is still on disk and the folder stays on the old key.
    OriginalGuard originals(&files);
    const auto abort_replace = [&](const wchar_t* failed) -> int {
        bool restored = restore_parked_originals(&files);
        if (!restored) {
            restored = restore_parked_originals(&files);
        }
        originals.rolled_back = true;
        if (restored) {
            fprintf(stderr, "ERROR: Failed to change the password.\n");
            if (failed != nullptr && failed[0] != L'\0') {
                fprintf(stderr, "ERROR: Could not replace %ls.\n", failed);
            }
            return kExitError;
        }
        fprintf(stderr, "ERROR: Failed to finish changing the password.\n");
        for (const RekeyFile& file : files) {
            if (!file.backup.empty()) {
                fprintf(stderr,
                        "ERROR: Put %ls back as %ls, replacing it if it already exists, "
                        "then run password again.\n",
                        file.backup.c_str(), file.dest.c_str());
            }
        }
        return kExitError;
    };

    for (RekeyFile& file : files) {
        if (!park_original(&file)) {
            return abort_replace(file.dest.c_str());
        }
    }
    for (RekeyFile& file : files) {
        if (!install_rekeyed_image(&file)) {
            return abort_replace(file.dest.c_str());
        }
    }
    if (!replace_with_temp(header_temp.c_str(), L".crivate")) {
        return abort_replace(L".crivate");
    }
    header_temp.clear();
    originals.committed = true;

    for (RekeyFile& file : files) {
        if (file.backup.empty()) {
            continue;
        }
        if (!delete_allowing_readonly(file.backup.c_str())) {
            fprintf(stderr,
                    "WARNING: The password was changed, but a leftover file could not be deleted: %ls.\n",
                    file.backup.c_str());
            continue;
        }
        file.backup.clear();
    }

    fprintf(stderr, "INFO: The password was changed.\n");
    return kExitOk;
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
        if (argc < 3) {
            print_usage();
            return kExitUsage;
        }
        return cmd_add(argc - 2, argv + 2);
    }

    if (wcscmp(cmd, L"count") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_count();
    }

    if (wcscmp(cmd, L"list") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_list();
    }

    if (wcscmp(cmd, L"view") == 0) {
        if (argc > 4) {
            print_usage();
            return kExitUsage;
        }
        return cmd_view(argc - 2, argv + 2);
    }

    if (wcscmp(cmd, L"del") == 0) {
        if (argc > 4) {
            print_usage();
            return kExitUsage;
        }
        return cmd_del(argc - 2, argv + 2);
    }

    if (wcscmp(cmd, L"password") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_password();
    }

    print_usage();
    return kExitUsage;
}
