// oz_injector — standalone DLL injector core.
//
// This file is the contract between the injection engine and the UI. Nothing
// here knows about Qt, Win32 messages, or windows — it is plain Win32 + kernel32
// so the engine can be reused from a console tool or a different UI.
//
// The engine supports two injection modes:
//
//   ManualMap  — no LoadLibrary, nothing is added to the target's module list.
//                The PE is relocated and its imports fixed up inside memory we
//                allocate ourselves, then DllMain is called through a tiny
//                x64 trampoline. Stealthier, but harder to get right.
//
//   LoadLibrary — classic remote LoadLibraryW. Simpler and battle-tested, but
//                the DLL shows up in the target's module list and can be
//                blocked by anything that inspects it.
//
// Both modes require the same baseline privileges on the target:
// PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
// PROCESS_VM_WRITE | PROCESS_VM_READ.
#pragma once

#ifndef OZ_INJECTOR_CORE_H
#define OZ_INJECTOR_CORE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace oz {

// How the DLL gets into the target process.
enum class InjectMode {
    ManualMap,  // stealthy, no module-list entry
    LoadLibrary  // classic, appears in module list
};

const wchar_t* inject_mode_name(InjectMode mode);

// A process the user can inject into.
struct ProcessInfo {
    DWORD pid = 0;
    std::wstring exe_name;    // e.g. L"javaw.exe"
    std::wstring exe_path;    // full path via QueryFullProcessImageNameW
    std::wstring window_title;// main window title, empty if headless
    std::wstring window_class;
    bool elevated = false;    // target runs at higher integrity than us
    bool injected = false;    // we already injected into this pid this session
};

// Why an injection failed, or what it did. Every failure path carries a code so
// the UI can show something more useful than "it didn't work".
enum class InjectStatus {
    Ok,
    InvalidDllPath,     // file missing or unreadable
    NotAPe,             // DOS / NT signature mismatch
    WrongArchitecture,  // not x64 while we are x64
    OpenProcessDenied,  // target protected, or integrity level mismatch
    AllocateFailed,
    RelocationUnsupported, // no .reloc section while loaded at a new base
    ImportResolveFailed,   // a dependency could not be loaded in the target
    RemoteThreadFailed,
    AlreadyInjected
};

const wchar_t* inject_status_text(InjectStatus s);

// Result of one injection attempt.
struct InjectResult {
    InjectStatus status = InjectStatus::Ok;
    std::wstring detail;         // human-readable specifics (which DLL, which step)
    std::vector<std::wstring> log;  // ordered step-by-step trace for the UI
    DWORD remote_image = 0;      // deprecated: kept for ABI stability, unused
    ULONGLONG remote_base = 0;   // ManualMap: where the image landed (0 = LoadLibrary)
    ULONGLONG remote_entry = 0;  // ManualMap: AddressOfEntryPoint + base
};

// Result of walking the DLL's export table.
struct ExportInfo {
    bool has_dll_main = false;
    std::vector<std::wstring> exports;
};

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------

// Snapshots the process list. When java_only is true only javaw.exe/java.exe are
// returned; otherwise every process the caller can open a query handle to.
std::vector<ProcessInfo> enumerate_processes(bool java_only);

// Reads the main window title for one pid, preferring the longest visible
// top-level title (the Minecraft main window beats a tiny tooltip window).
std::wstring window_title_for(DWORD pid, std::wstring* out_class);

// ---------------------------------------------------------------------------
// DLL inspection (runs entirely in our own process, no target involved)
// ---------------------------------------------------------------------------

// Validates the PE headers, architecture and reloc support. Fills `status` on
// failure and returns false.
bool validate_pe(const void* bytes, size_t size, InjectStatus* status,
                  std::wstring* detail);

// Lists the DLL's exports. Used to warn the user when a DLL has no DllMain
// entry point and injecting it would do nothing.
ExportInfo inspect_exports(const void* bytes, size_t size);

// Reads a file into memory. Returns false if it cannot be read.
bool read_file(const std::wstring& path, std::vector<BYTE>& out, std::wstring* error);

// ---------------------------------------------------------------------------
// Injection
// ---------------------------------------------------------------------------

// Injects `dll_path` into `pid` using `mode`. Blocks until finished — call it
// from a worker thread so the UI stays responsive.
InjectResult inject(DWORD pid, const std::wstring& dll_path, InjectMode mode);

// Manual-map only: returns the PE entry point of an already-mapped image.
// Exposed for tests and for callers that want to drive DllMain themselves.
bool manual_map_into(DWORD pid, const void* dll_bytes, size_t dll_size,
                     InjectResult* result);

}  // namespace oz

#endif  // OZ_INJECTOR_CORE_H
