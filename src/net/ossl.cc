/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/safestack.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <system_error>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include "net/tls-impl.hh"

#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/stack.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>
#include <seastar/util/log.hh>
#endif

namespace seastar {

static logger tls_logger{"tls"};

enum class ossl_errc : int {};

}

namespace std {

template<>
struct is_error_code_enum<seastar::ossl_errc> : true_type {};

}

template<>
struct fmt::formatter<seastar::ossl_errc> : public fmt::formatter<std::string_view> {
    auto format(seastar::ossl_errc error, fmt::format_context& ctx) const -> decltype(ctx.out()) {
        constexpr size_t error_buf_size = 256;
        // Buffer passed to ERR_error_string must be at least 256 bytes large
        // https://www.openssl.org/docs/man3.0/man3/ERR_error_string_n.html
        std::array<char, error_buf_size> buf{};
        ERR_error_string_n(
          static_cast<unsigned long>(error), buf.data(), buf.size());
        // ERR_error_string_n does include the terminating null character
        return fmt::format_to(ctx.out(), "{}", buf.data());
    }
};

namespace seastar {

class ossl_error_category : public std::error_category {
public:
    constexpr ossl_error_category() noexcept : std::error_category{} {}
    const char* name() const noexcept override {
        return "OpenSSL";
    }
    std::string message(int error) const override {
        return fmt::format("{}", static_cast<ossl_errc>(error));
    }
};

const std::error_category& tls::error_category() {
    static const ossl_error_category ec;
    return ec;
}

std::error_code make_error_code(ossl_errc e) {
    return std::error_code(static_cast<int>(e), tls::error_category());
}

class ossl_error : public std::system_error {
public:
    static ossl_error make_ossl_error(const sstring& msg) {
        auto error_codes = build_error_codes();
        auto formatted_msg = fmt::format(
          "{}: {}", msg, error_codes);

        if (error_codes.empty()) {
            return ossl_error(std::move(formatted_msg));
        } else {
            return ossl_error(std::move(formatted_msg), std::move(error_codes));
        }
    }

    const std::vector<ossl_errc>& get_ossl_error_codes() const {
        return _ossl_error_codes;
    }

private:
    explicit ossl_error(std::string msg)
      : std::system_error(0, tls::error_category(), std::move(msg)) {}
    ossl_error(std::string msg, std::vector<ossl_errc> error_codes)
      : std::system_error(make_error_code(error_codes.front()),
        std::move(msg))
      , _ossl_error_codes(std::move(error_codes)) {}

    static std::vector<ossl_errc> build_error_codes() {
        std::vector<ossl_errc> error_codes;
        for (auto code = ERR_get_error(); code != 0; code = ERR_get_error()) {
            error_codes.push_back(static_cast<ossl_errc>(code));
        }

        return error_codes;
    }
private:
    std::vector<ossl_errc> _ossl_error_codes;
};

template<typename T>
sstring asn1_str_to_str(T* asn1) {
    const auto len = ASN1_STRING_length(asn1);
    return sstring(reinterpret_cast<const char*>(ASN1_STRING_get0_data(asn1)), len);
};

static std::vector<std::byte> extract_x509_serial(X509* cert) {
    constexpr size_t serial_max = 160;
    const ASN1_INTEGER *serial_no = X509_get_serialNumber(cert);
    const size_t serial_size = std::min(serial_max, (size_t)serial_no->length);
    std::vector<std::byte> serial(
        reinterpret_cast<std::byte*>(serial_no->data),
        reinterpret_cast<std::byte*>(serial_no->data + serial_size));
    return serial;
}

static time_t extract_x509_expiry(X509* cert) {
    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    if (not_after != nullptr) {
        tm tm_struct{};
        ASN1_TIME_to_tm(not_after, &tm_struct);
        return mktime(&tm_struct);
    }
    return -1;
}

template<typename T, auto fn>
struct ssl_deleter {
    void operator()(T* ptr) { fn(ptr); }
};

// Must define this method as sk_X509_pop_free is a macro
void X509_pop_free(STACK_OF(X509)* ca) {
    sk_X509_pop_free(ca, X509_free);
}

void X509_INFO_pop_free(STACK_OF(X509_INFO)* infos) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
}

void GENERAL_NAME_pop_free(GENERAL_NAMES* gns) {
    sk_GENERAL_NAME_pop_free(gns, GENERAL_NAME_free);
}

template<typename T, auto fn>
using ssl_handle = std::unique_ptr<T, ssl_deleter<T, fn>>;

using bio_ptr = ssl_handle<BIO, BIO_free>;
using evp_pkey_ptr = ssl_handle<EVP_PKEY, EVP_PKEY_free>;
using x509_ptr = ssl_handle<X509, X509_free>;
using x509_crl_ptr = ssl_handle<X509_CRL, X509_CRL_free>;
using x509_store_ptr = ssl_handle<X509_STORE, X509_STORE_free>;
using x509_store_ctx_ptr = ssl_handle<X509_STORE_CTX, X509_STORE_CTX_free>;
using x509_chain_ptr = ssl_handle<STACK_OF(X509), X509_pop_free>;
using x509_infos_ptr = ssl_handle<STACK_OF(X509_INFO), X509_INFO_pop_free>;
using general_names_ptr = ssl_handle<GENERAL_NAMES, GENERAL_NAME_pop_free>;
using pkcs12 = ssl_handle<PKCS12, PKCS12_free>;
using ssl_ctx_ptr = ssl_handle<SSL_CTX, SSL_CTX_free>;
using ssl_ptr = ssl_handle<SSL, SSL_free>;

/// TODO: Implement the DH params impl struct
///
class tls::dh_params::impl {
public:
    explicit impl(level) {}
    impl(const blob&, x509_crt_format){}

    const EVP_PKEY* get() const { return _pkey.get(); }

    operator const EVP_PKEY*() const { return _pkey.get(); }

private:
    evp_pkey_ptr _pkey;
};

tls::dh_params::dh_params(level lvl) : _impl(std::make_unique<impl>(lvl))
{}

tls::dh_params::dh_params(const blob& b, x509_crt_format fmt)
        : _impl(std::make_unique<impl>(b, fmt)) {
}

// TODO(rob) some small amount of code duplication here
tls::dh_params::~dh_params() = default;

tls::dh_params::dh_params(dh_params&&) noexcept = default;
tls::dh_params& tls::dh_params::operator=(dh_params&&) noexcept = default;

class tls::certificate_credentials::impl {
    struct certkey_pair {
        x509_ptr cert;
        evp_pkey_ptr key;
        explicit operator bool() const noexcept {
            return cert != nullptr && key != nullptr;
        }
    };

    static const int credential_store_idx = 0;

public:
    // This callback is designed to intercept the verification process and to implement an additional
    // check, returning 0 or -1 will force verification to fail.
    //
    // However it has been implemented in this case soley to cache the last observed certificate so
    // that it may be inspected during the session::verify() method, if desired.
    //
    static int verify_callback(int preverify_ok, X509_STORE_CTX* store_ctx) {
        // Grab the 'this' pointer from the stores generic data cache, it should always exist
        auto store = X509_STORE_CTX_get0_store(store_ctx);
        auto credential_impl = static_cast<impl*>(X509_STORE_get_ex_data(store, credential_store_idx));
        assert(credential_impl != nullptr);
        // Store a pointer to the current connection certificate within the impl instance
        auto cert = X509_STORE_CTX_get_current_cert(store_ctx);
        X509_up_ref(cert);
        credential_impl->_last_cert = x509_ptr(cert);
        return preverify_ok;
    }

