// Persistent OpenSSH transport and a cancellable worker with scoped child-process lifetime.
// All runtime settings and credentials remain outside the repository.

#include "remote.h"
#include <wincred.h>
#include <map>
#include <vector>
#include <algorithm>

namespace {
// Windows argv quoting (including a key path with spaces or a trailing slash).
/** Quote a single Windows argv item, doubling backslashes correctly before quotes and at the closing
 * delimiter. */
std::wstring quote(const std::wstring& a) {
    std::wstring r = L"\"";
    size_t slash = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slash;
            continue;
        }
        if (c == L'\"')
            r.append(slash * 2 + 1, L'\\');
        else
            r.append(slash, L'\\');
        slash = 0;
        r += c;
    }
    r.append(slash * 2, L'\\');
    return r + L"\"";
}
/** Create a private double-NUL-terminated child environment; never mutate process-global askpass variables.
 */
std::vector<wchar_t> environment(const Config& c) {
    std::map<std::wstring, std::wstring> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (auto p = block; *p; p += wcslen(p) + 1) {
            std::wstring item = p;
            const auto eq = item.find(L'=', item[0] == L'=' ? 1 : 0);
            if (eq != std::wstring::npos)
                entries[item.substr(0, eq)] = item.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    entries[L"SSH_ASKPASS"] = executableDirectory() + L"\\node-sight-askpass.exe";
    entries[L"SSH_ASKPASS_REQUIRE"] = L"force";
    entries[L"DISPLAY"] = L"node-sight";
    entries[L"RV_CREDENTIAL_TARGET"] = c.credentialTarget();
    std::vector<wchar_t> result;
    for (const auto& [k, v] : entries) {
        auto s = k + L"=" + v;
        result.insert(result.end(), s.begin(), s.end());
        result.push_back(0);
    }
    result.push_back(0);
    return result;
}
/** Load the embedded read-only shell collector and normalize CRLF so POSIX sh receives valid source. */
std::string script() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!r)
        return {};
    HGLOBAL mem = LoadResource(nullptr, r);
    if (!mem)
        return {};
    auto p = static_cast<const char*>(LockResource(mem));
    std::string s(p, SizeofResource(nullptr, r));
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}
} // namespace
/** Create a manual-reset cancellation event shared by the owner thread and the SSH worker. */
RemoteCollector::RemoteCollector() {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
/** Stop and join the worker before releasing its cancellation handle. */
RemoteCollector::~RemoteCollector() {
    stop();
    if (stopEvent_)
        CloseHandle(stopEvent_);
}
/** Cancel reconnect waits, kill only this collector's job, and join before any member state can be destroyed.
 */
void RemoteCollector::stop() {
    SetEvent(stopEvent_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job_)
            TerminateJobObject(job_, 1);
    }
    if (thread_.joinable())
        thread_.join();
}
/** Replace an existing sampling session safely; the worker receives its own immutable configuration copy. */
void RemoteCollector::start(const Config& config) {
    stop();
    ResetEvent(stopEvent_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = RemoteState{};
    }
    thread_ = std::thread(&RemoteCollector::run, this, config);
}
/** Copy the complete public state under one lock so rendering never observes half-updated counters. */
RemoteState RemoteCollector::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}
/** Own the SSH lifecycle, bounded line parser and reconnect loop. No shell collector is installed remotely.
 */
