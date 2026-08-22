/*---------------------------------------------------------*\
| AutoStart-Windows.cpp                                     |
|                                                           |
|   Autostart implementation for Windows                    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <fstream>
#include <iostream>
#include <shlobj.h>
#include <vector>
#include "AutoStart-Windows.h"
#include "LogManager.h"
#include "filesystem.h"
#include "windows.h"

namespace
{
bool RunHiddenProcess(const std::wstring& executable, const std::wstring& arguments)
{
    std::wstring command_line = L"\"" + executable + L"\" " + arguments;
    std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};

    if(!CreateProcessW(executable.c_str(), command_buffer.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info))
    {
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 10000);
    DWORD exit_code = ERROR_GEN_FAILURE;
    if(wait_result == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(process_info.hProcess, &exit_code);
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return wait_result == WAIT_OBJECT_0 && exit_code == ERROR_SUCCESS;
}

bool RunScheduledTaskCommand(const std::wstring& arguments)
{
    wchar_t system_path[MAX_PATH] = {};
    if(GetSystemDirectoryW(system_path, MAX_PATH) == 0)
    {
        return false;
    }

    return RunHiddenProcess(std::wstring(system_path) + L"\\schtasks.exe", arguments);
}

bool ConfigureScheduledTask(const std::wstring& task_name)
{
    wchar_t system_path[MAX_PATH] = {};
    if(GetSystemDirectoryW(system_path, MAX_PATH) == 0)
    {
        return false;
    }

    std::wstring escaped_task_name;
    for(const wchar_t character : task_name)
    {
        escaped_task_name += character == L'\'' ? L"''" : std::wstring(1, character);
    }

    const std::wstring powershell = std::wstring(system_path)
                                  + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring command =
        L"-NoProfile -NonInteractive -WindowStyle Hidden -Command \""
        L"$task = Get-ScheduledTask -TaskName '" + escaped_task_name + L"'; "
        L"$task.Settings.ExecutionTimeLimit = 'PT0S'; "
        L"$task.Settings.DisallowStartIfOnBatteries = $false; "
        L"$task.Settings.StopIfGoingOnBatteries = $false; "
        L"$task.Settings.StartWhenAvailable = $true; "
        L"$task.Settings.MultipleInstances = 'IgnoreNew'; "
        L"Set-ScheduledTask -InputObject $task | Out-Null\"";

    return RunHiddenProcess(powershell, command);
}

std::wstring QuoteScheduledTaskArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\\\"";
    for(const wchar_t character : argument)
    {
        if(character == L'\"')
        {
            quoted += L"\\\\\\\"";
        }
        else
        {
            quoted += character;
        }
    }
    quoted += L"\\\"";
    return quoted;
}
}

AutoStart::AutoStart(std::string name)
{
    InitAutoStart(name);
}

bool AutoStart::DisableAutoStart()
{
    std::error_code autostart_file_remove_errcode;
    const std::wstring task_name = utf8_decode(autostart_name);
    const bool task_removed = RunScheduledTaskCommand(
        L"/Delete /TN \"" + task_name + L"\" /F");
    bool shortcut_removed = true;

    /*-----------------------------------------------------*\
    | Check if the filename is valid                        |
    \*-----------------------------------------------------*/
    if(autostart_file != "")
    {
        /*-------------------------------------------------*\
        | If file doesn't exist, disable is successful      |
        \*-------------------------------------------------*/
        if(!filesystem::exists(autostart_file))
        {
            shortcut_removed = true;
        }
        /*-------------------------------------------------*\
        | Otherwise, delete the file                        |
        \*-------------------------------------------------*/
        else
        {
            shortcut_removed = filesystem::remove(autostart_file, autostart_file_remove_errcode);

            if(!shortcut_removed)
            {
                LOG_ERROR("[AutoStart] An error occurred removing the auto start file.");
            }
        }
    }
    else
    {
        LOG_ERROR("[AutoStart] Could not establish correct autostart file path.");
    }

    /* A missing task also means autostart is disabled. */
    const bool task_absent = !RunScheduledTaskCommand(
        L"/Query /TN \"" + task_name + L"\"");
    return shortcut_removed && (task_removed || task_absent);
}

bool AutoStart::EnableAutoStart(AutoStartInfo autostart_info)
{
    const std::wstring task_name = utf8_decode(autostart_name);
    const std::wstring executable = utf8_decode(autostart_info.path);
    const std::wstring arguments = utf8_decode(autostart_info.args);
    const std::wstring task_command = QuoteScheduledTaskArgument(executable)
                                    + (arguments.empty() ? L"" : L" " + arguments);

    const bool task_created = RunScheduledTaskCommand(
        L"/Create /TN \"" + task_name + L"\" /SC ONLOGON /RL HIGHEST /F /TR \""
        + task_command + L"\"");
    const bool success = task_created && ConfigureScheduledTask(task_name);

    if(success && !autostart_file.empty() && filesystem::exists(autostart_file))
    {
        std::error_code remove_error;
        if(!filesystem::remove(autostart_file, remove_error))
        {
            LOG_WARNING("[AutoStart] Elevated scheduled task created, but the legacy startup shortcut could not be removed.");
        }
    }
    else if(!success)
    {
        LOG_ERROR("[AutoStart] Could not create the elevated Windows logon task.");
    }

    return success;
}

bool AutoStart::IsAutoStartEnabled()
{
    return RunScheduledTaskCommand(
        L"/Query /TN \"" + utf8_decode(autostart_name) + L"\"");
}

std::string AutoStart::GetExePath()
{
    /*-----------------------------------------------------*\
    | Create the OpenRGB executable path                    |
    \*-----------------------------------------------------*/
    char exepath[MAX_PATH] = "";

    DWORD count = GetModuleFileNameA(NULL, exepath, MAX_PATH);

    return(std::string(exepath, (count > 0) ? count : 0));
}

/*---------------------------------------------------------*\
| Windows AutoStart Implementation                          |
| Private Methods                                           |
\*---------------------------------------------------------*/

void AutoStart::InitAutoStart(std::string name)
{
    char startMenuPath[MAX_PATH];

    autostart_name = name;

    /*-----------------------------------------------------*\
    | Get startup applications path                         |
    \*-----------------------------------------------------*/
    HRESULT result = SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, startMenuPath);

    if(SUCCEEDED(result))
    {
        autostart_file = std::string(startMenuPath);

        autostart_file += "\\Startup\\" + autostart_name + ".lnk";
    }
    else
    {
        autostart_file.clear();
    }
}

/*---------------------------------------------------------*\
| Convert an UTF8 string to a wide Unicode String           |
| (from wmi.cpp)                                            |
\*---------------------------------------------------------*/
std::wstring AutoStart::utf8_decode(const std::string& str)
{
    if(str.empty())
    {
        return std::wstring();
    }

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), nullptr, 0);

    std::wstring wstrTo(size_needed, 0);

    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int) str.size(), &wstrTo[0], size_needed);

    return(wstrTo);
}