    impl() : _creds([] {
        auto store = X509_STORE_new();
        if(store == nullptr) {
            throw std::bad_alloc();
        }
        X509_STORE_set_verify_cb(store, verify_callback);
        return store;
    }()) {
        // The static verify_callback above will use the stored pointer to 'this' to store the last
        // observed x509 certificate
        assert(X509_STORE_set_ex_data(_creds.get(), credential_store_idx, this) == 1);
    }


    // Parses a PEM certificate file that may contain more then one entry, calls the callback provided
    // passing the associated X509_INFO* argument. The parameter is not retained so the caller must retain
    // the item before the end of the function call.
    template<typename LoadFunc>
    static void iterate_pem_certs(const bio_ptr& cert_bio, LoadFunc fn) {
        auto infos = x509_infos_ptr(PEM_X509_INFO_read_bio(cert_bio.get(), nullptr, nullptr, nullptr));
        auto num_elements = sk_X509_INFO_num(infos.get());
        if (num_elements <= 0) {
            throw ossl_error::make_ossl_error("Failed to parse PEM cert");
        }
        for (auto i=0; i < num_elements; i++) {
            auto object = sk_X509_INFO_value(infos.get(), i);
            fn(object);
        }
    }

    static x509_ptr parse_x509_cert(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_ptr cert;
        switch(fmt) {
        case tls::x509_crt_format::PEM:
            cert = x509_ptr(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr));
            break;
        case tls::x509_crt_format::DER:
            cert = x509_ptr(d2i_X509_bio(cert_bio.get(), nullptr));
            break;
        }
        if (!cert) {
            throw ossl_error::make_ossl_error("Failed to parse x509 certificate");
        }
        return cert;
    }

    void set_x509_trust(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_ptr cert;
        switch(fmt) {
        case tls::x509_crt_format::PEM:
            iterate_pem_certs(cert_bio, [this](X509_INFO* info){
                if (!info->x509) {
                    throw ossl_error::make_ossl_error("Failed to parse x509 cert");
                }
                X509_STORE_add_cert(*this, info->x509);
            });
            break;
        case tls::x509_crt_format::DER:
            cert = x509_ptr(d2i_X509_bio(cert_bio.get(), nullptr));
            if (!cert) {
                throw ossl_error::make_ossl_error("Failed to parse x509 certificate");
            }
            X509_STORE_add_cert(*this, cert.get());
            break;
        }
    }

    void set_x509_crl(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_crl_ptr crl;
        switch(fmt) {
        case x509_crt_format::PEM:
            iterate_pem_certs(cert_bio, [this](X509_INFO* info) {
                if (!info->crl) {
                    throw ossl_error::make_ossl_error("Failed to parse CRL");
                }
                X509_STORE_add_crl(*this, info->crl);
            });
            break;
        case x509_crt_format::DER:
            crl = x509_crl_ptr(d2i_X509_CRL_bio(cert_bio.get(), nullptr));
            if (!crl) {
                throw ossl_error::make_ossl_error("Failed to parse x509 crl");
            }
            X509_STORE_add_crl(*this, crl.get());
            break;
        }
    }

    void set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
        auto x509_cert = parse_x509_cert(cert, fmt);
        bio_ptr key_bio(BIO_new_mem_buf(key.begin(), key.size()));
        evp_pkey_ptr pkey;
        switch(fmt) {
        case x509_crt_format::PEM:
            pkey = evp_pkey_ptr(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
            break;
        case x509_crt_format::DER:
            pkey = evp_pkey_ptr(d2i_PrivateKey_bio(key_bio.get(), nullptr));
            break;
        default:
            __builtin_unreachable();
        }
        if (!pkey) {
            throw ossl_error::make_ossl_error("Error attempting to parse private key");
        }
        if (!X509_check_private_key(x509_cert.get(), pkey.get())) {
            throw ossl_error::make_ossl_error("Failed to verify cert/key pair");
        }
        _cert_and_key = certkey_pair{.cert = std::move(x509_cert), .key = std::move(pkey)};
    }

    void set_simple_pkcs12(const blob& b, x509_crt_format, const sstring& password) {
        // Load the PKCS12 file
        bio_ptr bio(BIO_new_mem_buf(b.begin(), b.size()));
        if (auto p12 = pkcs12(d2i_PKCS12_bio(bio.get(), nullptr))) {
            // Extract the certificate and private key from PKCS12, using provided password
            EVP_PKEY *pkey = nullptr;
            X509 *cert = nullptr;
            STACK_OF(X509) *ca = nullptr;
            if (!PKCS12_parse(p12.get(), password.c_str(), &pkey, &cert, &ca)) {
                throw ossl_error::make_ossl_error("Failed to extract cert key pair from pkcs12 file");
            }
            // Ensure signature validation checks pass before continuing
            if (!X509_check_private_key(cert, pkey)) {
                X509_free(cert);
                EVP_PKEY_free(pkey);
                throw ossl_error::make_ossl_error("Failed to verify cert/key pair");
            }
            _cert_and_key = certkey_pair{.cert = x509_ptr(cert), .key = evp_pkey_ptr(pkey)};

            // Iterate through all elements in the certificate chain, adding them to the store
            auto ca_ptr = x509_chain_ptr(ca);
            if (ca_ptr) {
                auto num_elements = sk_X509_num(ca_ptr.get());
                while (num_elements > 0) {
                    auto e = sk_X509_pop(ca_ptr.get());
                    X509_STORE_add_cert(*this, e);
                    // store retains certificate
                    X509_free(e);
                    num_elements -= 1;
                }
            }
        } else {
            throw ossl_error::make_ossl_error("Failed to parse pkcs12 file");
        }
    }

    void dh_params(const tls::dh_params&) {}

    std::vector<cert_info> get_x509_info() const {
        if (_cert_and_key.cert) {
            return {
                cert_info{
                    .serial = extract_x509_serial(_cert_and_key.cert.get()),
                    .expiry = extract_x509_expiry(_cert_and_key.cert.get())}
            };
        }
        return {};
    }

    std::vector<cert_info> get_x509_trust_list_info() const {
        std::vector<cert_info> cert_infos;
        STACK_OF(X509_OBJECT) *chain = X509_STORE_get0_objects(_creds.get());
        auto num_elements = sk_X509_OBJECT_num(chain);
        for (auto i=0; i < num_elements; i++) {
            auto object = sk_X509_OBJECT_value(chain, i);
            auto type = X509_OBJECT_get_type(object);
            if (type == X509_LU_X509) {
                auto cert = X509_OBJECT_get0_X509(object);
                cert_infos.push_back(cert_info{
                        .serial = extract_x509_serial(cert),
                        .expiry = extract_x509_expiry(cert)});
            }
        }
        return cert_infos;
    }

    void set_client_auth(client_auth ca) {
        _client_auth = ca;
    }
    client_auth get_client_auth() const {
        return _client_auth;
    }

    void set_priority_string(const sstring& priority) {
        _priority = priority;
    }

    void set_dn_verification_callback(dn_callback cb) {
        _dn_callback = std::move(cb);
    }

    const sstring& get_priority_string() const { return _priority; }

    // Returns the certificate of last attempted verification attempt, if there was no attempt,
    // this will not be updated and will remain stale
    const x509_ptr& get_last_cert() const { return _last_cert; }

    operator X509_STORE*() const { return _creds.get(); }

    const certkey_pair& get_certkey_pair() const {
        return _cert_and_key;
    }

