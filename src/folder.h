#pragma once

#include "crypto.h"

#include <string>
#include <vector>

enum class DirState {
    Empty,
    NotEmpty,
    Error,
};

DirState folder_dir_state();
bool folder_marker_exists();
bool folder_exe_in_cwd();

// Readable .crivate with a structurally valid v1 header.
bool folder_is_initialized();

// Write .crivate using the password. Caller must ensure the directory is empty.
bool folder_init(const uint8_t* password, size_t password_len);

// Derive the folder key and check the verifier. On failure, *out_key is wiped.
bool folder_unlock(const uint8_t* password, size_t password_len, AesKey* out_key);

// Basenames of *.cri files in cwd, case-insensitive ordinal sort.
bool folder_list_cri(std::vector<std::wstring>* names);
