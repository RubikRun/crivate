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

constexpr size_t kCrivateFileBytes = 92;

// New .crivate header and the key derived for it. Does not touch the file on disk.
struct FolderRekey {
    AesKey key{};
    uint8_t header[kCrivateFileBytes]{};

    FolderRekey() = default;
    FolderRekey(const FolderRekey&) = delete;
    FolderRekey& operator=(const FolderRekey&) = delete;
};

void folder_wipe_rekey(FolderRekey* rekey);

// Iteration count stored in the current .crivate.
bool folder_kdf_iters(uint32_t* out);

// Build a new header in memory using the iteration count already stored in the folder.
bool folder_prepare_rekey(const uint8_t* password, size_t password_len, uint32_t kdf_iters,
                          FolderRekey* out);

// Write that header to a temp file in cwd. The temp name does not end in .cri.
// Does not replace .crivate.
bool folder_write_rekey_temp(const FolderRekey& rekey, std::wstring* out_temp);

// Basenames of *.cri files in cwd, case-insensitive ordinal sort.
bool folder_list_cri(std::vector<std::wstring>* names);

// Delete a cwd .cri basename. Rejects paths. Does not touch .crivate.
bool folder_delete_cri(const wchar_t* basename);

enum class FolderActivityState {
    Held,
    Busy,
    Error,
};

// Serializes add and password in this folder. Hold one from the moment those
// commands might create a temp until the temp has been renamed or deleted.
// If the owner is killed, the mutex is abandoned and the next command can
// remove the leftovers.
class FolderActivity {
public:
    FolderActivity() = default;
    ~FolderActivity();
    FolderActivity(const FolderActivity&) = delete;
    FolderActivity& operator=(const FolderActivity&) = delete;

    // Zero wait. Busy means another crivate process holds the mutex.
    FolderActivityState try_acquire();
    // Waits until the mutex can be taken.
    FolderActivityState acquire();

private:
    bool open_mutex();
    void release();

    void* mutex_ = nullptr;
    bool owned_ = false;
};

struct OrphanTempResult {
    bool scanned = false;
    uint32_t removed = 0;
    // cri/crp/crh temps that exist but could not be deleted.
    std::vector<std::wstring> busy;
    // crb temps. Not deleted: a killed password may have parked the only copy
    // of an image under this name.
    std::vector<std::wstring> parked;
};

// Delete abandoned cri/crp/crh temps in cwd. Does not delete crb temps.
// Caller must hold FolderActivity so a file another crivate is writing stays put.
OrphanTempResult folder_reclaim_orphan_temps();
