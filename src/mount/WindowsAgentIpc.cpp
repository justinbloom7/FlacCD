#include "flaccd/mount/WindowsAgentIpc.h"
#include "flaccd/mount/MountAgent.h"
#include "flaccd/mount/AgentCommand.h"
#include "flaccd/mount/WindowsImgDriveHost.h"
#include "flaccd/mount/RestoreStore.h"
#include "flaccd/PathText.h"

#include <windows.h>
#include <sddl.h>
#include <dbt.h>

#include <algorithm>
#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace flaccd {
namespace {

class Handle {
public:
   explicit Handle(HANDLE handle = nullptr) : value(handle) {
   }
   ~Handle() {
      if (value && value != INVALID_HANDLE_VALUE) {
         CloseHandle(value);
      }
   }
   Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {
   }
   Handle(const Handle&) = delete;
   Handle& operator=(const Handle&) = delete;
   HANDLE get() const {
      return value;
   }
private:
   HANDLE value;
};

std::runtime_error failure(const char* operation) {
   return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")");
}

class PendingIo {
public:
   PendingIo(HANDLE pipeHandle, OVERLAPPED& pendingOperation) : pipe(pipeHandle), operation(pendingOperation) {
   }
   ~PendingIo() {
      // The OVERLAPPED/event must outlive cancellation even on exceptions.
      if (active) {
         CancelIoEx(pipe, &operation);
         DWORD count = 0;
         GetOverlappedResult(pipe, &operation, &count, TRUE);
      }
   }
   PendingIo(const PendingIo&) = delete;
   PendingIo& operator=(const PendingIo&) = delete;
   bool active = true;
private:
   HANDLE pipe;
   OVERLAPPED& operation;
};

std::wstring userSid(HANDLE process) {
   HANDLE rawToken = nullptr;
   if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
      throw failure("OpenProcessToken");
   }
   Handle token(rawToken);
   DWORD bytes = 0;
   GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
   std::vector<std::byte> buffer(bytes);
   if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes)) {
      throw failure("GetTokenInformation");
   }
   LPWSTR text = nullptr;
   if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
      throw failure("ConvertSidToStringSidW");
   }
   const std::wstring result(text);
   LocalFree(text);
   return result;
}

class Endpoint {
public:
   explicit Endpoint(const std::wstring& tag) : sid(userSid(GetCurrentProcess())) {
      if (tag.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::wstring::npos) {
         throw std::invalid_argument("invalid IPC test namespace");
      }
      const auto name = L"FlacCD-Agent-v1-" + sid + (tag.empty() ? L"" : L"-" + tag);
      pipe = L"\\\\.\\pipe\\" + name;
      mutex = L"Global\\" + name;
      // Protected DACL: no Everyone, anonymous, network, or other-user access.
      const auto sddl = L"D:P(A;;GA;;;" + sid + L")";
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
         throw failure("create current-user IPC security descriptor");
      }
      security = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
   }
   ~Endpoint() {
      LocalFree(descriptor);
   }
   Endpoint(const Endpoint&) = delete;
   Endpoint& operator=(const Endpoint&) = delete;
   std::wstring sid;
   std::wstring pipe;
   std::wstring mutex;
   SECURITY_ATTRIBUTES security{};
private:
   PSECURITY_DESCRIPTOR descriptor = nullptr;
};

void transfer(HANDLE pipe, bool write, char* bytes, std::size_t length, DWORD timeout, HANDLE stop = nullptr) {
   const auto deadline = GetTickCount64() + timeout;
   while (length > 0) {
      Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!event.get()) {
         throw failure("create IPC I/O event");
      }
      OVERLAPPED operation{};
      operation.hEvent = event.get();
      DWORD count = 0;
      const auto chunk = static_cast<DWORD>(std::min<std::size_t>(length, 65536));
      const bool complete = write ? WriteFile(pipe, bytes, chunk, &count, &operation) != 0 :
                                    ReadFile(pipe, bytes, chunk, &count, &operation) != 0;
      if (!complete) {
         if (GetLastError() != ERROR_IO_PENDING) {
            throw failure("IPC transfer; request outcome may be uncertain, inspect status before retrying");
         }
         const auto now = GetTickCount64();
         const HANDLE events[]{event.get(), stop};
         const auto remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
         const auto wait = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, remaining);
         if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            throw std::runtime_error("IPC timeout/shutdown; request is not replayed; inspect status after reconnecting");
         }
         if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) {
            throw failure("complete IPC transfer");
         }
      }
      if (count == 0) {
         throw std::runtime_error("agent pipe closed; outcome uncertain; request is not replayed");
      }
      bytes += count;
      length -= count;
   }
}