void RemoteCollector::run(Config c) {
    auto collector = script();
    if (collector.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.message = L"Coletor Linux ausente no executável.";
        return;
    }
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ssh = std::wstring(sys) + L"\\OpenSSH\\ssh.exe";
    if (GetFileAttributesW(ssh.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.message = L"Instale o Cliente OpenSSH do Windows.";
        return;
    }
    // Retry only after the previous attempt releases its handles.
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        // Child pipe ends inherit; parent-only ends are protected explicitly below.
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE readOut = nullptr, writeOut = nullptr, readIn = nullptr, writeIn = nullptr;
        if (!CreatePipe(&readOut, &writeOut, &sa, 0) || !CreatePipe(&readIn, &writeIn, &sa, 0)) {
            if (readOut)
                CloseHandle(readOut);
            if (writeOut)
                CloseHandle(writeOut);
            if (readIn)
                CloseHandle(readIn);
            if (writeIn)
                CloseHandle(writeIn);
            std::lock_guard<std::mutex> lock(mutex_);
            state_.message = L"Não foi possível criar o canal SSH.";
            return;
        }
        SetHandleInformation(readOut, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(writeIn, HANDLE_FLAG_INHERIT, 0);
        // Kill-on-close prevents orphaned SSH/askpass subprocesses.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job ||
            !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            if (job)
                CloseHandle(job);
            CloseHandle(readOut);
            CloseHandle(writeOut);
            CloseHandle(readIn);
            CloseHandle(writeIn);
            std::lock_guard<std::mutex> lock(mutex_);
            state_.message = L"Não foi possível isolar o processo SSH.";
            return;
        }
        // Check existence only; the helper supplies the saved secret directly to OpenSSH.
        PCREDENTIALW credential = nullptr;
        const bool password =
            CredReadW(c.credentialTarget().c_str(), CRED_TYPE_GENERIC, 0, &credential) != FALSE;
        if (credential) {
            SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
            CredFree(credential);
        }
        // Accept first-use host keys but reject changed known keys; never disable verification.
        std::wstring cmd =
            quote(ssh) +
            L" -T -o StrictHostKeyChecking=accept-new -o ConnectTimeout=6 -o ServerAliveInterval=5 -o "
            L"ServerAliveCountMax=2 -o LogLevel=ERROR -o ConnectionAttempts=1 -o NumberOfPasswordPrompts=1";
        cmd += password ? L" -o BatchMode=no" : L" -o BatchMode=yes";
        if (!c.key.empty())
            cmd += L" -o IdentitiesOnly=yes -i " + quote(c.key);
        cmd += L" -p " + std::to_wstring(c.port) + L" -l " + quote(c.user) + L" " + quote(c.host) +
               L" sh -s -- " + std::to_wstring(c.interval);
        auto env = environment(c);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = readIn;
        si.hStdOutput = writeOut;
        si.hStdError = writeOut;
        PROCESS_INFORMATION pi{};
        // Suspend until the process is assigned to our lifetime-scoped job.
        bool created = CreateProcessW(ssh.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                      env.data(), nullptr, &si, &pi) != FALSE;
        CloseHandle(readIn);
        CloseHandle(writeOut);
        if (created && !AssignProcessToJobObject(job, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            created = false;
        }
        if (created) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job_ = job;
                state_.connected = false;
                state_.message = L"Conectando via SSH...";
                state_.metrics.reset();
            }
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
                TerminateJobObject(job, 1);
            else
                ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            DWORD written = 0;
            // Stream the script through stdin; no file is installed on the Linux host.
            WriteFile(writeIn, collector.data(), static_cast<DWORD>(collector.size()), &written, nullptr);
            CloseHandle(writeIn);
            writeIn = nullptr;
            // Pipe chunks can contain partial/multiple frames; bound pending input and diagnostics.
            std::string pending;
            char buffer[2048];
            DWORD n = 0;
            while (ReadFile(readOut, buffer, sizeof(buffer), &n, nullptr) && n) {
                pending.append(buffer, n);
                size_t end;
                while ((end = pending.find('\n')) != std::string::npos) {
                    auto line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    RawSample sample;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (parseSample(line, monotonicSeconds(), sample)) {
                        state_.metrics.update(sample);
                        state_.lastSample = sample.time;
                        state_.connected = true;
                        state_.message = L"Conexão criptografada · SSH";
                    } else if (!line.empty()) {
                        state_.message = utf8ToWide(line.substr(0, 300));
                    }
                }
                if (pending.size() > 8192)
                    pending.clear();
                if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0)
                    break;
            }
            // If the server closes stdout without exiting, close its job too.
            TerminateJobObject(job, 0);
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
        }
        if (writeIn)
            CloseHandle(writeIn);
        CloseHandle(readOut);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = nullptr;
            state_.connected = false;
            if (!created)
                state_.message = L"Falha ao iniciar OpenSSH.";
            else if (state_.message == L"Conectando via SSH..." ||
                     state_.message == L"Conexão criptografada · SSH")
                state_.message = L"Servidor desconectado. Nova tentativa em 5s.";
            ++state_.reconnects;
        }
        CloseHandle(job);
        // Cancellation interrupts the reconnect delay immediately.
        if (WaitForSingleObject(stopEvent_, 5000) == WAIT_OBJECT_0)
            break;
    }
}
