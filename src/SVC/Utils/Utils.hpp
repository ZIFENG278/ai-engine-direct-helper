//==============================================================================
//
// Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
// 
// SPDX-License-Identifier: BSD-3-Clause
//
//==============================================================================

#pragma once

#ifndef _LIBAPPBUILDER_UTILS_H
#define _LIBAPPBUILDER_UTILS_H


#include <tchar.h>
#include <sstream>

#include "Utils/ShareMem.hpp"

#define GLOBAL_BUFSIZE      4096

#ifdef UNICODE  
#define SVC_APPBUILDER_CMD   TEXT("QAIAppSvc.exe svc %llu %llu %llu %d %d \"%S\"")
#else  
#define SVC_APPBUILDER_CMD   TEXT("QAIAppSvc.exe svc %llu %llu %llu %d %d \"%s\"")
#endif

uint64_t g_logEpoch = 0;
int g_logLevel = 0;
int g_profilingLevel = 0;
std::string g_ProcName = "^main";

char g_buffer[GLOBAL_BUFSIZE];

typedef struct ProcInfo {
    HANDLE hSvcPipeInWrite;
    HANDLE hSvcPipeOutRead;
    PROCESS_INFORMATION piSvcProcInfo;
} ProcInfo_t;

std::unordered_map<std::string, ProcInfo_t*> sg_proc_info_map;      // proc_name map to ProcInfo_t.
std::unordered_map<std::string, ProcInfo_t*> sg_model_info_map;     // model_name map to ProcInfo_t.

std::string GetLastErrorAsString(std::string message) {
    DWORD errorMessageID = ::GetLastError();
    if (errorMessageID == 0)
        return std::string();

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
    std::string result(messageBuffer, size);
    LocalFree(messageBuffer);

    return message + " Error: [" + std::to_string(errorMessageID) + "] " + result;
}

void ErrorExit(std::string message) {
    QNN_ERR(GetLastErrorAsString(message).c_str());
    ExitProcess(1);
}

void split_string(std::vector<std::string> & output, const std::string &input, const char separator) {
  std::istringstream tokenStream(input);
  while (!tokenStream.eof()) {
    std::string value;
    getline(tokenStream, value, separator);
    if (!value.empty()) {
        output.push_back(value);
    }
  }
}

ProcInfo_t* FindProcInfo(std::string proc_name) {
    auto it = sg_proc_info_map.find(proc_name);
    if (it != sg_proc_info_map.end()) {
        if (it->second) {
            auto pProcInfo = it->second;
            return pProcInfo;
        }
    }

    return nullptr;
}

ProcInfo_t* CreateSvcProcess(std::string proc_name) {
    STARTUPINFO siStartInfo;
    PROCESS_INFORMATION piSvcProcInfo;
    ProcInfo_t* pProcInfo = nullptr;
    BOOL bSuccess = FALSE;

    HANDLE hSvcPipeInRead = NULL;
    HANDLE hSvcPipeInWrite = NULL;
    HANDLE hSvcPipeOutRead = NULL;
    HANDLE hSvcPipeOutWrite = NULL;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hSvcPipeOutRead, &hSvcPipeOutWrite, &saAttr, 0))
        ErrorExit("Create out pipe failed.");

    if (!SetHandleInformation(hSvcPipeOutRead, HANDLE_FLAG_INHERIT, 0))
        ErrorExit("SetHandleInformation for out pipe failed.");

    if (!CreatePipe(&hSvcPipeInRead, &hSvcPipeInWrite, &saAttr, 0))
        ErrorExit("Create in pipe failed");

    if (!SetHandleInformation(hSvcPipeInWrite, HANDLE_FLAG_INHERIT, 0))
        ErrorExit("SetHandleInformation for in pipe failed.");

    ZeroMemory(&piSvcProcInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));

    siStartInfo.cb = sizeof(STARTUPINFO);

    _stprintf_s((TCHAR*)g_buffer, GLOBAL_BUFSIZE, SVC_APPBUILDER_CMD, (uint64_t)hSvcPipeInRead, (uint64_t)hSvcPipeOutWrite, 
                g_logEpoch, g_logLevel, g_profilingLevel, proc_name.c_str());

    bSuccess = CreateProcess(NULL, (TCHAR*)g_buffer, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piSvcProcInfo);

    if (!bSuccess) {
        ErrorExit("CreateProcess failed.");
    }
    else {
        CloseHandle(hSvcPipeOutWrite);
        CloseHandle(hSvcPipeInRead);

        ProcInfo_t* pProcInfo = (ProcInfo_t*)malloc(sizeof(ProcInfo_t));
        pProcInfo->hSvcPipeOutRead = hSvcPipeOutRead;
        pProcInfo->hSvcPipeInWrite = hSvcPipeInWrite;
        pProcInfo->piSvcProcInfo = piSvcProcInfo;

        sg_proc_info_map.insert(std::make_pair(proc_name, pProcInfo));

        QNN_INF("CreateSvcProcess Success!");
        return pProcInfo;
    }

    return nullptr;
}

