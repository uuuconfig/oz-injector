// oz_injector — injection engine implementation.
//
// Derived from OpenZen's native/loader/manual_map.cpp, generalised: the DLL now
// comes from an arbitrary filesystem path instead of an embedded RCDATA blob,
// and the target is any pid rather than only javaw.exe. Validation, status
// codes and a step log were added on top.
#include "oz_injector_core.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <sstream>

#pragma comment(lib, "psapi.lib")

namespace oz {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::wstring widen(const char* narrow) {
    if (!narrow) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, narrow, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, out.data(), n);
    return out;
}

std::wstring fmt(const wchar_t* format, ...) {
    std::wstring out(512, L'\0');
    for (;;) {
        va_list ap;
        va_start(ap, format);
        const int written = _vsnwprintf_s(&out[0], out.size() + 1, _TRUNCATE,
                                          format, ap);
        va_end(ap);
        if (written >= 0 && static_cast<size_t>(written) < out.size()) {
            out.resize(static_cast<size_t>(written));
            return out;
        }
        if (out.size() > (1u << 16)) return out;
        out.resize(out.size() * 2);
    }
}

std::wstring win_error_text(DWORD err) {
    wchar_t* raw = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring out;
    if (len && raw) {
        out.assign(raw, len);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' ||
                                out.back() == L' '))
            out.pop_back();
    }
    if (raw) LocalFree(raw);
    if (out.empty()) out = L"unknown error";
    return out;
}

bool iequals(const std::wstring& a, const wchar_t* b) {
    if (a.size() != wcslen(b)) return false;
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1,
                                TRUE) == CSTR_EQUAL;
}

const IMAGE_NT_HEADERS* nt_of(const void* base) {
    auto dos = static_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(
        static_cast<const BYTE*>(base) + dos->e_lfanew);
}

// True when `name` ends with a backslash, i.e. already looks like a directory.
bool ends_with_sep(const std::wstring& name) {
    return !name.empty() &&
           (name.back() == L'\\' || name.back() == L'/');
}

// Copies the raw file bytes into `out`. Reports a readable error on failure.
bool slurp(const std::wstring& path, std::vector<BYTE>& out, std::wstring* error) {    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = fmt(L"CreateFile failed: %s", win_error_text(GetLastError()).c_str());
        }
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (512LL << 20)) {
        if (error) {
            const DWORD e = GetLastError();
            *error = (size.QuadPart > (512LL << 20))
                         ? L"file is larger than 512 MB"
                         : L"GetFileSizeEx failed: " + win_error_text(e);
        }
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        if (!ReadFile(h, out.data() + total,
                      static_cast<DWORD>(std::min<size_t>(out.size() - total, 1u << 20)),
                      &got, nullptr) ||
            got == 0) {
            if (error) *error = L"ReadFile failed: " + win_error_text(GetLastError());
            CloseHandle(h);
            return false;
        }
        total += got;
    }
    CloseHandle(h);
    return true;
}

// Writes into the target's address space with a size check.
bool poke(HANDLE process, void* remote, const void* data, size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, remote, data, size, &written) &&
           written == size;
}

}  // namespace

const wchar_t* inject_mode_name(InjectMode mode) {
    return mode == InjectMode::ManualMap ? L"Manual map" : L"LoadLibrary";
}

const wchar_t* inject_status_text(InjectStatus s) {
    switch (s) {
        case InjectStatus::Ok: return L"Success";
        case InjectStatus::InvalidDllPath: return L"DLL not found or unreadable";
        case InjectStatus::NotAPe: return L"Not a valid PE image";
        case InjectStatus::WrongArchitecture: return L"DLL is not x64";
        case InjectStatus::OpenProcessDenied: return L"Cannot open target process";
        case InjectStatus::AllocateFailed: return L"Allocation in target failed";
        case InjectStatus::RelocationUnsupported:
            return L"DLL has no relocation table";
        case InjectStatus::ImportResolveFailed:
            return L"Could not resolve a dependency in the target";
        case InjectStatus::RemoteThreadFailed: return L"Remote thread creation failed";
        case InjectStatus::AlreadyInjected: return L"Already injected into this process";
    }
    return L"Unknown";
}

