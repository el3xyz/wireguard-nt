/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018-2021 WireGuard LLC. All Rights Reserved.
 */

#include "logger.h"
#include "main.h"
#include "namespace.h"

#include <Windows.h>
#include <winternl.h>
#include <bcrypt.h>
#include <winefs.h>
#include <wchar.h>
#include <stdlib.h>

static HANDLE PrivateNamespace = NULL;
static HANDLE BoundaryDescriptor = NULL;
static CRITICAL_SECTION Initializing;
static BCRYPT_ALG_HANDLE AlgProvider;

_Must_inspect_result_
static _Return_type_success_(return != NULL)
_Post_maybenull_
LPWSTR
NormalizeStringAlloc(_In_ NORM_FORM NormForm, _In_z_ LPCWSTR Source)
{
    int Len = NormalizeString(NormForm, Source, -1, NULL, 0);
    for (;;)
    {
        LPWSTR Str = AllocArray(Len, sizeof(*Str));
        if (!Str)
            return NULL;
        Len = NormalizeString(NormForm, Source, -1, Str, Len);
        if (Len > 0)
            return Str;
        Free(Str);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            LOG_LAST_ERROR(L"Failed: %s", Source);
            return NULL;
        }
        Len = -Len;
    }
}

static _Return_type_success_(return != FALSE)
BOOL NamespaceRuntimeInit(VOID)
{
    DWORD LastError;

    EnterCriticalSection(&Initializing);
    if (PrivateNamespace)
    {
        LeaveCriticalSection(&Initializing);
        return TRUE;
    }

    NTSTATUS Status;
    if (!BCRYPT_SUCCESS(Status = BCryptOpenAlgorithmProvider(&AlgProvider, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
    {
        LOG(WIREGUARD_LOG_ERR, L"Failed to open algorithm provider (status: 0x%x)", Status);
        LastError = RtlNtStatusToDosError(Status);
        goto cleanupLeaveCriticalSection;
    }

    BYTE Sid[MAX_SID_SIZE];
    DWORD SidSize = sizeof(Sid);
    if (!CreateWellKnownSid(IsLocalSystem ? WinLocalSystemSid : WinBuiltinAdministratorsSid, NULL, Sid, &SidSize))
    {
        LastError = LOG_LAST_ERROR(L"Failed to create SID");
        goto cleanupBCryptCloseAlgorithmProvider;
    }

    BoundaryDescriptor = CreateBoundaryDescriptorW(L"WireGuard", 0);
    if (!BoundaryDescriptor)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create boundary descriptor");
        goto cleanupBCryptCloseAlgorithmProvider;
    }
    if (!AddSIDToBoundaryDescriptor(&BoundaryDescriptor, Sid))
    {
        LastError = LOG_LAST_ERROR(L"Failed to add SID to boundary descriptor");
        goto cleanupBoundaryDescriptor;
    }

    for (;;)
    {
        if ((PrivateNamespace = CreatePrivateNamespaceW(&SecurityAttributes, BoundaryDescriptor, L"WireGuard")) != NULL)
            break;
        if ((LastError = GetLastError()) == ERROR_ALREADY_EXISTS)
        {
            if ((PrivateNamespace = OpenPrivateNamespaceW(BoundaryDescriptor, L"WireGuard")) != NULL)
                break;
            if ((LastError = GetLastError()) == ERROR_PATH_NOT_FOUND)
                continue;
            LOG_ERROR(LastError, L"Failed to open private namespace");
        }
        else
            LOG_ERROR(LastError, L"Failed to create private namespace");
        goto cleanupBoundaryDescriptor;
    }

    LeaveCriticalSection(&Initializing);
    return TRUE;

cleanupBoundaryDescriptor:
    DeleteBoundaryDescriptor(BoundaryDescriptor);
cleanupBCryptCloseAlgorithmProvider:
    BCryptCloseAlgorithmProvider(AlgProvider, 0);
cleanupLeaveCriticalSection:
    LeaveCriticalSection(&Initializing);
    SetLastError(LastError);
    return FALSE;
}

_Use_decl_annotations_
HANDLE
NamespaceTakePoolMutex(LPCWSTR Pool)
{
    if (!NamespaceRuntimeInit())
        return NULL;

    BCRYPT_HASH_HANDLE Sha256 = NULL;
    NTSTATUS Status;
    if (!BCRYPT_SUCCESS(Status = BCryptCreateHash(AlgProvider, &Sha256, NULL, 0, NULL, 0, 0)))
    {
        LOG(WIREGUARD_LOG_ERR, L"Failed to create hash (status: 0x%x)", Status);
        SetLastError(RtlNtStatusToDosError(Status));
        return NULL;
    }
    DWORD LastError;
    static const WCHAR mutex_label[] = L"WireGuard Adapter Name Mutex Stable Suffix v1 jason@zx2c4.com";
    if (!BCRYPT_SUCCESS(
            Status = BCryptHashData(Sha256, (PUCHAR)mutex_label, sizeof(mutex_label) /* Including NULL 2 bytes */, 0)))
    {
        LOG(WIREGUARD_LOG_ERR, L"Failed to hash data (status: 0x%x)", Status);
        LastError = RtlNtStatusToDosError(Status);
        goto cleanupSha256;
    }
    LPWSTR PoolNorm = NormalizeStringAlloc(NormalizationC, Pool);
    if (!PoolNorm)
    {
        LastError = GetLastError();
        goto cleanupSha256;
    }
    if (!BCRYPT_SUCCESS(
            Status = BCryptHashData(Sha256, (PUCHAR)PoolNorm, (int)wcslen(PoolNorm) + 2 /* Add in NULL 2 bytes */, 0)))
    {
        LOG(WIREGUARD_LOG_ERR, L"Failed to hash data (status: 0x%x)", Status);
        LastError = RtlNtStatusToDosError(Status);
        goto cleanupPoolNorm;
    }
    BYTE Hash[32];
    if (!BCRYPT_SUCCESS(Status = BCryptFinishHash(Sha256, Hash, sizeof(Hash), 0)))
    {
        LOG(WIREGUARD_LOG_ERR, L"Failed to calculate hash (status: 0x%x)", Status);
        LastError = RtlNtStatusToDosError(Status);
        goto cleanupPoolNorm;
    }
    static const WCHAR MutexNamePrefix[] = L"WireGuard\\WireGuard-Name-Mutex-";
    WCHAR MutexName[_countof(MutexNamePrefix) + sizeof(Hash) * 2];
    memcpy(MutexName, MutexNamePrefix, sizeof(MutexNamePrefix));
    for (size_t i = 0; i < sizeof(Hash); ++i)
        swprintf_s(&MutexName[_countof(MutexNamePrefix) - 1 + i * 2], 3, L"%02x", Hash[i]);
    HANDLE Mutex = CreateMutexW(&SecurityAttributes, FALSE, MutexName);
    if (!Mutex)
    {
        LastError = LOG_LAST_ERROR(L"Failed to create mutex %s", MutexName);
        goto cleanupPoolNorm;
    }
    DWORD Result = WaitForSingleObject(Mutex, INFINITE);
    switch (Result)
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        Free(PoolNorm);
        BCryptDestroyHash(Sha256);
        return Mutex;
    }
    LOG(WIREGUARD_LOG_ERR, L"Failed to get mutex %s (status: 0x%x)", MutexName, Result);
    LastError = ERROR_GEN_FAILURE;
    CloseHandle(Mutex);
cleanupPoolNorm:
    Free(PoolNorm);
cleanupSha256:
    BCryptDestroyHash(Sha256);
    SetLastError(LastError);
    return NULL;
}

_Use_decl_annotations_
HANDLE
NamespaceTakeDriverInstallationMutex(VOID)
{
    if (!NamespaceRuntimeInit())
        return NULL;
    HANDLE Mutex = CreateMutexW(&SecurityAttributes, FALSE, L"WireGuard\\WireGuard-Driver-Installation-Mutex");
    if (!Mutex)
    {
        LOG_LAST_ERROR(L"Failed to create mutex");
        return NULL;
    }
    DWORD Result = WaitForSingleObject(Mutex, INFINITE);
    switch (Result)
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        return Mutex;
    }
    LOG(WIREGUARD_LOG_ERR, L"Failed to get mutex (status: 0x%x)", Result);
    CloseHandle(Mutex);
    SetLastError(ERROR_GEN_FAILURE);
    return NULL;
}

_Use_decl_annotations_
VOID
NamespaceReleaseMutex(HANDLE Mutex)
{
    ReleaseMutex(Mutex);
    CloseHandle(Mutex);
}

VOID NamespaceInit(VOID)
{
    InitializeCriticalSection(&Initializing);
}

VOID NamespaceDone(VOID)
{
    EnterCriticalSection(&Initializing);
    if (PrivateNamespace)
    {
        BCryptCloseAlgorithmProvider(AlgProvider, 0);
        ClosePrivateNamespace(PrivateNamespace, 0);
        DeleteBoundaryDescriptor(BoundaryDescriptor);
        PrivateNamespace = NULL;
    }
    LeaveCriticalSection(&Initializing);
    DeleteCriticalSection(&Initializing);
}