void send(HANDLE pipe, std::string text, DWORD timeout, HANDLE stop = nullptr) {
   if (text.size() > MAX_AGENT_MESSAGE) {
      throw std::invalid_argument("IPC message too large");
   }
   std::array<char, 4> size{};
   for (unsigned position = 0; position < 4; ++position) {
      size[position] = static_cast<char>((text.size() >> (position * 8)) & 255);
   }
   transfer(pipe, true, size.data(), size.size(), timeout, stop);
   transfer(pipe, true, text.data(), text.size(), timeout, stop);
}

std::string receive(HANDLE pipe, DWORD timeout, HANDLE stop = nullptr) {
   std::array<char, 4> header{};
   transfer(pipe, false, header.data(), header.size(), timeout, stop);
   std::uint32_t size = 0;
   for (unsigned position = 0; position < 4; ++position) {
      size |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[position])) << (position * 8);
   }
   if (size > MAX_AGENT_MESSAGE || size < 4) {
      throw std::invalid_argument("invalid IPC message size");
   }
   std::string text(size, '\0');
   transfer(pipe, false, text.data(), text.size(), timeout, stop);
   return text;
}

Handle verifyPeer(HANDLE pipe, const Endpoint& endpoint, bool server) {
   ULONG id = 0;
   const bool queried = server ? GetNamedPipeClientProcessId(pipe, &id) != 0 : GetNamedPipeServerProcessId(pipe, &id) != 0;
   if (!queried) {
      throw failure("query IPC peer");
   }
   Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id));
   if (!process.get()) {
      throw failure("open IPC peer process for identity/exit verification");
   }
   if (userSid(process.get()) != endpoint.sid) {
      throw std::runtime_error("IPC peer is not the current user");
   }
   return process;
}

bool agentInstanceExists(const Endpoint& endpoint) {
   Handle instance(OpenMutexW(SYNCHRONIZE, FALSE, endpoint.mutex.c_str()));
   if (instance.get()) {
      return true;
   }
   if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return false;
   }
   throw failure("query existing agent instance");
}

DWORD remainingTime(ULONGLONG deadline) {
   const auto now = GetTickCount64();
   return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

AgentReply waitForAgentExit(HANDLE process, ULONGLONG deadline) {
   const auto wait = WaitForSingleObject(process, remainingTime(deadline));
   if (wait == WAIT_TIMEOUT) {
      return {1, {}, "Agent process " + std::to_string(GetProcessId(process)) + " did not exit before the shutdown timeout. In-progress work may still finish; mounts/state are retained. No forced termination was performed.\n", {}};
   }
   if (wait != WAIT_OBJECT_0) {
      throw failure("wait for complete agent process exit");
   }
   DWORD code = 0;
   if (!GetExitCodeProcess(process, &code)) {
      throw failure("query exited agent status");
   }
   if (code != 0) {
      return {1, {}, "Agent exited with status " + std::to_string(code) + "; inspect its log. Saved mount state is retained.\n", {}};
   }
   return {0, "Agent stopped; active mounts/state retained. Eject monitoring resumes when the agent restarts.\n", {}, {}};
}

void startAgent(const std::filesystem::path& path,
                const std::filesystem::path& imgDrive,
                const std::filesystem::path& cache,
                const std::wstring& tag) {
   if (!path.is_absolute() || !std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("flaccd-agent.exe missing beside flaccd.exe; rebuild/install both executables");
   }
   std::wstring command = quoteWindowsArgument(path.wstring());
   for (const auto& argument : {std::wstring(L"--imgdrive"), imgDrive.wstring(), std::wstring(L"--cache-dir"), cache.wstring()}) {
      command += L" " + quoteWindowsArgument(argument);
   }
   if (!tag.empty()) {
      command += L" --endpoint-tag " + quoteWindowsArgument(tag);
   }
   if (command.size() >= 32767) {
      throw std::invalid_argument("agent startup command too long");
   }
   STARTUPINFOW startup{};
   startup.cb = sizeof(startup);
   PROCESS_INFORMATION process{};
   if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, path.parent_path().c_str(), &startup, &process)) {
      throw failure("start per-user FlacCD agent");
   }
   Handle processHandle(process.hProcess);
   Handle thread(process.hThread);
}

