// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

// Copyright (c) 2017 Samsung Electronics Co., LTD

#include "symbolreader.h"

#include <coreclrhost.h>
#include <thread>

#include "modules.h"
#include "platform.h"
#include "torelease.h"
#include "cputil.h"


static const char *SymbolReaderDllName = "SymbolReader";
static const char *SymbolReaderClassName = "SOS.SymbolReader";

#ifdef FEATURE_PAL
// Suppress undefined reference
// `_invalid_parameter(char16_t const*, char16_t const*, char16_t const*, unsigned int, unsigned long)':
//      /coreclr/src/pal/inc/rt/safecrt.h:386: undefined reference to `RaiseException'
static void RaiseException(DWORD dwExceptionCode,
               DWORD dwExceptionFlags,
               DWORD nNumberOfArguments,
               CONST ULONG_PTR *lpArguments)
{
}
#endif

std::string SymbolReader::coreClrPath;
LoadSymbolsForModuleDelegate SymbolReader::loadSymbolsForModuleDelegate;
DisposeDelegate SymbolReader::disposeDelegate;
ResolveSequencePointDelegate SymbolReader::resolveSequencePointDelegate;
GetLocalVariableNameAndScope SymbolReader::getLocalVariableNameAndScopeDelegate;
GetLineByILOffsetDelegate SymbolReader::getLineByILOffsetDelegate;
GetStepRangesFromIPDelegate SymbolReader::getStepRangesFromIPDelegate;
GetSequencePointsDelegate SymbolReader::getSequencePointsDelegate;
ParseExpressionDelegate SymbolReader::parseExpressionDelegate = nullptr;
EvalExpressionDelegate SymbolReader::evalExpressionDelegate = nullptr;
RegisterGetChildDelegate SymbolReader::registerGetChildDelegate = nullptr;

SysAllocStringLen_t SymbolReader::sysAllocStringLen;
SysFreeString_t SymbolReader::sysFreeString;
SysStringLen_t SymbolReader::sysStringLen;
CoTaskMemAlloc_t SymbolReader::coTaskMemAlloc;
CoTaskMemFree_t SymbolReader::coTaskMemFree;

const int SymbolReader::HiddenLine = 0xfeefee;

HRESULT SymbolReader::LoadSymbols(IMetaDataImport* pMD, ICorDebugModule* pModule)
{
    HRESULT Status = S_OK;
    BOOL isDynamic = FALSE;
    BOOL isInMemory = FALSE;
    IfFailRet(pModule->IsDynamic(&isDynamic));
    IfFailRet(pModule->IsInMemory(&isInMemory));

    if (isDynamic)
    {
        // Dynamic and in memory assemblies are a special case which we will ignore for now
        return E_FAIL;
    }

    ULONG64 peAddress = 0;
    ULONG32 peSize = 0;
    IfFailRet(pModule->GetBaseAddress(&peAddress));
    IfFailRet(pModule->GetSize(&peSize));

    return LoadSymbolsForPortablePDB(
        Modules::GetModuleFileName(pModule),
        isInMemory,
        isInMemory, // isFileLayout
        peAddress,
        peSize,
        0,          // inMemoryPdbAddress
        0           // inMemoryPdbSize
    );
}

//
// Pass to managed helper code to read in-memory PEs/PDBs
// Returns the number of bytes read.
//
int ReadMemoryForSymbols(ULONG64 address, char *buffer, int cb)
{
    ULONG read;
    if (SafeReadMemory(TO_TADDR(address), (PVOID)buffer, cb, &read))
    {
        return read;
    }
    return 0;
}

HRESULT SymbolReader::LoadSymbolsForPortablePDB(
    const std::string &modulePath,
    BOOL isInMemory,
    BOOL isFileLayout,
    ULONG64 peAddress,
    ULONG64 peSize,
    ULONG64 inMemoryPdbAddress,
    ULONG64 inMemoryPdbSize)
{
    HRESULT Status = S_OK;

    if (loadSymbolsForModuleDelegate == nullptr)
    {
        IfFailRet(PrepareSymbolReader());
    }

    // The module name needs to be null for in-memory PE's.
    const char *szModuleName = nullptr;
    if (!isInMemory && !modulePath.empty())
    {
        szModuleName = modulePath.c_str();
    }

    m_symbolReaderHandle = loadSymbolsForModuleDelegate(szModuleName, isFileLayout, peAddress,
        (int)peSize, inMemoryPdbAddress, (int)inMemoryPdbSize, ReadMemoryForSymbols);

    if (m_symbolReaderHandle == 0)
    {
        return E_FAIL;
    }

    return Status;
}