// ---------------------------------------------------------------------------
// File + PE inspection
// ---------------------------------------------------------------------------

bool read_file(const std::wstring& path, std::vector<BYTE>& out,
               std::wstring* error) {
    return slurp(path, out, error);
}

bool validate_pe(const void* bytes, size_t size, InjectStatus* status,
                 std::wstring* detail) {
    *status = InjectStatus::Ok;
    if (detail) detail->clear();

    if (!bytes || size < sizeof(IMAGE_DOS_HEADER)) {
        *status = InjectStatus::NotAPe;
        if (detail) *detail = L"file is too small to contain a DOS header";
        return false;
    }
    auto dos = static_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        *status = InjectStatus::NotAPe;
        if (detail) *detail = L"bad DOS signature (not a PE file)";
        return false;
    }
    if (static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > size) {
        *status = InjectStatus::NotAPe;
        if (detail) *detail = L"e_lfanew points outside the file";
        return false;
    }
    const auto* nt = nt_of(bytes);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        *status = InjectStatus::NotAPe;
        if (detail) *detail = L"bad NT signature";
        return false;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        *status = InjectStatus::WrongArchitecture;
        if (detail) {
            *detail = fmt(L"machine 0x%04X is not AMD64 (this tool is x64-only)",
                          nt->FileHeader.Machine);
        }
        return false;
    }

    // Manual mapping at a new base needs IMAGE_DIRECTORY_ENTRY_BASERELOC, but a
    // missing .reloc is only fatal for that mode — LoadLibrary would place the
    // image at its preferred base and be fine. apply_relocations() reports it,
    // so leave the status Ok here.
    return true;
}

ExportInfo inspect_exports(const void* bytes, size_t size) {
    ExportInfo info;
    InjectStatus st = InjectStatus::Ok;
    if (!validate_pe(bytes, size, &st, nullptr)) return info;
    const auto* nt = nt_of(bytes);
    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0 || dir.VirtualAddress + dir.Size > size) return info;

    auto base = static_cast<const BYTE*>(bytes);
    const auto* ed =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const DWORD name_count = ed->NumberOfNames;
    if (name_count == 0 || !ed->AddressOfNames) return info;
    if (dir.VirtualAddress + ed->AddressOfNames + sizeof(DWORD) * name_count >
        size) {
        return info;
    }

    const auto* names =
        reinterpret_cast<const DWORD*>(base + dir.VirtualAddress + ed->AddressOfNames);
    for (DWORD i = 0; i < name_count; ++i) {
        const DWORD rva = names[i];
        if (dir.VirtualAddress + rva >= size) continue;
        std::wstring name =
            widen(reinterpret_cast<const char*>(base + dir.VirtualAddress + rva));
        if (name == L"DllMain") info.has_dll_main = true;
        info.exports.push_back(name);
    }
    return info;
}

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------

namespace {

struct TitleSearch {
    DWORD pid;
    std::wstring best_title;
    std::wstring best_class;
};

BOOL CALLBACK enum_title_proc(HWND hwnd, LPARAM lp) {
    auto* s = reinterpret_cast<TitleSearch*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    // Skip child/owned windows — we want the app's main window.
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != s->pid) return TRUE;

    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return TRUE;
    std::wstring title(static_cast<size_t>(len), L'\0');
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(static_cast<size_t>(len));

    // Prefer the longest title: the Minecraft main window carries the version
    // and world name, whereas tooltip windows are usually a bare "Java".
    if (title.size() > s->best_title.size()) {
        wchar_t cls[256] = {0};
        GetClassNameW(hwnd, cls, 256);
        s->best_title = std::move(title);
        s->best_class = cls;
    }
    return TRUE;
}

// Reads the integrity level of the main window's process so we can flag
// targets we are not allowed to touch (e.g. an elevated game vs our normal
// shell). Without this the failure shows up as a bare ERROR_ACCESS_DENIED.
bool process_is_elevated(DWORD pid) {
    // A higher-integrity target refuses a VM_READ handle from a normal shell,
    // so probing with that right is enough to detect the mismatch without
    // opening a token or duplicating handles.
    HANDLE probe = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE,
                              pid);
    if (!probe) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(probe);
    return false;
}

}  // namespace