private:
    friend class certificate_credentials;
    friend class credentials_builder;
    friend class session;

    void set_load_system_trust(bool trust) {
        _load_system_trust = trust;
    }

    bool need_load_system_trust() const {
        return _load_system_trust;
    }

    x509_ptr _last_cert;
    x509_store_ptr _creds;

    certkey_pair _cert_and_key;
    std::shared_ptr<tls::dh_params::impl> _dh_params;
    client_auth _client_auth = client_auth::NONE;
    bool _load_system_trust = false;
    dn_callback _dn_callback;
    sstring _priority;
};

tls::certificate_credentials::certificate_credentials()
        : _impl(make_shared<impl>()) {
}

tls::certificate_credentials::~certificate_credentials() {
}

tls::certificate_credentials::certificate_credentials(
        certificate_credentials&&) noexcept = default;
tls::certificate_credentials& tls::certificate_credentials::operator=(
        certificate_credentials&&) noexcept = default;

void tls::certificate_credentials::set_x509_trust(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_trust(b, fmt);
}

void tls::certificate_credentials::set_x509_crl(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_crl(b, fmt);

}
void tls::certificate_credentials::set_x509_key(const blob& cert,
        const blob& key, x509_crt_format fmt) {
    _impl->set_x509_key(cert, key, fmt);
}

void tls::certificate_credentials::set_simple_pkcs12(const blob& b,
        x509_crt_format fmt, const sstring& password) {
    _impl->set_simple_pkcs12(b, fmt, password);
}

future<> tls::certificate_credentials::set_system_trust() {
    _impl->_load_system_trust = true;
    return make_ready_future<>();
}

void tls::certificate_credentials::set_priority_string(const sstring& prio) {
    _impl->set_priority_string(prio);
}

void tls::certificate_credentials::set_dn_verification_callback(dn_callback cb) {
    _impl->set_dn_verification_callback(std::move(cb));
}