struct GetChildProxy
{
    SymbolReader::GetChildCallback &m_cb;
    static BOOL GetChild(PVOID opaque, PVOID corValue, const char* name, int *typeId, PVOID *data)
    {
        return static_cast<GetChildProxy*>(opaque)->m_cb(corValue, name, typeId, data);
    }
};

HRESULT SymbolReader::PrepareSymbolReader()
{
    static bool attemptedSymbolReaderPreparation = false;
    if (attemptedSymbolReaderPreparation)
    {
        // If we already tried to set up the symbol reader, we won't try again.
        return E_FAIL;
    }

    attemptedSymbolReaderPreparation = true;

    std::string clrDir = coreClrPath.substr(0, coreClrPath.rfind(DIRECTORY_SEPARATOR_STR_A));

    HRESULT Status;

    UnsetCoreCLREnv();

    void *coreclrLib = DLOpen(coreClrPath);
    if (coreclrLib == nullptr)
    {
        // TODO: Messages like this break VSCode debugger protocol. They should be reported through Protocol class.
        fprintf(stderr, "Error: Failed to load coreclr\n");
        return E_FAIL;
    }

    void *hostHandle;
    unsigned int domainId;
    coreclr_initialize_ptr initializeCoreCLR = (coreclr_initialize_ptr)DLSym(coreclrLib, "coreclr_initialize");
    if (initializeCoreCLR == nullptr)
    {
        fprintf(stderr, "Error: coreclr_initialize not found\n");
        return E_FAIL;
    }

#ifdef FEATURE_PAL
    sysAllocStringLen = (SysAllocStringLen_t)DLSym(coreclrLib, "SysAllocStringLen");
    sysFreeString = (SysFreeString_t)DLSym(coreclrLib, "SysFreeString");
    sysStringLen = (SysStringLen_t)DLSym(coreclrLib, "SysStringLen");
    coTaskMemAlloc = (CoTaskMemAlloc_t)DLSym(coreclrLib, "CoTaskMemAlloc");
    coTaskMemFree = (CoTaskMemFree_t)DLSym(coreclrLib, "CoTaskMemFree");
#else
    sysAllocStringLen = SysAllocStringLen;
    sysFreeString = SysFreeString;
    sysStringLen = SysStringLen;
    coTaskMemAlloc = CoTaskMemAlloc;
    coTaskMemFree = CoTaskMemFree;
#endif

    if (sysAllocStringLen == nullptr)
    {
        fprintf(stderr, "Error: SysAllocStringLen not found\n");
        return E_FAIL;
    }

    if (sysFreeString == nullptr)
    {
        fprintf(stderr, "Error: SysFreeString not found\n");
        return E_FAIL;
    }

    if (sysStringLen == nullptr)
    {
        fprintf(stderr, "Error: SysStringLen not found\n");
        return E_FAIL;
    }

    if (coTaskMemAlloc == nullptr)
    {
        fprintf(stderr, "Error: CoTaskMemAlloc not found\n");
        return E_FAIL;
    }

    if (coTaskMemFree == nullptr)
    {
        fprintf(stderr, "Error: CoTaskMemFree not found\n");
        return E_FAIL;
    }

    std::string tpaList;
    AddFilesFromDirectoryToTpaList(clrDir, tpaList);

    const char *propertyKeys[] = {
        "TRUSTED_PLATFORM_ASSEMBLIES",
        "APP_PATHS",
        "APP_NI_PATHS",
        "NATIVE_DLL_SEARCH_DIRECTORIES",
        "AppDomainCompatSwitch"};


    std::string exe = GetExeAbsPath();
    if (exe.empty())
    {
        fprintf(stderr, "GetExeAbsPath is empty\n");
        return E_FAIL;
    }

    std::size_t dirSepIndex = exe.rfind(DIRECTORY_SEPARATOR_STR_A);
    if (dirSepIndex == std::string::npos)
        return E_FAIL;

    std::string exeDir = exe.substr(0, dirSepIndex);

    const char *propertyValues[] = {// TRUSTED_PLATFORM_ASSEMBLIES
                                    tpaList.c_str(),
                                    // APP_PATHS
                                    exeDir.c_str(),
                                    // APP_NI_PATHS
                                    exeDir.c_str(),
                                    // NATIVE_DLL_SEARCH_DIRECTORIES
                                    clrDir.c_str(),
                                    // AppDomainCompatSwitch
                                    "UseLatestBehaviorWhenTFMNotSpecified"};

    Status = initializeCoreCLR(exe.c_str(), "debugger",
        sizeof(propertyKeys) / sizeof(propertyKeys[0]), propertyKeys, propertyValues, &hostHandle, &domainId);

    if (FAILED(Status))
    {
        fprintf(stderr, "Error: Fail to initialize CoreCLR %08x\n", Status);
        return Status;
    }

    coreclr_create_delegate_ptr createDelegate = (coreclr_create_delegate_ptr)DLSym(coreclrLib, "coreclr_create_delegate");
    if (createDelegate == nullptr)
    {
        fprintf(stderr, "Error: coreclr_create_delegate not found\n");
        return E_FAIL;
    }

    // TODO: If SymbolReaderDllName could not be found, we are going to see the error message.
    //       But the cleaner way is to provide error message for any failed createDelegate().
    if (FAILED(Status = createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "LoadSymbolsForModule", (void **)&loadSymbolsForModuleDelegate)))
    {
        fprintf(stderr, "Error: createDelegate failed for LoadSymbolsForModule: 0x%x\n", Status);
        return E_FAIL;
    }
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "Dispose", (void **)&disposeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "ResolveSequencePoint", (void **)&resolveSequencePointDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLocalVariableNameAndScope", (void **)&getLocalVariableNameAndScopeDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetLineByILOffset", (void **)&getLineByILOffsetDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetStepRangesFromIP", (void **)&getStepRangesFromIPDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "GetSequencePoints", (void **)&getSequencePointsDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "ParseExpression", (void **)&parseExpressionDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "EvalExpression", (void **)&evalExpressionDelegate));
    IfFailRet(createDelegate(hostHandle, domainId, SymbolReaderDllName, SymbolReaderClassName, "RegisterGetChild", (void **)&registerGetChildDelegate));
    if (!registerGetChildDelegate(GetChildProxy::GetChild))
        return E_FAIL;

    // Warm up Roslyn
    std::thread([](){ std::string data; std::string err; SymbolReader::ParseExpression("1", "System.Int32", data, err); }).detach();

    return S_OK;
}