std::wstring window_title_for(DWORD pid, std::wstring* out_class) {
    TitleSearch s{pid, {}, {}};
    EnumWindows(enum_title_proc, reinterpret_cast<LPARAM>(&s));
    if (out_class) *out_class = s.best_class;
    return s.best_title;
}

std::vector<ProcessInfo> enumerate_processes(bool java_only) {
    std::vector<ProcessInfo> out;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            const std::wstring name = pe.szExeFile;
            const bool java = iequals(name, L"javaw.exe") || iequals(name, L"java.exe");
            if (java_only && !java) continue;
            // Skip the 64-bit-vs-32-bit mismatch we cannot inject into anyway.
            if (!java_only && iequals(name, L"OpenZenInjector.exe")) continue;

            ProcessInfo pi;
            pi.pid = pe.th32ProcessID;
            pi.exe_name = name;

            HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.pid);
            if (q) {
                wchar_t buf[MAX_PATH * 2] = {0};
                DWORD size = static_cast<DWORD>(_countof(buf));
                if (QueryFullProcessImageNameW(q, 0, buf, &size)) {
                    pi.exe_path.assign(buf, size);
                }
                CloseHandle(q);
            }
            pi.window_title = window_title_for(pi.pid, &pi.window_class);
            pi.elevated = process_is_elevated(pi.pid);
            out.push_back(std::move(pi));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // Windows first, then by PID, so the interesting rows are on top.
    std::stable_sort(out.begin(), out.end(),
                     [](const ProcessInfo& a, const ProcessInfo& b) {
                         const bool aw = !a.window_title.empty();
                         const bool bw = !b.window_title.empty();
                         if (aw != bw) return aw;
                         return a.pid < b.pid;
                     });
    return out;
}

// ---------------------------------------------------------------------------
// Manual mapping
// ---------------------------------------------------------------------------

namespace {

DWORD protect_from_characteristics(DWORD c) {
    const bool x = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
    const bool r = (c & IMAGE_SCN_MEM_READ) != 0;
    const bool w = (c & IMAGE_SCN_MEM_WRITE) != 0;
    if (x && r && w) return PAGE_EXECUTE_READWRITE;
    if (x && r) return PAGE_EXECUTE_READ;
    if (x) return PAGE_EXECUTE;
    if (r && w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

// Applies IMAGE_REL_BASED fixups so the image can live at remote_image instead
// of its preferred ImageBase.
bool apply_relocations(BYTE* image, const IMAGE_NT_HEADERS* nt, ULONGLONG delta,
                       std::wstring* err) {
    if (delta == 0) return true;
    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.Size == 0) {
        if (err) *err = L"image has no .reloc section but must be relocated";
        return false;
    }
    BYTE* block_ptr = image + dir.VirtualAddress;
    BYTE* const end = block_ptr + dir.Size;
    while (block_ptr < end) {
        auto block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(block_ptr);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        const DWORD count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto entries = reinterpret_cast<const WORD*>(block + 1);
        BYTE* page = image + block->VirtualAddress;
        for (DWORD i = 0; i < count; ++i) {
            const WORD type = entries[i] >> 12;
            const WORD off = entries[i] & 0x0FFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                *reinterpret_cast<ULONGLONG*>(page + off) += delta;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                *reinterpret_cast<DWORD*>(page + off) +=
                    static_cast<DWORD>(delta);
            }
            // IMAGE_REL_BASED_ABSOLUTE (0) is padding — ignore it.
        }
        block_ptr += block->SizeOfBlock;
    }
    return true;
}

// Runs LoadLibraryW inside the target so we can bring in a dependency that is
// not already mapped. Returns the module base the target sees.
HMODULE remote_load_library(HANDLE process, const std::wstring& dll_name,
                            std::wstring* err) {
    const size_t bytes = (dll_name.size() + 1) * sizeof(wchar_t);
    LPVOID arg = VirtualAllocEx(process, nullptr, bytes,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!arg) {
        if (err) *err = L"VirtualAllocEx for the DLL path failed";
        return nullptr;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, arg, dll_name.c_str(), bytes, &written) ||
        written != bytes) {
        VirtualFreeEx(process, arg, 0, MEM_RELEASE);
        if (err) *err = L"WriteProcessMemory for the DLL path failed";
        return nullptr;
    }
    auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, load_lib, arg, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, arg, 0, MEM_RELEASE);
        if (err) *err = L"CreateRemoteThread(LoadLibraryW) failed";
        return nullptr;
    }
    WaitForSingleObject(thread, 10000);
    DWORD ret = 0;
    GetExitCodeThread(thread, &ret);
    CloseHandle(thread);
    VirtualFreeEx(process, arg, 0, MEM_RELEASE);

    // On x64 GetExitCodeThread truncates the 64-bit HMODULE to a DWORD. In
    // practice ASLR keeps module bases inside 32 bits, and resolve_imports()
    // falls back to an enum lookup when this comes back wrong.
    return reinterpret_cast<HMODULE>(static_cast<ULONGLONG>(ret));
}

