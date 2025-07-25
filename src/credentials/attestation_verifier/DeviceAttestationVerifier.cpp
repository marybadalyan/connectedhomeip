/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
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
#include "DeviceAttestationVerifier.h"

#include <credentials/DeviceAttestationConstructor.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/CHIPMem.h>

using namespace chip::Crypto;

namespace chip {
namespace Credentials {

namespace {

// Version to have a default placeholder so the getter never
// returns `nullptr` by default.
class UnimplementedDACVerifier : public DeviceAttestationVerifier
{
public:
    void VerifyAttestationInformation(const AttestationInfo & info,
                                      Callback::Callback<OnAttestationInformationVerification> * onCompletion) override
    {
        (void) info;
        (void) onCompletion;
    }

    AttestationVerificationResult ValidateCertificationDeclarationSignature(const ByteSpan & cmsEnvelopeBuffer,
                                                                            ByteSpan & certDeclBuffer) override
    {
        (void) cmsEnvelopeBuffer;
        (void) certDeclBuffer;
        return AttestationVerificationResult::kNotImplemented;
    }

    AttestationVerificationResult ValidateCertificateDeclarationPayload(const ByteSpan & certDeclBuffer,
                                                                        const ByteSpan & firmwareInfo,
                                                                        const DeviceInfoForAttestation & deviceInfo) override
    {
        (void) certDeclBuffer;
        (void) firmwareInfo;
        (void) deviceInfo;
        return AttestationVerificationResult::kNotImplemented;
    }