HRESULT SymbolReader::ResolveSequencePoint(const char *filename, ULONG32 lineNumber, TADDR mod, mdMethodDef* pToken, ULONG32* pIlOffset)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(resolveSequencePointDelegate != nullptr);

        if (resolveSequencePointDelegate(m_symbolReaderHandle, filename, lineNumber, pToken, pIlOffset) == FALSE)
        {
            return E_FAIL;
        }
        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetLineByILOffset(
    mdMethodDef methodToken,
    ULONG64 ilOffset,
    ULONG *pLinenum,
    WCHAR* pwszFileName,
    ULONG cchFileName)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getLineByILOffsetDelegate != nullptr);

        BSTR bstrFileName = sysAllocStringLen(0, MAX_LONGPATH);
        if (sysStringLen(bstrFileName) == 0)
        {
            return E_OUTOFMEMORY;
        }
        // Source lines with 0xFEEFEE markers are filtered out on the managed side.
        if ((getLineByILOffsetDelegate(m_symbolReaderHandle, methodToken, ilOffset, pLinenum, &bstrFileName) == FALSE) || (*pLinenum == 0))
        {
            sysFreeString(bstrFileName);
            return E_FAIL;
        }
        wcscpy_s(pwszFileName, cchFileName, bstrFileName);
        sysFreeString(bstrFileName);
        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetStepRangesFromIP(ULONG32 ip, mdMethodDef MethodToken, ULONG32 *ilStartOffset, ULONG32 *ilEndOffset)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getStepRangesFromIPDelegate != nullptr);

        if (getStepRangesFromIPDelegate(m_symbolReaderHandle, ip, MethodToken, ilStartOffset, ilEndOffset) == FALSE)
        {
            return E_FAIL;
        }

        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::GetNamedLocalVariableAndScope(
    ICorDebugILFrame * pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    WCHAR* paramName,
    ULONG paramNameLen,
    ICorDebugValue** ppValue,
    ULONG32* pIlStart,
    ULONG32* pIlEnd)
{
    HRESULT Status = S_OK;

    if (!m_symbolReaderHandle)
        return E_FAIL;

    _ASSERTE(getLocalVariableNameAndScopeDelegate != nullptr);

    BSTR wszParamName = sysAllocStringLen(0, mdNameLen);
    if (sysStringLen(wszParamName) == 0)
    {
        return E_OUTOFMEMORY;
    }

    if (getLocalVariableNameAndScopeDelegate(m_symbolReaderHandle, methodToken, localIndex, &wszParamName, pIlStart, pIlEnd) == FALSE)
    {
        sysFreeString(wszParamName);
        return E_FAIL;
    }

    wcscpy_s(paramName, paramNameLen, wszParamName);
    sysFreeString(wszParamName);

    if (FAILED(pILFrame->GetLocalVariable(localIndex, ppValue)) || (*ppValue == NULL))
    {
        *ppValue = NULL;
        return E_FAIL;
    }
    return S_OK;
}