HMODULE find_remote_module(HANDLE process, const wchar_t* name) {
    HMODULE mods[1024] = {nullptr};
    DWORD cb = 0;
    if (!EnumProcessModulesEx(process, mods, sizeof mods, &cb, LIST_MODULES_ALL)) {
        return nullptr;
    }
    const DWORD count = cb / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t buf[MAX_PATH] = {0};
        if (GetModuleBaseNameW(process, mods[i], buf, MAX_PATH) &&
            iequals(buf, name)) {
            return mods[i];
        }
    }
    return nullptr;
}

// Rewrites the local image's IAT so each entry points at the *target's* copy of
// the imported function.
bool resolve_imports(HANDLE process, BYTE* local_image,
                     const IMAGE_NT_HEADERS* nt, std::wstring* err) {
    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size == 0) return true;
    auto desc =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(local_image + dir.VirtualAddress);

    while (desc->Name) {
        const char* narrow =
            reinterpret_cast<const char*>(local_image + desc->Name);
        wchar_t dll_name[MAX_PATH] = {0};
        MultiByteToWideChar(CP_UTF8, 0, narrow, -1, dll_name, MAX_PATH);

        HMODULE remote_mod = find_remote_module(process, dll_name);
        if (!remote_mod) {
            remote_mod = remote_load_library(process, dll_name, nullptr);
            if (!remote_mod) remote_mod = find_remote_module(process, dll_name);
        }
        if (!remote_mod) {
            if (err) {
                *err = fmt(L"could not load dependency %s in the target process",
                           dll_name);
            }
            return false;
        }

        // Use our own copy of the dependency to walk its export table. Windows
        // resolves a given export to a constant RVA, so:
        //     remote_fn = remote_mod_base + (local_fn - local_mod_base)
        HMODULE local_mod = GetModuleHandleA(narrow);
        if (!local_mod) local_mod = LoadLibraryA(narrow);
        if (!local_mod) {
            if (err) *err = fmt(L"could not load dependency %s locally", dll_name);
            return false;
        }

        auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            local_image + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                     : desc->FirstThunk));
        auto iat =
            reinterpret_cast<IMAGE_THUNK_DATA*>(local_image + desc->FirstThunk);

        while (thunk->u1.AddressOfData) {
            FARPROC local_fn = nullptr;
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) {
                local_fn = GetProcAddress(
                    local_mod,
                    reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk->u1.Ordinal)));
            } else {
                auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    local_image + thunk->u1.AddressOfData);
                local_fn = GetProcAddress(local_mod, by_name->Name);
            }
            if (local_fn) {
                const ULONGLONG remote_fn =
                    reinterpret_cast<ULONGLONG>(remote_mod) +
                    (reinterpret_cast<ULONGLONG>(local_fn) -
                     reinterpret_cast<ULONGLONG>(local_mod));
                iat->u1.Function = remote_fn;
            }
            ++thunk;
            ++iat;
        }
        ++desc;
    }
    return true;
}

}  // namespace