std::optional<std::vector<cert_info>> tls::certificate_credentials::get_cert_info() const noexcept {
    if (_impl == nullptr) {
        return std::nullopt;
    }

    try {
        auto result = _impl->get_x509_info();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<cert_info>> tls::certificate_credentials::get_trust_list_info() const noexcept {
    if (_impl == nullptr) {
        return std::nullopt;
    }

    try {
        auto result = _impl->get_x509_trust_list_info();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void tls::certificate_credentials::enable_load_system_trust() {
    _impl->_load_system_trust = true;
}

void tls::certificate_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

tls::server_credentials::server_credentials()
    : server_credentials(dh_params{})
{}

tls::server_credentials::server_credentials(shared_ptr<dh_params> dh)
    : server_credentials(*dh)
{}

tls::server_credentials::server_credentials(const dh_params& dh) {
    _impl->dh_params(dh);
}

tls::server_credentials::server_credentials(server_credentials&&) noexcept = default;
tls::server_credentials& tls::server_credentials::operator=(
        server_credentials&&) noexcept = default;

void tls::server_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

namespace tls {
/**
 * Session wraps an OpenSSL SSL session and context,
 * and is the actual conduit for an TLS/SSL data flow.
 *
 * We use a connected_socket and its sink/source
 * for IO. Note that we need to keep ownership
 * of these, since we handle handshake etc.
 *
 * The implmentation below relies on OpenSSL, for the gnutls implementation
 * see tls.cc and the CMake option 'Seastar_WITH_OSSL'
 */
class session : public enable_shared_from_this<session>, public session_impl {
public:
    using buf_type = temporary_buffer<char>;
    using frag_iter = net::fragment*;

    session(session_type t, shared_ptr<tls::certificate_credentials> creds,
            std::unique_ptr<net::connected_socket_impl> sock, tls_options options = {})
      : _type(t)
      , _sock(std::move(sock))
      , _creds(creds->_impl)
      , _in(_sock->source())
      , _out(_sock->sink())
      , _in_sem(1)
      , _out_sem(1)
      , _options(std::move(options))
      , _output_pending(make_ready_future<>())
      , _ctx(make_ssl_context())
      , _ssl([this]() {
          auto ssl = SSL_new(_ctx.get());
          if (!ssl) {
              throw ossl_error::make_ossl_error("Failed to create SSL session");
          }
          return ssl;
      }())
      , _in_bio(BIO_new(BIO_s_mem()))
      , _out_bio(BIO_new(BIO_s_mem())) {
        if (!_in_bio || !_out_bio) {
            if (_in_bio) {
                BIO_free(_in_bio);
                _in_bio = nullptr;
            }
            if (_out_bio) {
                BIO_free(_out_bio);
                _out_bio = nullptr;
            }
            throw std::runtime_error("Failed to create BIOs");
        }
        if (_type == session_type::SERVER) {
            tls_logger.info("SERVER");
            SSL_set_accept_state(_ssl.get());
        } else {
            tls_logger.info("CLIENT");
            if (!_options.server_name.empty()) {
                SSL_set_tlsext_host_name(
                  _ssl.get(), _options.server_name.c_str());
            }
            SSL_set_connect_state(_ssl.get());
        }
        // SSL_set_bio transfers ownership of the read and write bios to the SSL
        // instance
        SSL_set_bio(_ssl.get(), _in_bio, _out_bio);
    }

    session(session_type t, shared_ptr<certificate_credentials> creds,
            connected_socket sock,
            tls_options options = {})
            : session(t, std::move(creds), net::get_impl::get(std::move(sock)), options) {}

    ~session() {
        assert(_output_pending.available());
    }

    future<> wait_for_output() {
        tls_logger.info("wait_for_output");
        return std::exchange(_output_pending, make_ready_future())
          .handle_exception([this](auto ep) {
              _error = ep;
              return make_exception_future(ep);
          });
    }

    // This function will attempt to read data out of the OpenSSL _out_bio
    // which the SSL session writes to.  If any data is presnent, it will
    // push it into the _out stream and save off the future into `_output_pending`.
    // If there is data waiting to be sent, this function will wait for
    // `_output_pending` to resolve.
    future<> perform_push() {
        tls_logger.info("perform_push");
        return _output_pending.then([this] {
            tls_logger.info("perform_push post _output_pending");
            auto msg = make_lw_shared<scattered_message<char>>();
            return repeat_until_value(
                [this, msg = scattered_message<char>()] () mutable {
                    using ret_t = std::optional<scattered_message<char>>;
                    buf_type buf(BIO_ctrl_pending(_out_bio));
                    auto n = BIO_read(
                        _out_bio, buf.get_write(), buf.size());
                    if (n > 0) {
                        buf.trim(n);
                        msg.append(std::move(buf));
                    } else if (!BIO_should_retry(_out_bio)) {
                        _error = std::make_exception_ptr(
                            ossl_error::make_ossl_error(
                                "Failed to read data from _out_bio"));
                        return make_exception_future<ret_t>(_error);
                    }
                    if (BIO_ctrl_pending(_out_bio) == 0) {
                        return make_ready_future<ret_t>(std::move(msg));
                    }
                    return make_ready_future<ret_t>();
                }
            ).then([this](scattered_message<char> msg) mutable {
                if (msg.size() > 0) {
                    _output_pending = _out.put(std::move(msg).release());
                } else {
                    _output_pending = make_ready_future();
                }
            });
        });
    }

    // This function will check to see if there is any data sitting in the
    // _out_bio, which is the BIO that the SSL session writes to to send
    // data.  If there is, it call `perform_push` and wait for the data
    // to be sent.  If there is no data to be sent, this function returns
    // immediately.
    // Returns true if data is sent, false if not
    future<bool> maybe_perform_push_with_wait() {
        tls_logger.info("{}: maybe_perform_push", _type == session_type::SERVER ? "S": "C");
        if (BIO_ctrl_pending(_out_bio) > 0) {
            tls_logger.info(
              "{}: maybe_perform_push BIO_ctrl_pending(_out_bio): {}", _type == session_type::SERVER ? "S": "C",
              BIO_ctrl_pending(_out_bio));
            return perform_push().then([this] {
                return wait_for_output();
            }).then([]() {
                return true;
            });
        } else {
            tls_logger.info("{}: maybe_perform_push nothing to send", _type == session_type::SERVER ? "S": "C");
            return make_ready_future<bool>(false);
        }
    }

    future<stop_iteration> handle_do_put_ssl_err(const int ssl_err, bool &renegotitate) {
        switch(ssl_err) {
        case SSL_ERROR_ZERO_RETURN:
            // Indicates a hang up somewhere
            // Mark _eof and stop iteratio
            _eof = true;
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        case SSL_ERROR_NONE:
            // Should not have been reached in this situation
            // Continue iteration
            return make_ready_future<stop_iteration>(stop_iteration::no);
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // These 'error' codes indicate to the caller that before we can
            // continue writing data to the SSL session, the SSL session needs
            // to send or receive data from the peer.  Could indicate a
            // renegotitation is required.
            renegotitate = true;
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        case SSL_ERROR_SYSCALL:
            tls_logger.info("do_put SSL_ERROR_SYSCAL");
            tls_logger.info("do_put SSL_ERROR_SYSCAL errno: {} ({})", errno, strerror(errno));
            _error = std::make_exception_ptr(std::system_error(errno, std::system_category(), "System error encountered during SSL write"));
            return make_exception_future<stop_iteration>(_error);
        case SSL_ERROR_SSL: {
            auto ec = ERR_GET_REASON(ERR_peek_error());
            if (ec == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                // Probably shouldn't have during a write, but
                // let's handle this gracefully
                _eof = true;
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }
            _error = make_exception_ptr(ossl_error::make_ossl_error(
                "Error occurred during SSL write"));
            // let's make sure there's no data to actually send
            return wait_for_output().then_wrapped([this](auto f) {
                try {
                    f.get();
                    return make_exception_future(_error);
                } catch(...) {
                    std::throw_with_nested(ossl_error::make_ossl_error(
                        "Encountered unexpected error while handling SSL error during SSL write"));
                }
            }).then([] {
                return stop_iteration::no;
            });
        }
        default:
            // Some other unhandled situation
            _error = std::make_exception_ptr(std::runtime_error(
                "Unknown error encountered during SSL write"));
            return make_exception_future<stop_iteration>(_error);
        }
    }

    // Called post locking of the _out_sem
    // This function takes and holds the sempahore units for _out_sem and
    // will attempt to send the provided packet.  If a renegotitation happens
    // any unprocessed part of the packet is returned.
    future<net::packet> do_put(net::packet p, semaphore_units<> units) {
        tls_logger.info("do_put");
        if (!connected()) {
            tls_logger.info("do_put NOT connected");
            return make_ready_future<net::packet>(std::move(p));
        }
        assert(_output_pending.available());
        return do_with(std::move(p), std::move(units), false,
            [this](net::packet & p, semaphore_units<> & units, bool & renegotitate) {
                // This do_until runs until either a renegotitation occurs or the packet is empty
                return do_until(
                    [&p, &renegotitate] { return renegotitate || p.len() == 0;},
                    [this,&p,&renegotitate]() mutable {
                        size_t off = 0;
                        return repeat([this, off, &renegotitate, &p]() mutable {
                            auto ptr = (*p.fragments().begin()).base;
                            auto size = (*p.fragments().begin()).size;
                            tls_logger.info("do_put size; {}, off: {}", size, off);
                            if (size == off) {
                                tls_logger.info("do_put off == size");
                                return make_ready_future<stop_iteration>(stop_iteration::yes);
                            }
                            size_t bytes_written = 0;
                            auto write_rc = SSL_write_ex(
                                _ssl.get(), ptr + off, size - off, &bytes_written);
                            tls_logger.info("do_put write_rc: {}", write_rc);
                            tls_logger.info("do_put connected() (post write): {}", connected());
                            if (write_rc != 1) {
                                if (!connected()) {
                                    renegotitate = true;
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                }
                                const auto ssl_err = SSL_get_error(_ssl.get(), write_rc);
                                return handle_do_put_ssl_err(ssl_err, renegotitate);
                            } else {
                                off += bytes_written;
                                p.trim_front(bytes_written);
                                return perform_push().then([this] {
                                    return wait_for_output().then([] {
                                        return stop_iteration::no;
                                    });
                                });
                            }
                        });
                    }
                ).then([&p] {
                    return p.share();
                });
            }
        );
    }

    // Used to push unencrypted data through OpenSSL, which will
    // encrypt it and then place it into the output bio.
    future<> put(net::packet p) override {
        tls_logger.info("put");
        constexpr size_t openssl_max_record_size = 16 * 1024;
        if (_error) {
            return make_exception_future(_error);
        }
        if (_shutdown) {
            return make_exception_future<>(
              std::system_error(EPIPE, std::system_category()));
        }
        if (!connected()) {
            return handshake().then(
              [this, p = std::move(p)]() mutable { return put(std::move(p)); });
        }

        if (p.nr_frags() > 1 && p.len() <= openssl_max_record_size) {
            p.linearize();
        }

        return get_units(_out_sem, 1).then([this, p = std::move(p)](auto units) mutable {
            return do_put(std::move(p), std::move(units)).then([this](net::packet p) {
                tls_logger.info("put: p.len(): {}", p.len());
                if (p.len() != 0) {
                    return handshake().then([this, p = std::move(p)]() mutable {
                        return put(std::move(p));
                    });
                } else {
                    return make_ready_future();
                }
            });
        });
    }

    // Called after locking the _in_sem and _out_sem semaphores.
    future<> do_handshake() {
        tls_logger.info("do_handshake");
        if (connected()) {
            tls_logger.info("do_handshake connected");
            return make_ready_future<>();
        } else if (eof()) {
            // if we have experienced and eof, set the error and return
            // GnuTLS will probably return GNUTLS_E_PREMATURE_TERMINATION
            // from gnutls_handshake in this situation.
            _error = std::make_exception_ptr(std::system_error(
              ENOTCONN,
              std::system_category(),
              "EOF encountered during handshake"));
            return make_exception_future(_error);
        }
        try {
            auto n = SSL_do_handshake(_ssl.get());
            tls_logger.info("do_handshake SSL_do_handshake: {}", n);
            if (n <= 0) {
                auto ssl_error = SSL_get_error(_ssl.get(), n);
                tls_logger.info("do_handshake SSL_get_error: {}", ssl_error);
                switch (ssl_error) {
                case SSL_ERROR_NONE:
                    tls_logger.info("do_handshake SSL_ERROR_NONE");
                    // probably shouldn't have gotten here, but we're gtg
                    break;
                case SSL_ERROR_ZERO_RETURN:
                    tls_logger.info("do_handshake SSL_ERROR_ZERO_RETURN");
                    // peer has closed
                    _eof = true;
                    break;
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ: {
                    tls_logger.info(
                      "do_handshake {}",
                      ssl_error == SSL_ERROR_WANT_WRITE
                        ? "SSL_ERROR_WANT_WRITE"
                        : "SSL_ERROR_WANT_READ");
                    // Always first check to see if there's any data to send.  Then wait
                    // for data to be received.
                    return maybe_perform_push_with_wait().then([this](bool) {
                        return perform_pull().then(
                          [this] { return do_handshake(); });
                    });
                    break;
                }
                case SSL_ERROR_SYSCALL:
                    tls_logger.info("do_handshake SSL_ERROR_SYSCAL");
                    tls_logger.info("do_handshake SSL_ERROR_SYSCAL errno: {} ({})", errno, strerror(errno));
                    _error = std::make_exception_ptr(std::system_error(
                        errno, std::system_category(), "System error encountered during handshake"));
                    return make_exception_future(_error);
                case SSL_ERROR_SSL:
                    tls_logger.info("do_handshake SSL_ERROR_SSL");
                    // oh boy an error!
                    {
                        auto ec = ERR_GET_REASON(ERR_peek_error());
                        tls_logger.info("do_handshake ec: {}", ec);
                        switch (ec) {
                        case SSL_R_UNEXPECTED_EOF_WHILE_READING:
                            tls_logger.info(
                              "do_handshake "
                              "SSL_R_UNEXPECTED_EOF_WHILE_READING");
                            // well in this situation, the remote end closed
                            _eof = true;
                            return make_ready_future<>();
                        case SSL_R_PEER_DID_NOT_RETURN_A_CERTIFICATE:
                        case SSL_R_CERTIFICATE_VERIFY_FAILED:
                        case SSL_R_NO_CERTIFICATES_RETURNED:
                            verify();
                            // may throw, otherwise fall through
                            [[fallthrough]];
                        default:
                            if (_error == nullptr) {
                                _error = std::make_exception_ptr(
                                  ossl_error::make_ossl_error(
                                    "Failed to establish SSL handshake"));
                            }
                            return wait_for_output().then_wrapped(
                              [this](auto f) {
                                  try {
                                      f.get();
                                      return make_exception_future(_error);
                                  } catch (...) {
                                      std::throw_with_nested(
                                        ossl_error::make_ossl_error("Error"));
                                  }
                              });
                        }
                    }
                    break;
                default:
                    tls_logger.info("do_handshake default");
                    // weird situation of unknown error
                    _error = std::make_exception_ptr(std::runtime_error(
                      "Unknown error encountered during handshake"));
                    return make_exception_future(_error);
                }
            } else {
                if (
                  _type == session_type::CLIENT
                  || _creds->get_client_auth() != client_auth::NONE) {
                    tls_logger.info(
                      "do_handshake client or server with verify");
                    verify();
                }
                return maybe_perform_push_with_wait().then([](bool){
                    return make_ready_future();
                });
            }
        } catch (...) {
            return make_exception_future<>(std::current_exception());
        }
        return make_ready_future<>();
    }

    // This function will attempt to pull data off of the _in stream
    // if there isn't already data needing to be processed first.
    future<> wait_for_input() {
        tls_logger.info("wait_for_input");
        // If we already have data, then it needs to be processed
        if (!_input.empty()) {
            tls_logger.info("wait_for_input _input not empty");
            return make_ready_future();
        }
        return _in.get()
          .then([this](buf_type buf) {
              tls_logger.info("wait_for_input buf is empty: {}", buf.empty());
              // Set EOF if it's empty
              _eof |= buf.empty();
              _input = std::move(buf);
          })
          .handle_exception([this](auto ep) {
              _error = ep;
              return make_exception_future(ep);
          });
    }

    // Called after locking the _in_sem semaphore
    // This function attempts to pull unencrypted data off of the
    // SSL session using SSL_read.  If ther eis no data, then
    // we will call perform_pull and wait for data to arrive.
    future<buf_type> do_get() {
        tls_logger.info("do_get");
        // Data is available to be pulled of the SSL session if there is pending
        // data on the SSL session or there is data in the _in_bio which SSL reads
        // from
        auto data_to_pull = (BIO_ctrl_pending(_in_bio) + SSL_pending(_ssl.get())) > 0;
        tls_logger.info("do_get data_to_pull: {}", data_to_pull);
        auto f = make_ready_future<>();
        if (!data_to_pull) {
            // If nothing is in the SSL buffers then we may have to wait for
            // data to come in
            tls_logger.info("do_get using perform_pull");
            f = perform_pull();
        }
        return f.then([this] {
            tls_logger.info("do_get post f");
            if (eof()) {
                tls_logger.info("do_get eof");
                return make_ready_future<buf_type>();
            }
            tls_logger.info("do_get connected(): {}", connected());
            tls_logger.info(
              "do_get HANDSHAKE state: {}", SSL_get_state(_ssl.get()));
            auto avail = BIO_ctrl_pending(_in_bio) + SSL_pending(_ssl.get());
            tls_logger.info("do_get avail2: {} (BIO_ctrl_pending: {}, SSL_pending: {})", avail, BIO_ctrl_pending(_in_bio), SSL_pending(_ssl.get()));
            buf_type buf(avail);
            size_t bytes_read = 0;
            auto read_result = SSL_read_ex(
              _ssl.get(), buf.get_write(), avail, &bytes_read);
            tls_logger.info("do_get read_result: {}", read_result);
            if (read_result != 1) {
                tls_logger.info("do_get connected() (post SSL_read): {}", connected());
                tls_logger.info(
                    "do_get HANDSHAKE state (post SSL_read): {}", SSL_get_state(_ssl.get()));
                const auto ssl_err = SSL_get_error(_ssl.get(), read_result);
                tls_logger.info("do_get ssl_err: {}", ssl_err);
                switch (ssl_err) {
                case SSL_ERROR_ZERO_RETURN:
                    tls_logger.info("do_get SSL_ERROR_ZERO_RETURN");
                    // Remote end has closed
                    _eof = true;
                    [[fallthrough]];
                case SSL_ERROR_NONE:
                    tls_logger.info("do_get SSL_ERROR_NONE");
                    // well we shouldn't be here at all
                    return make_ready_future<buf_type>();
                case SSL_ERROR_WANT_READ:
                case SSL_ERROR_WANT_WRITE:
                    tls_logger.info(
                      "do_get {}",
                      ssl_err == SSL_ERROR_WANT_WRITE ? "SSL_ERROR_WANT_WRITE"
                                                      : "SSL_ERROR_WANT_READ");
                    // This may be caused by a renegotiation request, in this situation
                    // return an empty buffer (the get() function will initiate a handshake)
                    return make_ready_future<buf_type>();
                case SSL_ERROR_SYSCALL:
                    tls_logger.info("do_get SSL_ERROR_SYSCAL");
                    tls_logger.info("do_get SSL_ERROR_SYSCAL errno: {} ({})", errno, strerror(errno));
                    if (errno == 0) {
                        return make_ready_future<buf_type>();
                    }
                    _error = std::make_exception_ptr(std::system_error(
                        errno, std::system_category(), "System error encountered during SSL read"));
                    return make_exception_future<buf_type>(_error);
                case SSL_ERROR_SSL:
                    tls_logger.info("do_get SSL_ERROR_SSL");
                    {
                        auto ec = ERR_GET_REASON(ERR_peek_error());
                        tls_logger.info("do_get ERR_GET_REASON: {}", ec);
                        if (ec == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                            tls_logger.info(
                              "do_get SSL_R_UNEXPECTED_EOF_WHILE_READING");
                            // in this situation, the remote end hung up
                            _eof = true;
                            return make_ready_future<buf_type>();
                        }
                        _error = std::make_exception_ptr(
                          ossl_error::make_ossl_error(
                            "Failure during processing SSL read"));
                        return make_exception_future<buf_type>(_error);
                    }
                default:
                    tls_logger.info("do_get default");
                    _error = std::make_exception_ptr(std::runtime_error(
                      "Unexpected error condition during SSL read"));
                    return make_exception_future<buf_type>(_error);
                }
            } else {
                tls_logger.info("do_get buf trim {}", bytes_read);
                buf.trim(bytes_read);
                return make_ready_future<buf_type>(std::move(buf));
            }
        });
    }

    // Called by user applications to pull data off of the TLS session
    future<buf_type> get() override {
        tls_logger.info("get");
        if (_error) {
            return make_exception_future<buf_type>(_error);
        }
        if (_shutdown || eof()) {
            return make_ready_future<buf_type>(buf_type());
        }
        if (!connected()) {
            return handshake().then(std::bind(&session::get, this));
        }
        return with_semaphore(_in_sem, 1, std::bind(&session::do_get, this))
          .then([this](buf_type buf) {
              if (buf.empty() && !eof()) {
                  return handshake().then(std::bind(&session::get, this));
              }
              return make_ready_future<buf_type>(std::move(buf));
          });
    }

    // Performs shutdown
    future<> do_shutdown() {
        tls_logger.info("{}: do_shutdown", _type == session_type::SERVER ? "S": "C");
        if (_error || !connected()) {
            return make_ready_future();
        }

        auto res = SSL_shutdown(_ssl.get());
        tls_logger.info("{}: do_shutdown res: {}", _type == session_type::SERVER ? "S": "C", res);
        if (res == 1) {
            return make_ready_future();
        } else if (res == 0) {
            return yield().then([this] { return do_shutdown(); });
        } else {
            auto ssl_err = SSL_get_error(_ssl.get(), res);
            tls_logger.info("{}: do_shutdown ssl_err: {}", _type == session_type::SERVER ? "S": "C", ssl_err);
            switch (ssl_err) {
            case SSL_ERROR_NONE:
                tls_logger.info("{}: do_shutdown SSL_ERROR_NONE", _type == session_type::SERVER ? "S": "C");
                // this is weird, yield and try again
                return yield().then([this] { return do_shutdown(); });
            case SSL_ERROR_ZERO_RETURN:
                tls_logger.info("{}: do_shutdown SSL_ERROR_ZERO_RETURN", _type == session_type::SERVER ? "S": "C");
                // Looks like the other end is done, so let's just assume we're
                // done as well
                return make_ready_future();
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                tls_logger.info(
                  "{}: do_shutdown {}", _type == session_type::SERVER ? "S": "C",
                  ssl_err == SSL_ERROR_WANT_WRITE ? "SSL_ERROR_WANT_WRITE"
                                                  : "SSL_ERROR_WANT_READ");
                return maybe_perform_push_with_wait().then([this](bool sent_data) {
                    // In neither case do we actually want to pull data off of the socket (yet)
                    // If we initiate the shutdown, then we just send the shutdown alert and wait
                    // for EOF (outside of this function)
                    if (sent_data) {
                        tls_logger.info("do_shutdown sent data, loop");
                        return do_shutdown();
                    } else {
                        tls_logger.info("do_shutdown did not send any data, so assume we're done");
                        return make_ready_future();
                    }
                });
            case SSL_ERROR_SYSCALL:
                tls_logger.info("do_shutdown SSL_ERROR_SYSCAL");
                tls_logger.info("do_shutdown SSL_ERROR_SYSCAL errno: {} ({})", errno, strerror(errno));
                _error = std::make_exception_ptr(std::system_error(errno, std::system_category(), "System error encountered during SSL shutdown"));
                return make_exception_future(_error);
            case SSL_ERROR_SSL:
                tls_logger.info(
                  "do_shutdown SSL_ERROR_SSL: {}",
                  ERR_GET_REASON(ERR_peek_error()));
                _error = std::make_exception_ptr(ossl_error::make_ossl_error(
                  "Error occurred during SSL shutdown"));
                return wait_for_output().then_wrapped([this](auto f) {
                    try {
                        f.get();
                        return make_exception_future(_error);
                    } catch (...) {
                        std::throw_with_nested(
                          ossl_error::make_ossl_error("Error"));
                    }
                });
            default:
                tls_logger.info("do_shutdown default");
                _error = std::make_exception_ptr(std::runtime_error(
                  "Unknown error occurred during SSL shutdown"));
                return make_exception_future(_error);
            }
        }
    }

    void verify() {
        tls_logger.info("verify");
        // A success return code (0) does not signify if a cert was presented or not, that
        // must be explicitly queried via SSL_get_peer_certificate
        auto res = SSL_get_verify_result(_ssl.get());
        tls_logger.info("verify SSL_get_verify_result: {}", res);
        if (res != X509_V_OK) {
            auto stat_str(X509_verify_cert_error_string(res));
            auto dn = extract_dn_information();
            if (dn) {
                std::string_view stat_str_view{stat_str};
                if (stat_str_view.ends_with(" ")) {
                    stat_str_view.remove_suffix(1);
                }
                throw verification_error(fmt::format(
                    R"|({} (Issuer=["{}"], Subject=["{}"]))|",
                    stat_str_view,
                    dn->issuer,
                    dn->subject));
            }
            throw verification_error(stat_str);
        } else if (SSL_get0_peer_certificate(_ssl.get()) == nullptr) {
            // If a peer certificate was not presented,
            // SSL_get_verify_result will return X509_V_OK:
            // https://www.openssl.org/docs/man3.0/man3/SSL_get_verify_result.html
            if (
              _type == session_type::SERVER
              && _creds->get_client_auth() == client_auth::REQUIRE) {
                throw verification_error("no certificate presented by peer");
            }
            return;
        }

        if (_creds->_dn_callback) {
            auto dn = extract_dn_information();
            assert(dn.has_value());
            _creds->_dn_callback(
              _type, std::move(dn->subject), std::move(dn->issuer));
        }
    }

    bool eof() const {
        return _eof;
    }

    bool connected() const {
        return SSL_is_init_finished(_ssl.get());
    }

    future<> wait_for_eof() {
        tls_logger.info("wait_for_eof");
        if (!_options.wait_for_eof_on_shutdown) {
            // Seastar option to allow users to just bypass EOF waiting
            return make_ready_future();
        }
        return with_semaphore(_in_sem, 1, [this] {
            if (_error || !connected()) {
                tls_logger.info("errored or not connected");
                return make_ready_future();
            }
            return do_until(
                [this] { return eof(); },
                [this] { return do_get().discard_result(); });
        });
    }

    future<> handshake() {
        tls_logger.info("handshake");
        if (_creds->need_load_system_trust()) {
            if (!SSL_CTX_set_default_verify_paths(_ctx.get())) {
                throw ossl_error::make_ossl_error(
                  "Could not load system trust");
            }
            _creds->set_load_system_trust(false);
        }

        return with_semaphore(_in_sem, 1, [this] {
            return with_semaphore(_out_sem, 1, [this] {
                return do_handshake().handle_exception([this](auto ep) {
                    if (!_error) {
                        _error = ep;
                    }
                    return make_exception_future<>(_error);
                });
            });
        });
    }

    future<> shutdown() {
        tls_logger.info("{}: shutdown", _type == session_type::SERVER ? "S": "C");
        // first, make sure any pending write is done.
        // bye handshake is a flush operation, but this
        // allows us to not pay extra attention to output state
        //
        // we only send a simple "bye" alert packet. Then we
        // read from input until we see EOF. Any other reader
        // before us will get it instead of us, and mark _eof = true
        // in which case we will be no-op. This is performed all
        // within do_shutdown
        return with_semaphore(_out_sem, 1,
                             std::bind(&session::do_shutdown, this)).then(
                              std::bind(&session::wait_for_eof, this)).finally([me = shared_from_this()] {});
        // note moved finally clause above. It is theorethically possible
        // that we could complete do_shutdown just before the close calls
        // below, get pre-empted, have "close()" finish, get freed, and
        // then call wait_for_eof on stale pointer.
    }

    void close() noexcept override {
        tls_logger.info("{}: close", _type == session_type::SERVER ? "S": "C");
        // only do once.
        if (!std::exchange(_shutdown, true)) {
            // running in background. try to bye-handshake us nicely, but after 10s we forcefully close.
            (void)with_timeout(
              timer<>::clock::now() + std::chrono::seconds(10), shutdown())
              .finally([this] {
                  _eof = true;
                  return _in.close();
              }).finally([this] {
                  return _out.close();
              }).finally([this] {
                  return with_semaphore(_in_sem, 1, [this] {
                      return with_semaphore(_out_sem, 1, [] {
                          tls_logger.info("close completed");
                      });
                  });
              }).handle_exception([me = shared_from_this()](std::exception_ptr) {                
              }).discard_result();
        }
    }
    // helper for sink
    future<> flush() noexcept override {
        tls_logger.info("Performing flush");
        return with_semaphore(_out_sem, 1, [this] { return _out.flush(); });
    }

    seastar::net::connected_socket_impl& socket() const override {
        return *_sock;
    }

    future<std::optional<session_dn>> get_distinguished_name() override {
        tls_logger.info("get_distinguished_name");
        using result_t = std::optional<session_dn>;
        if (_error) {
            return make_exception_future<result_t>(_error);
        }
        if (_shutdown) {
            return make_exception_future<result_t>(
              std::system_error(ENOTCONN, std::system_category()));
        }
        if (!connected()) {
            return handshake().then(
              [this]() mutable { return get_distinguished_name(); });
        }
        result_t dn = extract_dn_information();
        return make_ready_future<result_t>(std::move(dn));
    }

    future<std::vector<subject_alt_name>> get_alt_name_information(
      std::unordered_set<subject_alt_name_type> types) override {
        tls_logger.info("get_alt_name_information");
        using result_t = std::vector<subject_alt_name>;

        if (_error) {
            return make_exception_future<result_t>(_error);
        }
        if (_shutdown) {
            return make_exception_future<result_t>(
              std::system_error(ENOTCONN, std::system_category()));
        }
        if (!connected()) {
            tls_logger.info("get_alt_name_information not connected");
            return handshake().then([this, types = std::move(types)]() mutable {
                tls_logger.info("get_alt_name_information NOW connected");
                return get_alt_name_information(std::move(types));
            });
        }

        const auto& peer_cert = get_peer_certificate();
        if (!peer_cert) {
            tls_logger.info("get_alt_name_information no peer cert");
            return make_ready_future<result_t>();
        }
        return make_ready_future<result_t>(
          do_get_alt_name_information(peer_cert, types));
    }

private:
    std::vector<subject_alt_name> do_get_alt_name_information(const x509_ptr &peer_cert,
                                                              const std::unordered_set<subject_alt_name_type> &types) const {
    tls_logger.info("do_get_alt_name");
        tls_logger.info("do_get_alt_name");
        int ext_idx = X509_get_ext_by_NID(
          peer_cert.get(), NID_subject_alt_name, -1);
        if (ext_idx < 0) {
            return {};
        }
        auto ext = X509_get_ext(peer_cert.get(), ext_idx);
        if (!ext) {
            return {};
        }
        auto names = general_names_ptr(static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(ext)));
        if (!names) {
            return {};
        }
        int num_names = sk_GENERAL_NAME_num(names.get());
        std::vector<subject_alt_name> alt_names;
        alt_names.reserve(num_names);

        for (auto i = 0; i < num_names; i++) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), i);
            if (auto known_t = field_to_san_type(name)) {
                if (types.empty() || types.count(known_t->type)) {
                    alt_names.push_back(std::move(*known_t));
                }
            }
        }
        return alt_names;
    }

    std::optional<subject_alt_name> field_to_san_type(GENERAL_NAME* name) const {
        subject_alt_name san;
        switch(name->type) {
            case GEN_IPADD:
            {
                san.type = subject_alt_name_type::ipaddress;
                const auto* data = ASN1_STRING_get0_data(name->d.iPAddress);
                const auto size = ASN1_STRING_length(name->d.iPAddress);
                if (size == sizeof(::in_addr)) {
                    ::in_addr addr;
                    memcpy(&addr, data, size);
                    san.value = net::inet_address(addr);
                } else if (size == sizeof(::in6_addr)) {
                    ::in6_addr addr;
                    memcpy(&addr, data, size);
                    san.value = net::inet_address(addr);
                } else {
                    throw std::runtime_error(fmt::format("Unexpected size: {} for ipaddress alt name value", size));
                }
                break;
            }
            case GEN_EMAIL:
            {
                san.type = subject_alt_name_type::rfc822name;
                san.value = asn1_str_to_str(name->d.rfc822Name);
                break;
            }
            case GEN_URI:
            {
                san.type = subject_alt_name_type::uri;
                san.value = asn1_str_to_str(name->d.uniformResourceIdentifier);
                break;
            }
            case GEN_DNS:
            {
                san.type = subject_alt_name_type::dnsname;
                san.value = asn1_str_to_str(name->d.dNSName);
                break;
            }
            case GEN_OTHERNAME:
            {
                san.type = subject_alt_name_type::othername;
                san.value = asn1_str_to_str(name->d.dNSName);
                break;
            }
            case GEN_DIRNAME:
            {
                san.type = subject_alt_name_type::dn;
                auto dirname = get_dn_string(name->d.directoryName);
                if (!dirname) {
                    throw std::runtime_error("Expected non null value for SAN dirname");
                }
                san.value = std::move(*dirname);
                break;
            }
            default:
                return std::nullopt;
        }
        return san;
    }

    const x509_ptr& get_peer_certificate() const {
        return _creds->get_last_cert();
    }

    std::optional<session_dn> extract_dn_information() const {
        const auto& peer_cert = get_peer_certificate();
        if (!peer_cert) {
            return std::nullopt;
        }
        auto subject = get_dn_string(X509_get_subject_name(peer_cert.get()));
        auto issuer = get_dn_string(X509_get_issuer_name(peer_cert.get()));
        if (!subject || !issuer) {
            throw ossl_error::make_ossl_error(
              "error while extracting certificate DN strings");
        }
        return session_dn{
          .subject = std::move(*subject), .issuer = std::move(*issuer)};
    }

    ssl_ctx_ptr make_ssl_context() {
        auto ssl_ctx = ssl_ctx_ptr(SSL_CTX_new(TLSv1_2_method()));
        // auto ssl_ctx = ssl_ctx_ptr(SSL_CTX_new(TLS_method()));
        if (!ssl_ctx) {
            throw ossl_error::make_ossl_error(
              "Failed to initialize SSL context");
        }
        const auto& ck_pair = _creds->get_certkey_pair();
        if (_type == session_type::SERVER) {
            if (!ck_pair) {
                throw ossl_error::make_ossl_error(
                  "Cannot start session without cert/key pair for server");
            }
            switch (_creds->get_client_auth()) {
            case client_auth::NONE:
            default:
                SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_NONE, nullptr);
                break;
            case client_auth::REQUEST:
                SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
                break;
            case client_auth::REQUIRE:
                SSL_CTX_set_verify(
                  ssl_ctx.get(),
                  SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                  nullptr);
                break;
            }

            SSL_CTX_set_options(
              ssl_ctx.get(), SSL_OP_ALL | SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
        }

        // Servers must supply both certificate and key, clients may
        // optionally use these
        if (ck_pair) {
            if (!SSL_CTX_use_cert_and_key(
                  ssl_ctx.get(),
                  ck_pair.cert.get(),
                  ck_pair.key.get(),
                  nullptr,
                  1)) {
                throw ossl_error::make_ossl_error(
                  "Failed to load cert/key pair");
            }
        }
        // Increments the reference count of *_creds, now should have a
        // total ref count of two, will be deallocated when both OpenSSL and
        // the certificate_manager call X509_STORE_free
        SSL_CTX_set1_cert_store(ssl_ctx.get(), *_creds);

        if (_creds->get_priority_string() != "") {
            if (SSL_CTX_set_cipher_list(ssl_ctx.get(),
            _creds->get_priority_string().c_str()) != 1) {
                throw ossl_error::make_ossl_error("Failed to set priority list");
            }
        }
        return ssl_ctx;
    }

    static std::optional<sstring> get_dn_string(X509_NAME* name) {
        auto out = bio_ptr(BIO_new(BIO_s_mem()));
        if (-1 == X509_NAME_print_ex(out.get(), name, 0, ASN1_STRFLGS_RFC2253 | XN_FLAG_SEP_COMMA_PLUS |
                                     XN_FLAG_FN_SN | XN_FLAG_DUMP_UNKNOWN_FIELDS)) {
            return std::nullopt;
        }
        char* bio_ptr = nullptr;
        auto len = BIO_get_mem_data(out.get(), &bio_ptr);
        if (len < 0) {
            throw ossl_error::make_ossl_error("Failed to allocate DN string");
        }
        return sstring(bio_ptr, len);
    }

    future<> perform_pull() {
        tls_logger.info("{}: perform_pull", _type == session_type::SERVER ? "S": "C");
        return wait_for_input().then([this] {
            tls_logger.info(
              "{}: perform_pull post wait_for_input, _input.size(): {}", _type == session_type::SERVER ? "S": "C",
              _input.size());
            if (eof() || _input.empty()) {
                tls_logger.info("perform_pull eof");
                _eof = true;
                return make_ready_future<>();
            }
            return do_until(
              [this] { return _input.empty(); },
              [this] {
                  const auto n = BIO_write(
                    _in_bio, _input.get(), _input.size());
                  tls_logger.info("perform_pull BIO_write: {}", n);
                  if (n <= 0) {
                      _error = std::make_exception_ptr(
                        ossl_error::make_ossl_error(
                          "Error while inserting into _in_bio"));
                      return make_exception_future(_error);
                  }
                  _input.trim_front(n);
                  return make_ready_future();
              });
        });
    }

    size_t in_avail() const { return _input.size(); }

private:
    session_type _type;
    std::unique_ptr<net::connected_socket_impl> _sock;
    shared_ptr<tls::certificate_credentials::impl> _creds;
    data_source _in;
    data_sink _out;
    std::exception_ptr _error;

    bool _eof = false;
    semaphore _in_sem, _out_sem;
    tls_options _options;

    bool _shutdown = false;
    future<> _output_pending;
    buf_type _input;
    ssl_ctx_ptr _ctx;
    ssl_ptr _ssl;
    BIO* _in_bio = nullptr;
    BIO* _out_bio = nullptr;
};
} // namespace tls

