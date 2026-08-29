#include "folder.h"

#include <cstdio>
#include <cwchar>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

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
                "warning: crivate.exe should not live in a crivate folder\n");
    }
}

int cmd_init() {
    if (folder_marker_exists()) {
        fprintf(stderr, "error: .crivate already exists\n");
        return kExitError;
    }

    const DirState dir = folder_dir_state();
    if (dir == DirState::Error) {
        fprintf(stderr, "error: failed to list directory\n");
        return kExitError;
    }
    if (dir == DirState::NotEmpty) {
        fprintf(stderr, "error: directory is not empty\n");
        return kExitError;
    }

    if (!folder_write_dummy_marker()) {
        fprintf(stderr, "error: failed to create .crivate\n");
        return kExitError;
    }
    return kExitOk;
}

int cmd_not_implemented() {
    fprintf(stderr, "error: command not implemented yet\n");
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
        return cmd_not_implemented();
    }

    if (wcscmp(cmd, L"count") == 0) {
        if (argc != 2) {
            print_usage();
            return kExitUsage;
        }
        return cmd_not_implemented();
    }

    if (wcscmp(cmd, L"view") == 0) {
        if (argc != 3) {
            print_usage();
            return kExitUsage;
        }
        return cmd_not_implemented();
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
