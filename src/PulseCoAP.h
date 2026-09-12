// PulseCoAP.h
// Umbrella header — #include <PulseCoAP.h> and you get the whole library.
//
// Override any PULSECOAP_* macro (see PulseCoAPConfig.h) with a #define
// BEFORE this include to change buffer sizes, disable a role
// (PULSECOAP_ROLE_CLIENT_ONLY / PULSECOAP_ROLE_SERVER_ONLY), or opt into
// PULSECOAP_ENABLE_TRACE / PULSECOAP_ENABLE_BLOCKWISE.
#pragma once

#include "PulseCoAPConfig.h"
#include "PulseCoAPTypes.h"
#include "PulseCoAPMessage.h"
#include "PulseCoAPUri.h"
#include "PulseCoAPTransport.h"
#include "PulseCoAPTransaction.h"

#if PULSECOAP_ENABLE_SERVER
#include "PulseCoAPServer.h"
#endif

#if PULSECOAP_ENABLE_CLIENT
#include "PulseCoAPClient.h"
#endif

#ifdef ARDUINO
#include "PulseCoAPTransportArduinoUDP.h"
#if PULSECOAP_ENABLE_DTLS
#include "PulseCoAPTransportDTLS.h"
#endif
#endif