future<connected_socket> tls::wrap_client(shared_ptr<certificate_credentials> cred, connected_socket&& s, sstring name) {
    tls_options options{.server_name = std::move(name)};
    return wrap_client(std::move(cred), std::move(s), std::move(options));
}

future<connected_socket> tls::wrap_client(shared_ptr<certificate_credentials> cred, connected_socket&& s, tls_options options) {
    session_ref sess(seastar::make_shared<session>(session_type::CLIENT, std::move(cred), std::move(s),  options));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

future<connected_socket> tls::wrap_server(shared_ptr<server_credentials> cred, connected_socket&& s) {
    session_ref sess(seastar::make_shared<session>(session_type::SERVER, std::move(cred), std::move(s)));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

} // namespace seastar

const int seastar::tls::ERROR_UNKNOWN_COMPRESSION_ALGORITHM = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_COMPRESSION_ALGORITHM);
const int seastar::tls::ERROR_UNKNOWN_CIPHER_TYPE = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNKNOWN_CIPHER_TYPE);
const int seastar::tls::ERROR_INVALID_SESSION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_INVALID_SESSION_ID);
const int seastar::tls::ERROR_UNEXPECTED_HANDSHAKE_PACKET = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_RECORD);
const int seastar::tls::ERROR_UNKNOWN_CIPHER_SUITE = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_PROTOCOL);
const int seastar::tls::ERROR_UNKNOWN_ALGORITHM = ERR_PACK(
  ERR_LIB_RSA, 0, RSA_R_UNKNOWN_ALGORITHM_TYPE);
