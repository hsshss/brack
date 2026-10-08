#pragma once
// Minimal hand-written declarations of the Windows MIDI Services (MidiSrv) client COM ABI.
//
// Transcribed from the MIT-licensed microsoft/MIDI repository so that the build needs neither MIDL
// nor the repository:
//   src/in-box/idl/WindowsMidiServices.idl                        (interfaces, enums, structs)
//   src/in-box/Transport/MidiSrvTransport/Midi2MidiSrvTransport.idl  (coclass Midi2MidiSrvTransport)
//   src/in-box/Transport/VirtualMidiTransport/Midi2VirtualMidiTransport.idl (virtual device transport id)
//   src/in-box/Inc/midi_ump_message_defs.h, MidiDefs.h            (UMP stream message constants)
// Copyright (c) Microsoft Corporation. Licensed under the MIT License (https://github.com/microsoft/MIDI).
//
// Only the interfaces this project calls are declared, and only in full vtable order. The ABI of these
// interfaces is identical between the rc-2 tag (service 1.0.15, the version in Windows 11 25H2 retail
// builds) and main as of 2026-10 (only MessageOptionFlags_HasRunningStatus was added).
// These are internal service-client interfaces, not a documented app contract.

#include <windows.h>
#include <unknwn.h>

namespace brack::win::midisrv {

enum MidiDataFormats : unsigned int {
    MidiDataFormats_Invalid = 0,
    MidiDataFormats_ByteStream = 0x1,
    MidiDataFormats_UMP = 0x2,
    MidiDataFormats_Any = 0xFFFFFFFF,
};

enum MessageOptionFlags : int {
    MessageOptionFlags_None = 0,
    MessageOptionFlags_WaitForSendComplete = 1,
    MessageOptionFlags_ContextContainsGroupIndex = 2,
    MessageOptionFlags_CallbackRetry = 4,
    MessageOptionFlags_SeparateUMPs = 8,
    MessageOptionFlags_HasRunningStatus = 16,
};

struct TRANSPORTCREATIONPARAMS {
    MessageOptionFlags MessageOptions;
    MidiDataFormats DataFormat;
    GUID CallingComponent;
};

// interface IMidiTransport : IUnknown
MIDL_INTERFACE("EA264200-3328-49E5-8815-73649A8748BE")
IMidiTransport : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Activate(REFIID iid, void** activatedInterface) = 0;
};

// interface IMidiCallback : IUnknown
MIDL_INTERFACE("4D6A29E5-DF4F-4A2D-A923-9B23B3F2D6F6")
IMidiCallback : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Callback(MessageOptionFlags optionFlags, PVOID message, UINT size,
                                               LONGLONG position, LONGLONG context) = 0;
};

// interface IMidiBidirectional : IUnknown
MIDL_INTERFACE("B89BBB45-7001-4BEA-BBD8-C7CC26E7836C")
IMidiBidirectional : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR endpointDeviceInterfaceId, TRANSPORTCREATIONPARAMS* creationParams,
                                                 DWORD* mmcssTaskId, IMidiCallback* callback, LONGLONG context,
                                                 GUID sessionId) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
    virtual HRESULT STDMETHODCALLTYPE SendMidiMessage(MessageOptionFlags optionFlags, PVOID message, UINT size,
                                                      LONGLONG position) = 0;
};

// interface IMidiTransportConfigurationManager : IUnknown
// (the two optional pointer parameters are IMidiDeviceManager* / IMidiServiceConfigurationManager*,
// only used inside the service; clients pass nullptr, so they are declared as IUnknown* here)
MIDL_INTERFACE("f19dd642-1809-4497-9eee-f230b11bd6fb")
IMidiTransportConfigurationManager : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Initialize(GUID transportId, IUnknown* midiDeviceManager,
                                                 IUnknown* midiServiceConfigurationManager) = 0;
    // responseJson is allocated with CoTaskMemAlloc; free with CoTaskMemFree.
    virtual HRESULT STDMETHODCALLTYPE UpdateConfiguration(LPCWSTR configurationJsonSection, LPWSTR* responseJson) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};