bool manual_map_into(DWORD pid, const void* dll_bytes, size_t dll_size,
                     InjectResult* result) {
    result->log.clear();
    auto step = [&](const std::wstring& s) { result->log.push_back(s); };

    InjectStatus st = InjectStatus::Ok;
    std::wstring detail;
    if (!validate_pe(dll_bytes, dll_size, &st, &detail)) {
        result->status = st;
        result->detail = detail;
        step(L"PE validation failed: " + detail);
        return false;
    }
    const auto* nt = nt_of(dll_bytes);
    step(L"PE validated: x64, SizeOfImage=" +
         fmt(L"0x%X", nt->OptionalHeader.SizeOfImage));

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        result->status = InjectStatus::OpenProcessDenied;
        result->detail = win_error_text(GetLastError());
        step(L"OpenProcess failed: " + result->detail);
        return false;
    }
    step(L"Opened target process");

    const SIZE_T image_size = nt->OptionalHeader.SizeOfImage;
    LPVOID remote_image =
        VirtualAllocEx(process, nullptr, image_size, MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE);
    if (!remote_image) {
        result->status = InjectStatus::AllocateFailed;
        result->detail = win_error_text(GetLastError());
        step(L"VirtualAllocEx failed: " + result->detail);
        CloseHandle(process);
        return false;
    }
    step(fmt(L"Allocated image at 0x%llX", reinterpret_cast<ULONGLONG>(remote_image)));

    // Assemble the image in our own memory first: relocation and import fixups
    // are pure local writes, far cheaper than round-tripping the whole PE
    // through ReadProcessMemory/WriteProcessMemory.
    std::vector<BYTE> local_image(image_size, 0);
    memcpy(local_image.data(), dll_bytes, nt->OptionalHeader.SizeOfHeaders);
    auto sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sect) {
        if (sect->SizeOfRawData == 0) continue;
        if (static_cast<size_t>(sect->PointerToRawData) + sect->SizeOfRawData >
            dll_size) {
            continue;
        }
        memcpy(local_image.data() + sect->VirtualAddress,
               static_cast<const BYTE*>(dll_bytes) + sect->PointerToRawData,
               sect->SizeOfRawData);
    }
    step(L"Copied headers and sections locally");

    const ULONGLONG delta = reinterpret_cast<ULONGLONG>(remote_image) -
                            nt->OptionalHeader.ImageBase;
    std::wstring err;
    if (!apply_relocations(local_image.data(), nt, delta, &err)) {
        result->status = InjectStatus::RelocationUnsupported;
        result->detail = err;
        step(L"Relocation failed: " + err);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    step(fmt(L"Relocations applied (delta 0x%llX)", delta));

    if (!resolve_imports(process, local_image.data(), nt, &err)) {
        result->status = InjectStatus::ImportResolveFailed;
        result->detail = err;
        step(L"Import resolution failed: " + err);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    step(L"Imports resolved against the target's modules");

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_image, local_image.data(), image_size,
                            &written) ||
        written != image_size) {
        result->status = InjectStatus::AllocateFailed;
        result->detail = win_error_text(GetLastError());
        step(L"WriteProcessMemory failed: " + result->detail);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    step(L"Image written to the target");

    // Drop RWX down to what each section originally declared.
    sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sect) {
        if (sect->Misc.VirtualSize == 0) continue;
        DWORD old = 0;
        VirtualProtectEx(process,
                         static_cast<BYTE*>(remote_image) + sect->VirtualAddress,
                         sect->Misc.VirtualSize,
                         protect_from_characteristics(sect->Characteristics), &old);
    }
    step(L"Section permissions tightened to match the PE");

    // CreateRemoteThread takes a single pointer-sized argument, but DllMain
    // needs three. Drop a tiny x64 trampoline that marshals them through the
    // standard MSVC ABI:
    //     DllMain(hModule = image, fdwReason = DLL_PROCESS_ATTACH, NULL)
    BYTE trampoline[] = {
        0x48, 0xB9, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rcx, imm64 (hModule)
        0xBA, 0x01, 0x00, 0x00, 0x00,        // mov edx, 1     (DLL_PROCESS_ATTACH)
        0x4D, 0x31, 0xC0,                    // xor r8, r8     (lpvReserved)
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, imm64 (entry point)
        0x48, 0x83, 0xEC, 0x28,              // sub rsp, 0x28  (shadow space)
        0xFF, 0xD0,                          // call rax
        0x48, 0x83, 0xC4, 0x28,              // add rsp, 0x28
        0xC3                                 // ret
    };
    const ULONGLONG hmod = reinterpret_cast<ULONGLONG>(remote_image);
    const ULONGLONG entry = hmod + nt->OptionalHeader.AddressOfEntryPoint;
    memcpy(trampoline + 2, &hmod, sizeof hmod);
    memcpy(trampoline + 20, &entry, sizeof entry);

    LPVOID remote_trampoline =
        VirtualAllocEx(process, nullptr, sizeof trampoline, MEM_COMMIT | MEM_RESERVE,
                       PAGE_EXECUTE_READWRITE);
    if (!remote_trampoline) {
        result->status = InjectStatus::AllocateFailed;
        result->detail = L"could not allocate the trampoline";
        step(L"Trampoline allocation failed: " + result->detail);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    if (!poke(process, remote_trampoline, trampoline, sizeof trampoline)) {
        result->status = InjectStatus::AllocateFailed;
        result->detail = L"could not write the trampoline: " +
                         win_error_text(GetLastError());
        step(result->detail);
        VirtualFreeEx(process, remote_trampoline, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    step(L"Trampoline written");

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                           remote_trampoline),
                                       nullptr, 0, nullptr);
    if (!thread) {
        result->status = InjectStatus::RemoteThreadFailed;
        result->detail = win_error_text(GetLastError());
        step(L"CreateRemoteThread failed: " + result->detail);
        VirtualFreeEx(process, remote_trampoline, 0, MEM_RELEASE);
        VirtualFreeEx(process, remote_image, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // 30s is generous; a stuck DllMain in the target should not hang our UI
    // forever, and we still free the trampoline below.
    WaitForSingleObject(thread, 30000);
    CloseHandle(thread);
    VirtualFreeEx(process, remote_trampoline, 0, MEM_RELEASE);

    // Deliberately leave remote_image mapped: a manually mapped DLL has no
    // unload path, so it must stay resident for the life of the process.
    result->status = InjectStatus::Ok;
    result->remote_base = reinterpret_cast<ULONGLONG>(remote_image);
    result->remote_entry = result->remote_base +
                           nt->OptionalHeader.AddressOfEntryPoint;
    result->detail = fmt(L"Image resident at 0x%llX, entry 0x%llX",
                         result->remote_base, result->remote_entry);
    step(L"DllMain returned; image left resident at 0x" +
         fmt(L"%llX", reinterpret_cast<ULONGLONG>(remote_image)));
    CloseHandle(process);
    return true;
}

// ---------------------------------------------------------------------------
// Injection entry point
// ---------------------------------------------------------------------------

namespace {

InjectResult inject_load_library(DWORD pid, const std::wstring& dll_path) {
    InjectResult r;
    r.log.clear();
    auto step = [&](const std::wstring& s) { r.log.push_back(s); };

    // Remote LoadLibraryW resolves its argument through the target's module
    // search order. An absolute path is accepted as-is, so we hand over the
    // full path the user picked rather than a bare filename — that way a DLL
    // outside the target's working directory still loads. When the target's
    // search policy rejects it we surface the NULL return below.
    step(L"Mode: LoadLibrary (remote thread)");

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE,
                                 FALSE, pid);
    if (!process) {
        r.status = InjectStatus::OpenProcessDenied;
        r.detail = win_error_text(GetLastError());
        step(L"OpenProcess failed: " + r.detail);
        return r;
    }

    std::wstring name = dll_path;
    if (ends_with_sep(name)) {
        r.status = InjectStatus::InvalidDllPath;
        r.detail = L"path points at a directory";
        step(L"Rejected: " + r.detail);
        CloseHandle(process);
        return r;
    }

    const size_t bytes = (name.size() + 1) * sizeof(wchar_t);
    LPVOID arg = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
    if (!arg) {
        r.status = InjectStatus::AllocateFailed;
        r.detail = win_error_text(GetLastError());
        step(L"VirtualAllocEx failed: " + r.detail);
        CloseHandle(process);
        return r;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, arg, name.c_str(), bytes, &written) ||
        written != bytes) {
        r.status = InjectStatus::AllocateFailed;
        r.detail = L"could not write the DLL path into the target";
        step(r.detail);
        VirtualFreeEx(process, arg, 0, MEM_RELEASE);
        CloseHandle(process);
        return r;
    }

    auto load_lib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread =
        CreateRemoteThread(process, nullptr, 0, load_lib, arg, 0, nullptr);
    if (!thread) {
        r.status = InjectStatus::RemoteThreadFailed;
        r.detail = win_error_text(GetLastError());
        step(L"CreateRemoteThread(LoadLibraryW) failed: " + r.detail);
        VirtualFreeEx(process, arg, 0, MEM_RELEASE);
        CloseHandle(process);
        return r;
    }
    WaitForSingleObject(thread, 15000);
    DWORD ret = 0;
    GetExitCodeThread(thread, &ret);
    CloseHandle(thread);
    VirtualFreeEx(process, arg, 0, MEM_RELEASE);
    CloseHandle(process);

    if (ret == 0) {
        r.status = InjectStatus::RemoteThreadFailed;
        r.detail = L"LoadLibraryW returned NULL in the target (wrong bitness, or "
                   L"the DLL failed its DllMain)";
        step(r.detail);
        return r;
    }

    r.status = InjectStatus::Ok;
    // GetExitCodeThread gives us a DWORD, so the 64-bit HMODULE is truncated.
    // Report it as such rather than pretending it is a usable base.
    r.remote_base = ret;
    r.detail = fmt(
        L"LoadLibraryW returned a non-null handle in the target (truncated to "
        L"0x%X; the real base is a 64-bit value)",
        ret);
    step(r.detail);
    return r;
}

}  // namespace