HRESULT SymbolReader::GetSequencePoints(mdMethodDef methodToken, std::vector<SequencePoint> &points)
{
    HRESULT Status = S_OK;

    if (m_symbolReaderHandle != 0)
    {
        _ASSERTE(getSequencePointsDelegate != nullptr);

        SequencePoint *allocatedPoints = nullptr;
        int pointsCount = 0;

        if (getSequencePointsDelegate(m_symbolReaderHandle, methodToken, (PVOID*)&allocatedPoints, &pointsCount) == FALSE)
        {
            return E_FAIL;
        }

        points.assign(allocatedPoints, allocatedPoints + pointsCount);

        coTaskMemFree(allocatedPoints);

        return S_OK;
    }

    return E_FAIL;
}

HRESULT SymbolReader::ParseExpression(
    const std::string &expr,
    const std::string &typeName,
    std::string &data,
    std::string &errorText)
{
    PrepareSymbolReader();

    if (parseExpressionDelegate == nullptr)
        return E_FAIL;

    BSTR werrorText;
    PVOID dataPtr;
    int dataSize = 0;
    if (parseExpressionDelegate(expr.c_str(), typeName.c_str(), &dataPtr, &dataSize, &werrorText) == FALSE)
    {
        errorText = to_utf8(werrorText);
        sysFreeString(werrorText);
        return E_FAIL;
    }

    if (typeName == "System.String")
    {
        data = to_utf8((BSTR)dataPtr);
        sysFreeString((BSTR)dataPtr);
    }
    else
    {
        data.resize(dataSize);
        memmove(&data[0], dataPtr, dataSize);
        coTaskMemFree(dataPtr);
    }

    return S_OK;
}

HRESULT SymbolReader::EvalExpression(const std::string &expr, std::string &result, int *typeId, ICorDebugValue **ppValue, GetChildCallback cb)
{
    PrepareSymbolReader();

    if (evalExpressionDelegate == nullptr)
        return E_FAIL;

    GetChildProxy proxy { cb };

    PVOID valuePtr = nullptr;
    int size = 0;
    BSTR resultText;
    BOOL ok = evalExpressionDelegate(expr.c_str(), &proxy, &resultText, typeId, &size, &valuePtr);
    if (!ok)
    {
        if (resultText)
        {
            result = to_utf8(resultText);
            sysFreeString(resultText);
        }
        return E_FAIL;
    }

    switch(*typeId)
    {
        case TypeCorValue:
            *ppValue = static_cast<ICorDebugValue*>(valuePtr);
            if (*ppValue)
                (*ppValue)->AddRef();
            break;
        case TypeObject:
            result = std::string();
            break;
        case TypeString:
            result = to_utf8((BSTR)valuePtr);
            sysFreeString((BSTR)valuePtr);
            break;
        default:
            result.resize(size);
            memmove(&result[0], valuePtr, size);
            coTaskMemFree(valuePtr);
            break;
    }

    return S_OK;
}

PVOID SymbolReader::AllocBytes(size_t size)
{
    PrepareSymbolReader();
    if (coTaskMemAlloc == nullptr)
        return nullptr;
    return coTaskMemAlloc(size);
}

PVOID SymbolReader::AllocString(const std::string &str)
{
    PrepareSymbolReader();
    if (sysAllocStringLen == nullptr)
        return nullptr;
    auto wstr = to_utf16(str);
    if (wstr.size() > UINT_MAX)
        return nullptr;
    BSTR bstr = sysAllocStringLen(0, (UINT)wstr.size());
    if (sysStringLen(bstr) == 0)
        return nullptr;
    memmove(bstr, wstr.data(), wstr.size() * sizeof(decltype(wstr[0])));
    return bstr;
}