// interface IMidiSessionTracker : IUnknown
MIDL_INTERFACE("194c2746-3ae5-419a-94d9-20416c7dbefe")
IMidiSessionTracker : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Initialize() = 0;
    virtual HRESULT STDMETHODCALLTYPE AddClientSession(GUID sessionId, LPCWSTR sessionName) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateClientSessionName(GUID sessionId, LPCWSTR sessionName) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveClientSession(GUID sessionId) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessionList(LPWSTR* sessionDetailsList) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
    // Win32 BOOL, not HRESULT. Demand-starts the service.
    virtual BOOL STDMETHODCALLTYPE VerifyConnectivity() = 0;
};

// coclass Midi2MidiSrvTransport (in-box, C:\Windows\System32\Midi2.MidiSrvTransport.dll)
inline constexpr CLSID CLSID_Midi2MidiSrvTransport = {
    0x2BA15E4E, 0x5417, 0x4A66, {0x85, 0xB8, 0x2B, 0x22, 0x60, 0xEF, 0xBC, 0x84}};

// coclass Midi2VirtualMidiTransport: the transport id of the "Virtual Device App" (APP) transport.
inline constexpr GUID TransportId_VirtualMidi = {
    0x8FEAAD91, 0x70E1, 0x4A19, {0x99, 0x7A, 0x37, 0x77, 0x20, 0xA7, 0x19, 0xC1}};
inline constexpr wchar_t TransportIdString_VirtualMidi[] = L"{8FEAAD91-70E1-4A19-997A-377720A719C1}";

// UMP stream (message type 0xF) status values, midi_ump_message_defs.h
inline constexpr unsigned kStreamEndpointDiscovery = 0x000;
inline constexpr unsigned kStreamEndpointInfoNotification = 0x001;
inline constexpr unsigned kStreamDeviceIdentityNotification = 0x002;
inline constexpr unsigned kStreamEndpointNameNotification = 0x003;
inline constexpr unsigned kStreamProductInstanceIdNotification = 0x004;
inline constexpr unsigned kStreamConfigurationRequest = 0x005;
inline constexpr unsigned kStreamConfigurationNotification = 0x006;
inline constexpr unsigned kStreamFunctionBlockDiscovery = 0x010;
inline constexpr unsigned kStreamFunctionBlockInfoNotification = 0x011;
inline constexpr unsigned kStreamFunctionBlockNameNotification = 0x012;

inline constexpr unsigned kStreamFormComplete = 0x0;
inline constexpr unsigned kStreamFormStart = 0x1;
inline constexpr unsigned kStreamFormContinue = 0x2;
inline constexpr unsigned kStreamFormEnd = 0x3;

inline constexpr unsigned kEndpointNameMaxBytes = 98;          // MIDI_STREAM_MESSAGE_ENDPOINT_NAME_MAX_LENGTH
inline constexpr unsigned kFunctionBlockNameMaxBytes = 91;     // MIDI_STREAM_MESSAGE_FUNCTION_BLOCK_NAME_MAX_LENGTH
inline constexpr unsigned kProductInstanceIdMaxBytes = 42;     // MIDI_STREAM_MESSAGE_PRODUCT_INSTANCE_ID_MAX_LENGTH
inline constexpr unsigned kVirtualDeviceUniqueIdMaxLen = 32;   // MIDI_CONFIG_JSON_ENDPOINT_VIRTUAL_DEVICE_UNIQUE_ID_MAX_LEN

// MidiDefs.h
inline constexpr unsigned kFunctionBlockDirectionInput = 0x1;   // block receives (host -> device): a MIDI 1.0 *output* port
inline constexpr unsigned kFunctionBlockDirectionOutput = 0x2;
inline constexpr unsigned kFunctionBlockDirectionBidirectional = 0x3;
inline constexpr unsigned kFunctionBlockUiHintReceiver = 0x1;
inline constexpr unsigned kFunctionBlockMidi10Unrestricted = 0x1;
inline constexpr unsigned kStreamProtocolMidi1 = 0x01;
inline constexpr unsigned kStreamProtocolMidi2 = 0x02;

}  // namespace brack::win::midisrv