BOOL StopSvcProcess(std::string proc_name) {
    ProcInfo_t* pProcInfo = FindProcInfo(proc_name);
    if (!pProcInfo) {
        QNN_ERR("TalkToSvc_Inference::Cant find this process %s.\n", proc_name.c_str());
        return false;
    }

    CloseHandle(pProcInfo->piSvcProcInfo.hProcess);
    CloseHandle(pProcInfo->piSvcProcInfo.hThread);
    CloseHandle(pProcInfo->hSvcPipeInWrite);        // This will close pipe write for Svc process, it will exit.
    CloseHandle(pProcInfo->hSvcPipeOutRead);
    sg_proc_info_map.erase(proc_name);
    free(pProcInfo);
    return true;
}

// Send model data to the Svc through share meoory and receive model generated data from share memory.
BOOL TalkToSvc_Initialize(const std::string& model_name, const std::string& proc_name, const std::string& model_path,
                          const std::string& backend_lib_path, const std::string& system_lib_path) {
    ProcInfo_t* pProcInfo = FindProcInfo(proc_name);
    if (!pProcInfo) {
        pProcInfo = CreateSvcProcess(proc_name);

        if (!pProcInfo) return false;
    }

    HANDLE hSvcPipeInWrite = pProcInfo->hSvcPipeInWrite;
    HANDLE hSvcPipeOutRead = pProcInfo->hSvcPipeOutRead;
    DWORD dwRead = 0, dwWrite = 0;
    BOOL bSuccess;

    std::string command = "l" + model_name + ";" + model_path + ";" + backend_lib_path + ";" + system_lib_path;
    dwRead = (DWORD)command.length() + 1;

    TimerHelper timerHelper;
    // Write command to Svc.
    bSuccess = WriteFile(hSvcPipeInWrite, command.c_str(), dwRead, &dwWrite, NULL);
    // QNN_INF("TalkToSvc_Initialize::WriteToPipe: %s dwRead = %d dwWrite = %d\n", command.c_str(), dwRead, dwWrite);
    if (!bSuccess) return false;

    // Read command from Svc.
    bSuccess = ReadFile(hSvcPipeOutRead, g_buffer, GLOBAL_BUFSIZE, &dwRead, NULL);
    if(dwRead) {
        g_buffer[dwRead] = 0;
        QNN_INF("TalkToSvc_Initialize::ReadFromPipe: %s dwRead = %d\n", g_buffer, dwRead);
    }
    else {
        QNN_ERR("TalkToSvc_Initialize::ReadFromPipe: Failed to read from hSvcPipeOutRead, perhaps child process died.\n");
    }
    if (!bSuccess || dwRead == 0) return false;
    timerHelper.Print("TalkToSvc_Initialize::Pipe talk");

    // Add "model_name" to "sg_model_info_map".
    sg_model_info_map.insert(std::make_pair(model_name, pProcInfo));

    return bSuccess;
}

