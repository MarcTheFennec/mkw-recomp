// The start screen's update (ui/launcher/launcher_update.cpp): GitHub's latest published release, offered at start,
// downloaded by tools/prepare/update.py with the package's Python, then put in place here and the program started
// again, which sets up again only what changed (prepare.py without --source).
#pragma once

enum UpdatePhase { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_OFFER, UPDATE_FETCHING, UPDATE_READY, UPDATE_FAILED };

typedef struct {
    int phase;                // UpdatePhase
    char tag[96];             // the release offered, as "v0.1.5"
    long long bytes;          // its package's size
    long long done, total;    // of the download
    char path[1200];          // UPDATE_READY: the AppImage (Linux) or the package's folder (Windows)
    char error[512];          // UPDATE_FAILED: why
} UpdateState;

void update_check_start(const char *exe_dir);   // at start: nothing unless MKW_UPDATE_CHECK and a packaged install
void update_fetch_start(const char *exe_dir);   // the offered release, once UPDATE_OFFER (or again after its failure)
UpdateState update_state(void);                 // a copy, any time
// UPDATE_READY: the new files in place of the old ones (renamed to *.old, removed at the next start). 1: done, start the
// program again with update_relaunch once the window is gone; 0: nothing changed, the error is in the state.
int update_apply(const char *exe_dir);
void update_relaunch(const char *exe_dir);   // the new program; on Linux this process becomes it
// The version the last update replaced, kept for Undo (Windows: update-previous/ beside the program; Linux: the
// AppImage's copy beside it): 1 and its tag in tag when there is one.
int update_previous(const char *exe_dir, char *tag, unsigned cap);
// Undo: the kept version in place of this one, installed as an update is (so it can be undone in turn). 1: start it
// with update_relaunch once the window is gone; 0: nothing changed, why says why.
int update_revert(const char *exe_dir, char *why, unsigned cap);
// At start: 1 when the previous run installed an update (its mark next to the program, removed here); the files it
// left are removed in the background. The tag installed goes to tag.
int update_just_installed(const char *exe_dir, char *tag, unsigned cap);
