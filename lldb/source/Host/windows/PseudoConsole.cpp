//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/windows/PseudoConsole.h"

#include <cstdio>
#include <mutex>

#include "lldb/Host/windows/PipeWindows.h"
#include "lldb/Host/windows/windows.h"
#include "lldb/Utility/LLDBLog.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Errno.h"

using namespace lldb_private;

typedef HRESULT(WINAPI *CreatePseudoConsole_t)(COORD size, HANDLE hInput,
                                               HANDLE hOutput, DWORD dwFlags,
                                               HPCON *phPC);

typedef VOID(WINAPI *ClosePseudoConsole_t)(HPCON hPC);

static constexpr DWORD PSEUDOCONSOLE_INHERIT_CURSOR = 0x1;

struct Kernel32 {
  Kernel32() {
    hModule = LoadLibraryW(L"kernel32.dll");
    if (!hModule) {
      llvm::Error err = llvm::errorCodeToError(
          std::error_code(GetLastError(), std::system_category()));
      LLDB_LOG_ERROR(GetLog(LLDBLog::Host), std::move(err),
                     "Could not load kernel32: {0}");
      return;
    }
    CreatePseudoConsole_ =
        (CreatePseudoConsole_t)GetProcAddress(hModule, "CreatePseudoConsole");
    ClosePseudoConsole_ =
        (ClosePseudoConsole_t)GetProcAddress(hModule, "ClosePseudoConsole");
    isAvailable = (CreatePseudoConsole_ && ClosePseudoConsole_);
  }

  HRESULT CreatePseudoConsole(COORD size, HANDLE hInput, HANDLE hOutput,
                              DWORD dwFlags, HPCON *phPC) {
    assert(CreatePseudoConsole_ && "CreatePseudoConsole is not available!");
    return CreatePseudoConsole_(size, hInput, hOutput, dwFlags, phPC);
  }

  VOID ClosePseudoConsole(HPCON hPC) {
    assert(ClosePseudoConsole_ && "ClosePseudoConsole is not available!");
    return ClosePseudoConsole_(hPC);
  }

  bool IsConPTYAvailable() { return isAvailable; }

private:
  HMODULE hModule;
  CreatePseudoConsole_t CreatePseudoConsole_;
  ClosePseudoConsole_t ClosePseudoConsole_;
  bool isAvailable;
};

static Kernel32 kernel32;

llvm::Error PseudoConsole::OpenPseudoConsole() {
  if (!kernel32.IsConPTYAvailable())
    return llvm::make_error<llvm::StringError>("ConPTY is not available",
                                               llvm::errc::io_error);
  HRESULT hr;
  HANDLE hInputRead = INVALID_HANDLE_VALUE;
  HANDLE hInputWrite = INVALID_HANDLE_VALUE;
  HANDLE hOutputRead = INVALID_HANDLE_VALUE;
  HANDLE hOutputWrite = INVALID_HANDLE_VALUE;

  wchar_t pipe_name[MAX_PATH];
  swprintf(pipe_name, MAX_PATH, L"\\\\.\\pipe\\conpty-lldb-%d-%p",
           GetCurrentProcessId(), this);

  // A 4096 bytes buffer should be large enough for the majority of console
  // burst outputs.
  hOutputRead =
      CreateNamedPipeW(pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                       PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, NULL);
  hOutputWrite = CreateFileW(pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);

  if (!CreatePipe(&hInputRead, &hInputWrite, NULL, 0))
    return llvm::errorCodeToError(
        std::error_code(GetLastError(), std::system_category()));

  // Default cursor position: last row so ConPTY won't scroll back over
  // existing output if we can't query the real console.
  COORD consoleSize{80, 25};
  int cursorRow = consoleSize.Y;
  int cursorCol = 1;
  CONSOLE_SCREEN_BUFFER_INFO csbi;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
    consoleSize = {
        static_cast<SHORT>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
        static_cast<SHORT>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)};
    cursorRow = csbi.dwCursorPosition.Y - csbi.srWindow.Top + 1;
    cursorCol = csbi.dwCursorPosition.X + 1;
  }
  HPCON hPC = INVALID_HANDLE_VALUE;
  // PSEUDOCONSOLE_INHERIT_CURSOR prevents ConPTY from emitting screen-clear
  // sequences when the first process attaches.
  hr = kernel32.CreatePseudoConsole(consoleSize, hInputRead, hOutputWrite,
                                    PSEUDOCONSOLE_INHERIT_CURSOR, &hPC);
  CloseHandle(hInputRead);
  CloseHandle(hOutputWrite);

  if (FAILED(hr)) {
    CloseHandle(hInputWrite);
    CloseHandle(hOutputRead);
    return llvm::make_error<llvm::StringError>(
        "Failed to create Windows ConPTY pseudo terminal",
        llvm::errc::io_error);
  }

  DWORD mode = PIPE_NOWAIT;
  SetNamedPipeHandleState(hOutputRead, &mode, NULL, NULL);

  m_conpty_handle = hPC;
  m_conpty_output = hOutputRead;
  m_conpty_input = hInputWrite;

  // PSEUDOCONSOLE_INHERIT_CURSOR causes ConPTY to emit \x1b[6n on the output
  // pipe to query the cursor position before finishing init. Write the
  // response so ConPTY can complete initialization without clearing the screen.
  char response[32];
  int rlen =
      snprintf(response, sizeof(response), "\x1b[%d;%dR", cursorRow, cursorCol);
  if (rlen > 0) {
    DWORD nwritten = 0;
    WriteFile(m_conpty_input, response, static_cast<DWORD>(rlen), &nwritten,
              NULL);
  }

  return llvm::Error::success();
}

void PseudoConsole::Close() {
  if (m_conpty_handle != INVALID_HANDLE_VALUE)
    kernel32.ClosePseudoConsole(m_conpty_handle);
  CloseHandle(m_conpty_input);
  CloseHandle(m_conpty_output);
  m_conpty_handle = INVALID_HANDLE_VALUE;
  m_conpty_input = INVALID_HANDLE_VALUE;
  m_conpty_output = INVALID_HANDLE_VALUE;
}
