/*
 *
 *    Copyright (c) 2024 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <diagnostic-logs-provider-delegate-impl.h>
#include <lib/support/SafeInt.h>
#include <tracing/esp32_diagnostic_trace/DiagnosticTracing.h>

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
#include <esp_core_dump.h>
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)

using namespace chip;
using namespace chip::app::Clusters::DiagnosticLogs;

LogProvider LogProvider::sInstance;
LogProvider::CrashLogContext LogProvider::sCrashLogContext;

static uint32_t sReadEntries = 0;

namespace {
bool IsValidIntent(IntentEnum intent)
{
    return intent != IntentEnum::kUnknownEnumValue;
}

} // namespace

LogProvider::~LogProvider()
{
    for (auto sessionSpan : mSessionContextMap)
    {
        Platform::MemoryFree(sessionSpan.second);
    }
    mSessionContextMap.clear();
    if (mStorageInstance != nullptr)
    {
        delete mStorageInstance;
        mStorageInstance = nullptr;
    }
    if (mRetrievalBuffer != nullptr)
    {
        Platform::MemoryFree(mRetrievalBuffer);
        mRetrievalBuffer = nullptr;
    }
}

CHIP_ERROR LogProvider::Init(LogProviderInit & providerInit)
{
    mRetrievalBuffer = providerInit.retrievalBuffer;
    mBufferSize      = providerInit.retrievalBufferSize;
    mStorageInstance = new CircularDiagnosticBuffer(providerInit.endUserBuffer, providerInit.endUserBufferSize);
    if (mStorageInstance == nullptr)
    {
        return CHIP_ERROR_NO_MEMORY;
    }
    static chip::Tracing::Diagnostics::ESP32Diagnostics diagnosticBackend(mStorageInstance);
    chip::Tracing::Register(diagnosticBackend);
    return CHIP_NO_ERROR;
}

CHIP_ERROR LogProvider::GetLogForIntent(IntentEnum intent, MutableByteSpan & outBuffer, Optional<uint64_t> & outTimeStamp,
                                        Optional<uint64_t> & outTimeSinceBoot)
{
    CHIP_ERROR err                 = CHIP_NO_ERROR;
    LogSessionHandle sessionHandle = kInvalidLogSessionHandle;

    err = StartLogCollection(intent, sessionHandle, outTimeStamp, outTimeSinceBoot);
    VerifyOrReturnError(CHIP_NO_ERROR == err, err, outBuffer.reduce_size(0));

    bool unusedOutIsEndOfLog;
    CHIP_ERROR collectErr = CollectLog(sessionHandle, outBuffer, unusedOutIsEndOfLog);
    VerifyOrDo(collectErr == CHIP_NO_ERROR, outBuffer.reduce_size(0));

    CHIP_ERROR endErr = EndLogCollection(sessionHandle, collectErr);

    return (collectErr != CHIP_NO_ERROR) ? collectErr : endErr;
}

size_t LogProvider::GetSizeForIntent(IntentEnum intent)
{
    switch (intent)
    {
    case IntentEnum::kEndUserSupport:
        VerifyOrReturnError(mStorageInstance != nullptr, 0,
                            ChipLogError(DeviceLayer, "Diagnostic Storage instance cannot be null."));
        return mStorageInstance->GetDataSize();
    case IntentEnum::kNetworkDiag:
        return 0;
    case IntentEnum::kCrashLogs:
        return GetCrashSize();
    default:
        return 0;
    }
}

size_t LogProvider::GetCrashSize()
{
    size_t outSize = 0;

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    // Verify that the crash is present and sane
    esp_err_t esp_err = esp_core_dump_image_check();
    VerifyOrReturnValue(esp_err == ESP_OK, 0, ChipLogError(DeviceLayer, "Core dump image check failed, esp_err:%d", esp_err));

    outSize = sizeof(esp_core_dump_summary_t);
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)

    return outSize;
}

CHIP_ERROR LogProvider::PrepareLogContextForIntent(LogContext * context, IntentEnum intent)
{
    context->intent = intent;

    switch (intent)
    {
    case IntentEnum::kEndUserSupport: {
        VerifyOrReturnError(mStorageInstance != nullptr, CHIP_ERROR_INTERNAL,
                            ChipLogError(DeviceLayer, "Diagnostic Storage instance cannot be null."));
        MutableByteSpan endUserSupportSpan(mRetrievalBuffer, mBufferSize);
        VerifyOrReturnError(!mStorageInstance->IsBufferEmpty(), CHIP_ERROR_NOT_FOUND,
                            ChipLogError(DeviceLayer, "Empty Diagnostic buffer"));
        // Retrieve data from the diagnostic storage
        CHIP_ERROR err = mStorageInstance->Retrieve(endUserSupportSpan, sReadEntries);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to retrieve data: %s", ErrorStr(err));
            return err;
        }
        context->EndUserSupport.span = endUserSupportSpan;
    }
    break;

    case IntentEnum::kNetworkDiag: {
        return CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE;
    }
    break;

    case IntentEnum::kCrashLogs: {
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
        sCrashLogContext.Reset();
        context->Crash.logContext = &sCrashLogContext;

        size_t crashSize = GetCrashSize();
        VerifyOrReturnError(crashSize > 0, CHIP_ERROR_NOT_FOUND);

        esp_core_dump_summary_t * summary =
            reinterpret_cast<esp_core_dump_summary_t *>(Platform::MemoryCalloc(1, sizeof(esp_core_dump_summary_t)));
        VerifyOrReturnError(summary != nullptr, CHIP_ERROR_NO_MEMORY);

        esp_err_t esp_err = esp_core_dump_get_summary(summary);
        if (esp_err != ESP_OK)
        {
            ChipLogError(DeviceLayer, "Failed to get core dump image, esp_err:%d", esp_err);
            Platform::MemoryFree(summary);
            return CHIP_ERROR_NOT_FOUND;
        }

        context->Crash.logContext->crashSize  = crashSize;
        context->Crash.logContext->readOffset = 0;
        context->Crash.logContext->summary    = summary;
#else
        return CHIP_ERROR_NOT_FOUND;
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    }
    break;

    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    return CHIP_NO_ERROR;
}

void LogProvider::CleanupLogContextForIntent(LogContext * context)
{
    switch (context->intent)
    {
    case IntentEnum::kEndUserSupport:
        break;

    case IntentEnum::kNetworkDiag:
        break;

    case IntentEnum::kCrashLogs: {
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
        CrashLogContext * logContext = context->Crash.logContext;
        // Reset() frees the summary if allocated
        logContext->Reset();
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    }
    break;

    default:
        break;
    }
}

CHIP_ERROR LogProvider::GetDataForIntent(LogContext * context, MutableByteSpan & outBuffer, bool & outIsEndOfLog)
{
    switch (context->intent)
    {
    case IntentEnum::kEndUserSupport: {
        auto dataSize = context->EndUserSupport.span.size();
        auto count    = std::min(dataSize, outBuffer.size());

        VerifyOrReturnError(CanCastTo<off_t>(count), CHIP_ERROR_INVALID_ARGUMENT, outBuffer.reduce_size(0));
        ReturnErrorOnFailure(CopySpanToMutableSpan(ByteSpan(context->EndUserSupport.span.data(), count), outBuffer));

        outIsEndOfLog = dataSize == count;
        if (!outIsEndOfLog)
        {
            // reduce the span after reading count bytes
            context->EndUserSupport.span = context->EndUserSupport.span.SubSpan(count);
        }
    }
    break;

    case IntentEnum::kNetworkDiag: {
        auto dataSize = context->NetworkDiag.span.size();
        auto count    = std::min(dataSize, outBuffer.size());

        VerifyOrReturnError(CanCastTo<off_t>(count), CHIP_ERROR_INVALID_ARGUMENT, outBuffer.reduce_size(0));
        ReturnErrorOnFailure(CopySpanToMutableSpan(ByteSpan(context->NetworkDiag.span.data(), count), outBuffer));

        outIsEndOfLog = dataSize == count;
        if (!outIsEndOfLog)
        {
            // reduce the span after reading count bytes
            context->NetworkDiag.span = context->NetworkDiag.span.SubSpan(count);
        }
    }
    break;

    case IntentEnum::kCrashLogs: {
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
        CrashLogContext * logContext = context->Crash.logContext;

        VerifyOrReturnError(logContext->readOffset < logContext->crashSize, CHIP_ERROR_INCORRECT_STATE, outBuffer.reduce_size(0));

        size_t dataSize = logContext->crashSize - logContext->readOffset;
        auto count      = std::min(dataSize, outBuffer.size());

        VerifyOrReturnError(CanCastTo<off_t>(count), CHIP_ERROR_INVALID_ARGUMENT, outBuffer.reduce_size(0));

        const uint8_t * readAddr = reinterpret_cast<const uint8_t *>(logContext->summary) + logContext->readOffset;
        memcpy(outBuffer.data(), readAddr, count);
        outBuffer.reduce_size(count);

        logContext->readOffset += count;
        outIsEndOfLog = dataSize == count;
#else
        outBuffer.reduce_size(0);
        return CHIP_ERROR_NOT_FOUND;
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    }
    break;

    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR LogProvider::StartLogCollection(IntentEnum intent, LogSessionHandle & outHandle, Optional<uint64_t> & outTimeStamp,
                                           Optional<uint64_t> & outTimeSinceBoot)
{
    VerifyOrReturnValue(IsValidIntent(intent), CHIP_ERROR_INVALID_ARGUMENT);

#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    // In case of crash logs we can only mmap at max once, so check before doing anything
    if (intent == IntentEnum::kCrashLogs)
    {
        VerifyOrReturnError(sCrashLogContext.summary == nullptr, CHIP_ERROR_INCORRECT_STATE,
                            ChipLogError(DeviceLayer, "Crash summary already allocated"));
    }
#endif // defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)

    LogContext * context = reinterpret_cast<LogContext *>(Platform::MemoryCalloc(1, sizeof(LogContext)));
    VerifyOrReturnValue(context != nullptr, CHIP_ERROR_NO_MEMORY);

    CHIP_ERROR err = PrepareLogContextForIntent(context, intent);
    VerifyOrReturnError(err == CHIP_NO_ERROR, err, Platform::MemoryFree(context));

    mLogSessionHandle++;
    // If the session handle rolls over to UINT16_MAX which is invalid, reset to 0.
    VerifyOrDo(mLogSessionHandle != kInvalidLogSessionHandle, mLogSessionHandle = 0);

    outHandle                             = mLogSessionHandle;
    mSessionContextMap[mLogSessionHandle] = context;

    return CHIP_NO_ERROR;
}

CHIP_ERROR LogProvider::EndLogCollection(LogSessionHandle sessionHandle, CHIP_ERROR error)
{
    VerifyOrReturnError(mStorageInstance != nullptr, CHIP_ERROR_INTERNAL,
                        ChipLogError(DeviceLayer, "Diagnostic Storage instance cannot be null."));
    if (error == CHIP_NO_ERROR)
    {
        CHIP_ERROR err = mStorageInstance->ClearBuffer(sReadEntries);
        if (err != CHIP_NO_ERROR)
        {
            ChipLogError(DeviceLayer, "Failed to clear diagnostic read entries");
        }
        else
        {
            ChipLogDetail(DeviceLayer, "Cleared all read diagnostics successfully");
        }
    }

    VerifyOrReturnValue(sessionHandle != kInvalidLogSessionHandle, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnValue(mSessionContextMap.count(sessionHandle), CHIP_ERROR_INVALID_ARGUMENT);

    LogContext * context = mSessionContextMap[sessionHandle];
    VerifyOrReturnError(context, CHIP_ERROR_INCORRECT_STATE);

    CleanupLogContextForIntent(context);
    Platform::MemoryFree(context);
    mSessionContextMap.erase(sessionHandle);

    return CHIP_NO_ERROR;
}

CHIP_ERROR LogProvider::CollectLog(LogSessionHandle sessionHandle, MutableByteSpan & outBuffer, bool & outIsEndOfLog)
{
    VerifyOrReturnValue(sessionHandle != kInvalidLogSessionHandle, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnValue(mSessionContextMap.count(sessionHandle), CHIP_ERROR_INVALID_ARGUMENT);

    LogContext * context = mSessionContextMap[sessionHandle];
    VerifyOrReturnError(context, CHIP_ERROR_INCORRECT_STATE);

    return GetDataForIntent(context, outBuffer, outIsEndOfLog);
}