const int seastar::tls::ERROR_UNSUPPORTED_SIGNATURE_ALGORITHM = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_NO_SUITABLE_SIGNATURE_ALGORITHM);
const int seastar::tls::ERROR_SAFE_RENEGOTIATION_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_RENEGOTIATION_MISMATCH);
const int seastar::tls::ERROR_UNSAFE_RENEGOTIATION_DENIED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSAFE_LEGACY_RENEGOTIATION_DISABLED);
const int seastar::tls::ERROR_UNKNOWN_SRP_USERNAME = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_INVALID_SRP_USERNAME);
const int seastar::tls::ERROR_PREMATURE_TERMINATION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_EOF_WHILE_READING);
const int seastar::tls::ERROR_PUSH = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_BIO_NOT_SET);
const int seastar::tls::ERROR_PULL = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_READ_BIO_NOT_SET);
const int seastar::tls::ERROR_UNEXPECTED_PACKET = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_MESSAGE);
const int seastar::tls::ERROR_UNSUPPORTED_VERSION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_SSL_VERSION);
const int seastar::tls::ERROR_NO_CIPHER_SUITES = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_NO_CIPHERS_AVAILABLE);
const int seastar::tls::ERROR_DECRYPTION_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_DECRYPTION_FAILED);
const int seastar::tls::ERROR_MAC_VERIFY_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC);