struct WindowContext {
   HANDLE changed;
   HANDLE stop;
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM parameter, LPARAM detail) {
   auto* context = reinterpret_cast<WindowContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
   if (message == WM_NCCREATE) {
      context = static_cast<WindowContext*>(reinterpret_cast<CREATESTRUCTW*>(detail)->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
   }
   if (context && message == WM_DEVICECHANGE &&
       (parameter == DBT_DEVICEARRIVAL || parameter == DBT_DEVICEREMOVECOMPLETE || parameter == DBT_DEVNODES_CHANGED)) {
      // Rescan all recorded PnP identities. Do not assume a notification's drive
      // mask still denotes the same device by the time the worker handles it.
      SetEvent(context->changed);
   }
   if (message == WM_QUERYENDSESSION) {
      return TRUE;
   }
   if (context && message == WM_ENDSESSION && parameter) {
      SetEvent(context->stop);
      PostQuitMessage(0);
   }
   if (message == WM_CLOSE) {
      if (context) {
         SetEvent(context->stop);
      }
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcW(window, message, parameter, detail);
}

void logMessage(const std::filesystem::path& cache, const std::string& message) {
   if (message.empty()) {
      return;
   }
   try {
      const auto path = cache / "agent.log";
      if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 1024) {
         std::error_code error;
         std::filesystem::remove(cache / "agent.previous.log", error);
         std::filesystem::rename(path, cache / "agent.previous.log", error);
      }
      std::ofstream output(path, std::ios::binary | std::ios::app);
      output << GetTickCount64() << " " << message << '\n';
   } catch (const std::exception&) {
      // Logging failure never grants permission to delete mount resources.
   }
}

}