BOOL TalkToSvc_Destroy(std::string model_name, std::string proc_name) {
    ProcInfo_t* pProcInfo = FindProcInfo(proc_name);
    if (!pProcInfo) {
        QNN_ERR("TalkToSvc_Destroy::Cant find this process %s.\n", proc_name.c_str());
        return false;
    }

    HANDLE hSvcPipeInWrite = pProcInfo->hSvcPipeInWrite;
    HANDLE hSvcPipeOutRead = pProcInfo->hSvcPipeOutRead;
    DWORD dwRead = 0, dwWrite = 0;
    BOOL bSuccess;

    std::string command = "r" + model_name;
    dwRead = (DWORD)command.length() + 1;

    TimerHelper timerHelper;
    // Write command to Svc.
    bSuccess = WriteFile(hSvcPipeInWrite, command.c_str(), dwRead, &dwWrite, NULL);
    QNN_INF("TalkToSvc_Destroy::WriteToPipe: %s dwRead = %d dwWrite = %d\n", command.c_str(), dwRead, dwWrite);
    if (!bSuccess) return false;

    // Read command from Svc.
    bSuccess = ReadFile(hSvcPipeOutRead, g_buffer, GLOBAL_BUFSIZE, &dwRead, NULL);
    if (dwRead) {
        g_buffer[dwRead] = 0;
        QNN_INF("TalkToSvc_Destroy::ReadFromPipe: %s dwRead = %d\n", g_buffer, dwRead);
    }
    else {
        QNN_ERR("TalkToSvc_Destroy::ReadFromPipe: Failed to read from hSvcPipeOutRead, perhaps child process died.\n");
    }
    if (!bSuccess || dwRead == 0) return false;
    timerHelper.Print("TalkToSvc_Destroy::Pipe talk");

    sg_model_info_map.erase(model_name);
    if(sg_model_info_map.size() == 0) {     // If no model in this process, stop this process.
        QNN_INF("TalkToSvc_Destroy::StopSvcProcess.\n");
        StopSvcProcess(proc_name);
    }

    return bSuccess;
}

// The format of strStringSize: "124,3333,434343,132", included the inputSize content.
void ShareMemToVector(std::string strBufferArray, uint8_t* lpBase, std::vector<uint8_t*>& buffers, std::vector<size_t>& size) {
    std::vector<std::string> strArray;
    std::vector<std::string> strOffsetArray;
    std::vector<std::string> strSizeArray;
    split_string(strArray, strBufferArray, '=');
    split_string(strOffsetArray, strArray[0], ',');
    split_string(strSizeArray, strArray[1], ',');

    size_t offset = 0;
    size_t dataSize = 0;

    // Perhaps the data in buffer is not in order.
    for (int i = 0; i < strOffsetArray.size(); i++) {
        offset = std::stoull(strOffsetArray[i]);
        dataSize = std::stoull(strSizeArray[i]);
        size.push_back(dataSize);
        buffers.push_back(reinterpret_cast<uint8_t*>(lpBase + offset));
    }
}

// Copy data to 'pShareMemInfo->lpBase'. If the data in 'buffers' has been in the area of share memory, don't copy.
std::pair<std::string, std::string> VectorToShareMem(size_t share_memory_size, uint8_t* lpBase, std::vector<uint8_t*>& buffers, std::vector<size_t>& size) {
    QNN_INF("VectorToShareMem Start. size = %llu\n", share_memory_size);
    //TimerHelper timerHelper;

    std::string strOffsetArray = "";
    std::string strSizeArray = "";
    size_t offset = 0;
    size_t dataSize = 0;
    uint8_t* buffer = nullptr;

    // How to handle the case - part of the data in buffers are in the share memory?
    // Calculate the offset, avoid overflow the input data which already in the share memory.
    for (int i = 0; i < buffers.size(); i++) {
        buffer = buffers[i];
        if (buffer >= lpBase && buffer <= lpBase + share_memory_size) {     // This buffer is in the share memory area.
            offset += size[i];
        }
    }

    // Copy the data which is not in share memory to share memory.
    for (int i = 0; i < buffers.size(); i++) {
        buffer = buffers[i];
        dataSize = size[i];
        if (buffer >= lpBase && buffer <= lpBase + share_memory_size) {     // This buffer is in the share memory area.
            strOffsetArray += std::to_string(buffer - lpBase) + ",";
            //QNN_INF("VectorToShareMem in buffers, ignore copy.\n");
        }
        else {
            memcpy((uint8_t*)lpBase + offset, buffers[i], dataSize);        // This buffer is NOT in the share memory area, copy it.
            strOffsetArray += std::to_string(offset) + ",";
            offset += dataSize;
            //QNN_INF("VectorToShareMem NOT in buffers, copy...\n");
        }
        strSizeArray += std::to_string(dataSize) + ",";
    }

    //timerHelper.Print("VectorToShareMem::offset = " + std::to_string(offset));
    // QNN_INF("VectorToShareMem End.\n");
    // QNN_INF("VectorToShareMem::strOffsetArray = %s.\n", strOffsetArray.c_str());
    return std::make_pair(strOffsetArray, strSizeArray);
}