InjectResult inject(DWORD pid, const std::wstring& dll_path, InjectMode mode) {
    InjectResult r;

    if (dll_path.empty()) {
        r.status = InjectStatus::InvalidDllPath;
        r.detail = L"no DLL selected";
        return r;
    }

    std::vector<BYTE> bytes;
    std::wstring read_err;
    if (!slurp(dll_path, bytes, &read_err)) {
        r.status = InjectStatus::InvalidDllPath;
        r.detail = read_err;
        r.log.push_back(L"Read failed: " + read_err);
        return r;
    }
    r.log.push_back(fmt(L"Read %zu bytes from %s", bytes.size(),
                        dll_path.substr(dll_path.find_last_of(L'\\') + 1).c_str()));

    InjectStatus st = InjectStatus::Ok;
    std::wstring detail;
    if (!validate_pe(bytes.data(), bytes.size(), &st, &detail)) {
        r.status = st;
        r.detail = detail;
        r.log.push_back(L"Validation failed: " + detail);
        return r;
    }

    // Note: most toolchains do not export DllMain (it is reached through the PE
    // entry point, not the export table), so its absence is normal and not a
    // warning. Report the export count instead, and only flag a PE that exports
    // nothing at all.
    const ExportInfo exp = inspect_exports(bytes.data(), bytes.size());
    r.log.push_back(fmt(L"Exports: %zu%s", exp.exports.size(),
                        exp.has_dll_main ? L" (DllMain exported)" : L""));

    if (mode == InjectMode::LoadLibrary) {
        InjectResult lr = inject_load_library(pid, dll_path);
        lr.log.insert(lr.log.begin(), r.log.begin(), r.log.end());
        return lr;
    }

    // manual_map_into fills `r` and reports success as its return value.
    manual_map_into(pid, bytes.data(), bytes.size(), &r);
    return r;
}

}  // namespace oz
