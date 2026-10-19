/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/// \addtogroup Acored
/// @{
/// \file

#include "CliRunnable.h"
#include "Config.h"
#include "ObjectMgr.h"
#include "World.h"
#include <fmt/core.h>

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
#include <windows.h>
#include <iostream>
#else
#include "Chat.h"
#include "ChatCommand.h"
#include <cstring>
#include <readline/history.h>
#include <readline/readline.h>
#endif

static constexpr char CLI_PREFIX[] = "AC> ";

static inline void PrintCliPrefix()
{
    fmt::print(CLI_PREFIX);
}

#if AC_PLATFORM != AC_PLATFORM_WINDOWS
namespace Acore::Impl::Readline
{
    static std::vector<std::string> vec;
    char* cli_unpack_vector(char const*, int state)
    {
        static std::size_t i=0;
        if (!state)
            i = 0;
        if (i < vec.size())
            return strdup(vec[i++].c_str());
        else
            return nullptr;
    }

    char** cli_completion(char const* text, int /*start*/, int /*end*/)
    {
        ::rl_attempted_completion_over = 1;
        vec = Acore::ChatCommands::GetAutoCompletionsFor(CliHandler(nullptr,nullptr), text);
        return ::rl_completion_matches(text, &cli_unpack_vector);
    }

    int cli_hook_func()
    {
           if (World::IsStopped())
               ::rl_done = 1;
           return 0;
    }
}
#endif

void utf8print(void* /*arg*/, std::string_view str)
{
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    fmt::print("{}", str);
#else
{
    fmt::print("{}", str);
    fflush(stdout);
}
#endif
}

void commandFinished(void*, bool /*success*/)
{
    PrintCliPrefix();
    fflush(stdout);
}

#ifdef linux
// Non-blocking keypress detector, when return pressed, return 1, else always return 0
int kb_hit_return()
{
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    select(STDIN_FILENO+1, &fds, nullptr, nullptr, &tv);
    return FD_ISSET(STDIN_FILENO, &fds);
}
#endif

/// %Thread start
void CliThread()
{
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    // Set console code pages to UTF-8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    // print this here the first time
    // later it will be printed after command queue updates
    PrintCliPrefix();
#else
    ::rl_attempted_completion_function = &Acore::Impl::Readline::cli_completion;
    {
        static char BLANK = '\0';
        ::rl_completer_word_break_characters = &BLANK;
    }
    ::rl_event_hook = &Acore::Impl::Readline::cli_hook_func;
#endif

    if (sConfigMgr->GetOption<bool>("BeepAtStart", true))
        printf("\a"); // \a = Alert

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    if (sConfigMgr->GetOption<bool>("FlashAtStart", true))
    {
        FLASHWINFO fInfo;
        fInfo.cbSize = sizeof(FLASHWINFO);
        fInfo.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        fInfo.hwnd = GetConsoleWindow();
        fInfo.uCount = 0;
        fInfo.dwTimeout = 0;
        FlashWindowEx(&fInfo);
    }

    // Get console input handle once for reading commands
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("server.worldserver", "Failed to get console input handle");
        return;
    }
#endif

    ///- As long as the World is running (no World::m_stopEvent), get the command line and handle it
    while (!World::IsStopped())
    {
        fflush(stdout);

        std::string command;

#if AC_PLATFORM == AC_PLATFORM_WINDOWS

        static bool checkedConsole = false;
        static bool isRealConsole = false;
        static std::wstring lineBuf;

        if (!checkedConsole)
        {
            DWORD mode = 0;
            isRealConsole = GetConsoleMode(hStdIn, &mode);
            if (isRealConsole)
            {
                // Line-mode ReadConsoleW holds the console lock until Enter; worker logs then deadlock
                // with the CLI thread, so Enter never completes and shutdown cannot join this thread.
                mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
                mode |= ENABLE_PROCESSED_INPUT;
                SetConsoleMode(hStdIn, mode);
            }
            checkedConsole = true;
        }

        if (isRealConsole)
        {
            if (WaitForSingleObject(hStdIn, 200) != WAIT_OBJECT_0)
                continue;

            INPUT_RECORD recs[32];
            DWORD recsRead = 0;
            if (!ReadConsoleInputW(hStdIn, recs, 32, &recsRead))
                continue;

            bool lineReady = false;
            for (DWORD i = 0; i < recsRead; ++i)
            {
                if (recs[i].EventType != KEY_EVENT || !recs[i].Event.KeyEvent.bKeyDown)
                    continue;
                WCHAR const ch = recs[i].Event.KeyEvent.uChar.UnicodeChar;
                if (recs[i].Event.KeyEvent.wVirtualKeyCode == VK_RETURN || ch == L'\r')
                {
                    lineReady = true;
                    break;
                }
                if (ch == L'\b')
                {
                    if (!lineBuf.empty())
                    {
                        lineBuf.pop_back();
                        DWORD written = 0;
                        wchar_t const erase[] = L"\b \b";
                        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), erase, 3, &written, nullptr);
                    }
                    continue;
                }
                if (ch)
                {
                    lineBuf.push_back(ch);
                    DWORD written = 0;
                    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &ch, 1, &written, nullptr);
                }
            }

            if (!lineReady)
                continue;

            fmt::print("\n");
            if (!lineBuf.empty() && !WStrToUtf8(lineBuf, command))
            {
                lineBuf.clear();
                PrintCliPrefix();
                continue;
            }
            lineBuf.clear();
        }
        else
        {
            // Redirected input (pipe): cannot block forever or CTRL+C never joins this thread.
            if (WaitForSingleObject(hStdIn, 200) != WAIT_OBJECT_0)
                continue;
            if (!std::getline(std::cin, command))
            {
                if (World::IsStopped())
                    break;
                World::StopNow(SHUTDOWN_EXIT_CODE);
                break;
            }
        }

#else
        char* command_str = readline(CLI_PREFIX);
        ::rl_bind_key('\t', ::rl_complete);
        if (command_str != nullptr)
        {
            command = command_str;
            free(command_str);
        }
#endif

        if (!command.empty())
        {
            std::size_t nextLineIndex = command.find_first_of("\r\n");
            if (nextLineIndex != std::string::npos)
            {
                if (nextLineIndex == 0)
                {
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
                    PrintCliPrefix();
#endif
                    continue;
                }

                command.erase(nextLineIndex);
            }

            fflush(stdout);
            sWorld->QueueCliCommand(new CliCommandHolder(nullptr, command.c_str(), &utf8print, &commandFinished));
#if AC_PLATFORM != AC_PLATFORM_WINDOWS
            add_history(command.c_str());
#endif
        }
        else if (feof(stdin))
        {
            World::StopNow(SHUTDOWN_EXIT_CODE);
        }
    }
}