    CHIP_ERROR VerifyNodeOperationalCSRInformation(const ByteSpan & nocsrElementsBuffer,
                                                   const ByteSpan & attestationChallengeBuffer,
                                                   const ByteSpan & attestationSignatureBuffer,
                                                   const Crypto::P256PublicKey & dacPublicKey, const ByteSpan & csrNonce) override
    {
        (void) nocsrElementsBuffer;
        (void) attestationChallengeBuffer;
        (void) attestationSignatureBuffer;
        (void) dacPublicKey;
        (void) csrNonce;
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    void CheckForRevokedDACChain(const AttestationInfo & info,
                                 Callback::Callback<OnAttestationInformationVerification> * onCompletion) override
    {
        (void) info;
        (void) onCompletion;
        VerifyOrDie(false);
    }
};

// Default to avoid nullptr on getter and cleanly handle new products/clients before
// they provide their own.
UnimplementedDACVerifier gDefaultDACVerifier;

DeviceAttestationVerifier * gDacVerifier = &gDefaultDACVerifier;

} // namespace

const char * GetAttestationResultDescription(AttestationVerificationResult resultCode)
{
    switch (resultCode)
    {
    case AttestationVerificationResult::kSuccess:
        return "Success";
    case AttestationVerificationResult::kPaaUntrusted:
        return "PAA is untrusted (OBSOLETE: consider using a different error)";
    case AttestationVerificationResult::kPaaNotFound:
        return "PAA not found in DCL and/or local PAA trust store";
    case AttestationVerificationResult::kPaaExpired:
        return "PAA is expired";
    case AttestationVerificationResult::kPaaSignatureInvalid:
        return "PAA signature is invalid";
    case AttestationVerificationResult::kPaaRevoked:
        return "PAA is revoked (consider removing from DCL or PAA trust store!)";
    case AttestationVerificationResult::kPaaFormatInvalid:
        return "PAA format is invalid";
    case AttestationVerificationResult::kPaaArgumentInvalid:
        return "PAA argument is invalid in some way according to X.509 backend";
    case AttestationVerificationResult::kPaiExpired:
        return "PAI is expired";
    case AttestationVerificationResult::kPaiSignatureInvalid:
        return "PAI signature is invalid";
    case AttestationVerificationResult::kPaiRevoked:
        return "PAI is revoked";
    case AttestationVerificationResult::kPaiFormatInvalid:
        return "PAI format is invalid";
    case AttestationVerificationResult::kPaiArgumentInvalid:
        return "PAI argument is invalid in some way according to X.509 backend";
    case AttestationVerificationResult::kPaiVendorIdMismatch:
        return "PAI vendor ID mismatch (did not match VID present in PAA)";
    case AttestationVerificationResult::kPaiAuthorityNotFound:
        return "PAI authority not found (OBSOLETE: consider using a different error)";
    case AttestationVerificationResult::kPaiMissing:
        return "PAI is missing/empty from attestation information data";
    case AttestationVerificationResult::kPaiAndDacRevoked:
        return "Both PAI and DAC are revoked";
    case AttestationVerificationResult::kDacExpired:
        return "DAC is expired";
    case AttestationVerificationResult::kDacSignatureInvalid:
        return "DAC signature is invalid";
    case AttestationVerificationResult::kDacRevoked:
        return "DAC is revoked";
    case AttestationVerificationResult::kDacFormatInvalid:
        return "DAC format is invalid";
    case AttestationVerificationResult::kDacArgumentInvalid:
        return "DAC is invalid in some way according to X.509 backend";
    case AttestationVerificationResult::kDacVendorIdMismatch:
        return "DAC vendor ID mismatch (either between DAC and PAI, or between DAC and Basic Information cluster)";
    case AttestationVerificationResult::kDacProductIdMismatch:
        return "DAC product ID mismatch (either between DAC and PAI, or between DAC and Basic Information cluster)";
    case AttestationVerificationResult::kDacAuthorityNotFound:
        return "DAC authority not found (OBSOLETE: consider using a different error)";
    case AttestationVerificationResult::kFirmwareInformationMismatch:
        return "Firmware information mismatch";
    case AttestationVerificationResult::kFirmwareInformationMissing:
        return "Firmware information missing";
    case AttestationVerificationResult::kAttestationSignatureInvalid:
        return "Attestation signature failed to validate against DAC subject public key";
    case AttestationVerificationResult::kAttestationElementsMalformed:
        return "Attestation elements payload is malformed";
    case AttestationVerificationResult::kAttestationNonceMismatch:
        return "Attestation nonce does not match the one from Attestation Request";
    case AttestationVerificationResult::kAttestationSignatureInvalidFormat:
        return "Attestation signature format is invalid (likely wrong signature algorithm in certificate)";
    case AttestationVerificationResult::kCertificationDeclarationNoKeyId:
        return "Certification declaration missing the required key ID in CMS envelope";
    case AttestationVerificationResult::kCertificationDeclarationNoCertificateFound:
        return "Could not find matching trusted verification certificate for the certification declaration's key ID";
    case AttestationVerificationResult::kCertificationDeclarationInvalidSignature:
        return "Certification declaration signature failed to validate against the verification certificate";
    case AttestationVerificationResult::kCertificationDeclarationInvalidFormat:
        return "Certification declaration format is invalid";
    case AttestationVerificationResult::kCertificationDeclarationInvalidVendorId:
        return "Certification declaration vendor ID failed to cross-reference with DAC and/or PAI and/or Basic Information cluster";
    case AttestationVerificationResult::kCertificationDeclarationInvalidProductId:
        return "Certification declaration product ID failed to cross-reference with DAC and/or PAI and/or Basic Information "
               "cluster";
    case AttestationVerificationResult::kCertificationDeclarationInvalidPAA:
        return "Certification declaration required a fixed allowed PAA which does not match the final PAA found";
    case AttestationVerificationResult::kNoMemory:
        return "Failed to allocate memory to process attestation verification";
    case AttestationVerificationResult::kInvalidArgument:
        return "Some unexpected invalid argument was provided internally to the device attestation procedure (likely malformed "
               "input data from candidate device)";
    case AttestationVerificationResult::kInternalError:
        return "An internal error arose in the device attestation procedure (likely malformed input data from candidate "
               "device)";
    case AttestationVerificationResult::kNotImplemented:
        return "Reached a critical-but-unimplemented part of the device attestation procedure!";
    }

    return "<AttestationVerificationResult does not have a description!>";
}

CHIP_ERROR DeviceAttestationVerifier::ValidateAttestationSignature(const P256PublicKey & pubkey,
                                                                   const ByteSpan & attestationElements,
                                                                   const ByteSpan & attestationChallenge,
                                                                   const P256ECDSASignature & signature)
{
    Hash_SHA256_stream hashStream;
    uint8_t md[kSHA256_Hash_Length];
    MutableByteSpan messageDigestSpan(md);

    ReturnErrorOnFailure(hashStream.Begin());
    ReturnErrorOnFailure(hashStream.AddData(attestationElements));
    ReturnErrorOnFailure(hashStream.AddData(attestationChallenge));
    ReturnErrorOnFailure(hashStream.Finish(messageDigestSpan));

    ReturnErrorOnFailure(pubkey.ECDSA_validate_hash_signature(messageDigestSpan.data(), messageDigestSpan.size(), signature));

    return CHIP_NO_ERROR;
}

DeviceAttestationVerifier * GetDeviceAttestationVerifier()
{
    return gDacVerifier;
}

void SetDeviceAttestationVerifier(DeviceAttestationVerifier * verifier)
{
    if (verifier == nullptr)
    {
        return;
    }

    gDacVerifier = verifier;
}

static inline Platform::ScopedMemoryBufferWithSize<uint8_t> CopyByteSpanHelper(const ByteSpan & span_to_copy)
{
    Platform::ScopedMemoryBufferWithSize<uint8_t> bufferCopy;
    if (bufferCopy.Alloc(span_to_copy.size()))
    {
        memcpy(bufferCopy.Get(), span_to_copy.data(), span_to_copy.size());
    }
    return bufferCopy;
}

DeviceAttestationVerifier::AttestationDeviceInfo::AttestationDeviceInfo(const AttestationInfo & attestationInfo) :
    mPaiDerBuffer(CopyByteSpanHelper(attestationInfo.paiDerBuffer)),
    mDacDerBuffer(CopyByteSpanHelper(attestationInfo.dacDerBuffer)), mBasicInformationVendorId(attestationInfo.vendorId),
    mBasicInformationProductId(attestationInfo.productId)
{
    ByteSpan certificationDeclarationSpan;
    ByteSpan attestationNonceSpan;
    uint32_t timestampDeconstructed;
    ByteSpan firmwareInfoSpan;
    DeviceAttestationVendorReservedDeconstructor vendorReserved;

    if (DeconstructAttestationElements(attestationInfo.attestationElementsBuffer, certificationDeclarationSpan,
                                       attestationNonceSpan, timestampDeconstructed, firmwareInfoSpan,
                                       vendorReserved) == CHIP_NO_ERROR)
    {
        mCdBuffer = CopyByteSpanHelper(certificationDeclarationSpan);
    }
}

} // namespace Credentials
} // namespace chip