AgentReply requestWindowsAgent(const std::vector<std::string>& arguments,
                               const std::filesystem::path& imgDrive,
                               const std::filesystem::path& cache,
                               const std::filesystem::path& agentExecutable,
                               const std::wstring& endpointTag,
                               std::chrono::milliseconds requestTimeout) {
   const auto normalized = normalizeAgentCommand(arguments);
   const auto stopTimeout = agentStopTimeout(normalized);
   Endpoint endpoint(endpointTag);
   const auto request = encodeAgentRequest(normalized);
   bool started = false;
   const bool bounded = stopTimeout.has_value() || requestTimeout.count() > 0;
   auto deadline = GetTickCount64() + (stopTimeout ? static_cast<ULONGLONG>(stopTimeout->count()) :
                                      requestTimeout.count() > 0 ? static_cast<ULONGLONG>(requestTimeout.count()) : 15000);
   bool queued = false;
   bool observedInstance = false;
   for (;;) {
      Handle pipe(CreateFileW(endpoint.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
      if (pipe.get() != INVALID_HANDLE_VALUE) {
         // Retain a verified process handle before submitting shutdown. PID
         // reuse or an already-exited process cannot invalidate the final wait.
         auto server = verifyPeer(pipe.get(), endpoint, false);
         try {
            send(pipe.get(), request, bounded ? remainingTime(deadline) : 10000);
            auto reply = decodeAgentReply(receive(pipe.get(), bounded ? remainingTime(deadline) : 1250000));
            send(pipe.get(), "FlacCD ack 1", bounded ? remainingTime(deadline) : 10000);
            if (stopTimeout && reply.exitCode == 0) {
               return waitForAgentExit(server.get(), deadline);
            }
            return reply;
         } catch (const std::exception&) {
            if (stopTimeout) {
               // Stop is idempotent. A lost reply/ack must not prevent exit or
               // turn a completed process exit into an ambiguous success.
               return waitForAgentExit(server.get(), deadline);
            }
            throw; // Never replay a submitted mount/unmount after disconnect.
         }
      }
      const auto error = GetLastError();
      if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
         throw failure("connect to current-user agent");
      }
      const bool instanceExists = agentInstanceExists(endpoint);
      if (stopTimeout && !instanceExists) {
         return {0, "Agent is not running; nothing to stop.\n", {}, {}};
      }
      if (!stopTimeout && observedInstance && !instanceExists) {
         throw std::runtime_error("Agent exited while this request was queued; no request was submitted. Retry explicitly after shutdown.");
      }
      observedInstance = observedInstance || instanceExists || error == ERROR_PIPE_BUSY;
      if (!bounded && error == ERROR_PIPE_BUSY && !queued) {
         deadline = GetTickCount64() + 1250000;
         queued = true;
      }
      if (!stopTimeout && !started && !instanceExists && error == ERROR_FILE_NOT_FOUND) {
         if (!normalized.empty() && normalized[0] == "restore-step") {
            WindowsImgDriveHost stateHost;
            auto lock = stateHost.lock(cache, std::chrono::milliseconds(remainingTime(deadline)));
            if (!lock) {
               throw std::runtime_error("cannot inspect restoration intent before starting the agent");
            }
            const auto desired = RestoreStore(cache).read();
            if (!desired.enabled || desired.mounts.empty()) {
               return {0, "No enabled desired mounts; agent not started.\n", {}, {}};
            }
            if (std::all_of(desired.mounts.begin(), desired.mounts.end(), [](const auto& entry) {
               return entry.suspended;
            })) {
               return {1, {}, "Restoration is suspended; agent not restarted.\n", {}};
            }
            startAgent(agentExecutable, imgDrive, cache, endpointTag);
         } else {
            startAgent(agentExecutable, imgDrive, cache, endpointTag);
         }
         started = true;
      }
      if (remainingTime(deadline) == 0) {
         if (stopTimeout) {
            throw std::runtime_error("Agent is busy/starting and the stop request could not be submitted before timeout; in-progress work and mount state were not interrupted. Retry agent stop with a longer --timeout.");
         }
         throw std::runtime_error("agent connection timed out; inspect cache/agent.log; no request was submitted");
      }
      WaitNamedPipeW(endpoint.pipe.c_str(), std::min<DWORD>(200, remainingTime(deadline)));
      Sleep(25);
   }
}

int runWindowsAgent(const std::filesystem::path& imgDrive,
                    const std::filesystem::path& cache,
                    const std::wstring& endpointTag) {
   Endpoint endpoint(endpointTag);
   Handle instance(CreateMutexW(&endpoint.security, FALSE, endpoint.mutex.c_str()));
   if (!instance.get()) {
      throw failure("create per-user agent mutex");
   }
   if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return 0;
   }
   // The instance reservation belongs to the process, not just the pipe loop.
   // Keep RAII ownership through main/CRT shutdown so a concurrent/repeated stop
   // cannot mistake a draining process for an absent agent and restart it.
   static std::unique_ptr<Handle> processInstance;
   processInstance = std::make_unique<Handle>(std::move(instance));
   WindowsImgDriveHost host;
   // Validate configuration/ownership before opening the IPC endpoint or log.
   { ImgDriveBackend validate(host, imgDrive, cache); }
   MountAgent agent(host, imgDrive, cache);
   Handle changed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
   Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
   if (!changed.get() || !stop.get()) {
      throw failure("create agent events");
   }
   const auto module = GetModuleHandleW(nullptr);
   WNDCLASSW windowClass{};
   windowClass.hInstance = module;
   windowClass.lpfnWndProc = windowProcedure;
   windowClass.lpszClassName = L"FlacCD Agent Notifications";
   if (!RegisterClassW(&windowClass)) {
      throw failure("register agent notification window");
   }
   WindowContext context{changed.get(), stop.get()};
   // A hidden top-level window receives default media broadcasts. A
   // message-only HWND_MESSAGE window would not receive these broadcasts.
   const auto window = CreateWindowExW(0, windowClass.lpszClassName, L"FlacCD Agent", WS_OVERLAPPED,
                                       0, 0, 0, 0, nullptr, nullptr, module, &context);
   if (!window) {
      throw failure("create agent notification window");
   }
   std::thread worker([&] {
      try {
         logMessage(cache, "Agent started; " + describeDrives(host.opticalDrives()));
         logMessage(cache, agent.reconcile());
         while (WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0) {
            Handle pipe(CreateNamedPipeW(endpoint.pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                          1, 65536, 65536, 0, &endpoint.security));
            if (pipe.get() == INVALID_HANDLE_VALUE) {
               throw failure("create agent named pipe");
            }
            Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!connected.get()) {
               throw failure("create pipe connection event");
            }
            OVERLAPPED connection{};
            connection.hEvent = connected.get();
            const bool immediate = ConnectNamedPipe(pipe.get(), &connection) != 0;
            const auto error = immediate ? ERROR_SUCCESS : GetLastError();
            if (!immediate && error != ERROR_PIPE_CONNECTED && error != ERROR_IO_PENDING) {
               throw failure("listen on agent pipe");
            }
            bool accepted = immediate || error == ERROR_PIPE_CONNECTED;
            PendingIo pending(pipe.get(), connection);
            pending.active = !accepted;
            while (!accepted) {
               const HANDLE events[]{stop.get(), connected.get(), changed.get()};
               const auto wait = WaitForMultipleObjects(3, events, FALSE, 5000);
               if (wait == WAIT_OBJECT_0) {
                  CancelIoEx(pipe.get(), &connection);
                  DWORD count = 0;
                  GetOverlappedResult(pipe.get(), &connection, &count, TRUE);
                  pending.active = false;
                  break;
               }
               if (wait == WAIT_OBJECT_0 + 1) {
                  DWORD count = 0;
                  if (!GetOverlappedResult(pipe.get(), &connection, &count, FALSE)) {
                     throw failure("accept agent connection");
                  }
                  accepted = true;
                  pending.active = false;
               } else if (wait == WAIT_TIMEOUT || wait == WAIT_OBJECT_0 + 2) {
                  ResetEvent(changed.get());
                  logMessage(cache, agent.reconcile());
               } else {
                  throw failure("wait on agent events");
               }
            }
            if (!accepted) {
               break;
            }
            bool shutdownAccepted = false;
            try {
               verifyPeer(pipe.get(), endpoint, true);
               std::vector<std::string> arguments;
               AgentReply reply;
               try {
                  arguments = decodeAgentRequest(receive(pipe.get(), 10000, stop.get()));
                  reply = agent.handle(arguments);
               } catch (const std::exception& problem) {
                  reply.exitCode = 2;
                  reply.diagnostics = problem.what();
               }
               shutdownAccepted = agent.isStopping();
               logMessage(cache, reply.diagnostics);
               const auto replyTimeout = shutdownAccepted ? 2000u : 10000u;
               const auto cancel = shutdownAccepted ? nullptr : stop.get();
               send(pipe.get(), encodeAgentReply(reply), replyTimeout, cancel);
               if (receive(pipe.get(), replyTimeout, cancel) != "FlacCD ack 1") {
                  throw std::runtime_error("invalid IPC reply acknowledgment");
               }
            } catch (const std::exception& problem) {
               logMessage(cache, problem.what());
            }
            DisconnectNamedPipe(pipe.get());
            if (shutdownAccepted || WaitForSingleObject(stop.get(), 0) == WAIT_OBJECT_0) {
               // The reply is best-effort: a client crash/lost ACK cannot veto
               // accepted shutdown. Never reconcile or delete resources here.
               SetEvent(stop.get());
               PostMessageW(window, WM_CLOSE, 0, 0);
               break;
            }
            // Reconcile even under sustained client traffic, not only during
            // idle waits. The coordinator enforces the five-second debounce.
            logMessage(cache, agent.reconcile());
         }
      } catch (const std::exception& problem) {
         logMessage(cache, problem.what());
         PostMessageW(window, WM_CLOSE, 0, 0);
      }
   });
   MSG message{};
   while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
   }
   SetEvent(stop.get());
   worker.join();
   DestroyWindow(window);
   UnregisterClassW(windowClass.lpszClassName, module);
   return 0;
}

}