// Send model data to the Svc through share memory and receive model generated data from share memory.
BOOL TalkToSvc_Inference(std::string model_name, std::string proc_name, std::string share_memory_name, 
                         std::vector<uint8_t*>& inputBuffers, std::vector<size_t>& inputSize,
                         std::vector<uint8_t*>& outputBuffers, std::vector<size_t>& outputSize,
                         std::string perfProfile) {
    ProcInfo_t* pProcInfo = FindProcInfo(proc_name);
    if (!pProcInfo) {
        QNN_ERR("TalkToSvc_Inference::Cant find this process %s.\n", proc_name.c_str());
        return false;
    }

    ShareMemInfo_t* pShareMemInfo = FindShareMem(share_memory_name);
    if (!pShareMemInfo) {
        QNN_ERR("TalkToSvc_Inference::Cant find this share memory %s.\n", share_memory_name.c_str());
        return false;
    }

    HANDLE hSvcPipeInWrite = pProcInfo->hSvcPipeInWrite;
    HANDLE hSvcPipeOutRead = pProcInfo->hSvcPipeOutRead;
    DWORD dwRead = 0, dwWrite = 0;
    BOOL bSuccess;

    std::string command = "g" + model_name + ";" + share_memory_name + ";" + std::to_string(pShareMemInfo->size) + ";";
    // 'offset' in share memory(according to 'inputBuffers' data size, so that we can restore this data to 'std::vector<uint8_t*>' in Svc).
    std::pair<std::string, std::string> strResultArray = VectorToShareMem(pShareMemInfo->size, (uint8_t*)pShareMemInfo->lpBase, inputBuffers, inputSize);
    command = command + strResultArray.first + "=" + strResultArray.second + ";";
    command = command + perfProfile;
    dwRead = (DWORD)command.length() + 1;

    // start_time();
    // Write command to Svc.
    bSuccess = WriteFile(hSvcPipeInWrite, command.c_str(), dwRead, &dwWrite, NULL);
    // QNN_INF("TalkToSvc_Inference::WriteToPipe: %s dwRead = %d dwWrite = %d\n", command.c_str(), dwRead, dwWrite);
    if (!bSuccess) return false;

    // Read command from Svc.
    bSuccess = ReadFile(hSvcPipeOutRead, g_buffer, GLOBAL_BUFSIZE, &dwRead, NULL);
    if(dwRead) {
        g_buffer[dwRead] = 0;
        QNN_INF("TalkToSvc_Inference::ReadFromPipe: %s dwRead = %d\n", g_buffer, dwRead);
    }
    else {
        QNN_ERR("TalkToSvc_Inference::ReadFromPipe: Failed to read from hSvcPipeOutRead, perhaps child process died.\n");
    }
    if (!bSuccess || dwRead == 0) return false;
    //print_time("TalkToSvc_Inference::Pipe talk");

    // Read the output data from 'share_memory_name'.
    if (dwRead) {
        if (g_buffer[0] == 'F') {  // ACTION_FAILED == Failed.
            return false;
        }

        ShareMemToVector(g_buffer, (uint8_t*)pShareMemInfo->lpBase, outputBuffers, outputSize);
    }

    return bSuccess;
}

#endif

