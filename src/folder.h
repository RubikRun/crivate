#pragma once

enum class DirState {
    Empty,
    NotEmpty,
    Error,
};

DirState folder_dir_state();
bool folder_marker_exists();
bool folder_write_dummy_marker();
bool folder_exe_in_cwd();
