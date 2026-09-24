// libcurl TLS bridging helpers. Included from every direct-curl call site so
// the trust-store configuration lives in exactly one place.

#pragma once

#include <curl/curl.h>

namespace yoai { namespace client {

// Tell libcurl to consult the platform's native trust store (Windows Schannel,
// macOS keychain) at handshake time, regardless of which SSL backend it was
// built against.
//
// On Windows + vcpkg, libcurl is wired against OpenSSL, which only reads its
// compiled-in cert path — frequently a non-existent location on the user's
// machine. The result is every HTTPS request failing with
//
//     SSL peer certificate or SSH remote key was not OK
//
// even for certs signed by public CAs. CURLSSLOPT_NATIVE_CA flips libcurl to
// hand the OpenSSL backend the system trust roots, which is what every other
// HTTPS client on the box already does.
//
// No-op on libcurl versions that don't define the flag (added in 7.71 for
// Schannel, 7.81 broadly for OpenSSL). Safe to call on every easy handle
// before curl_easy_perform; cheap (just sets a flag).
//
// On OpenHarmony / HarmonyOS NEXT there is no NATIVE_CA path either -- curl is
// built against our cross-compiled OpenSSL, whose compiled-in default
// (OPENSSLDIR /etc/ssl -> cert.pem + a c_rehash'd certs/ dir) matches nothing
// on the device. What the OS actually ships is a Mozilla-style PEM bundle at
// /etc/ssl/certs/cacert.pem, so point CAINFO straight at it. Without this every
// HTTPS request fails the handshake with the same "SSL peer certificate or SSH
// remote key was not OK".
inline void configure_tls_trust(CURL* curl) {
#if defined(__OHOS__)
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/cacert.pem");
#elif defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
}

}
}
