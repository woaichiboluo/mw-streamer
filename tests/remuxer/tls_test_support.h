#ifndef MW_STREAMER_TESTS_REMUXER_TLS_TEST_SUPPORT_H_
#define MW_STREAMER_TESTS_REMUXER_TLS_TEST_SUPPORT_H_

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace tls_test {

inline void Check(bool success, const char* message) {
  if (!success) throw std::runtime_error(message);
}

class CertFixture final {
 public:
  static constexpr const char* kPassword = "fixture-password";

  explicit CertFixture(
      const std::string& subject_alt_names = "DNS:localhost,IP:127.0.0.1") {
    static std::atomic<unsigned> next{0};
    directory_ =
        std::filesystem::temp_directory_path() /
        ("mw-tls-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++next));
    Check(std::filesystem::create_directory(directory_),
          "TLS fixture directory creation failed");
    try {
      Generate(subject_alt_names);
    } catch (...) {
      std::filesystem::remove_all(directory_);
      throw;
    }
  }
  ~CertFixture() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }
  CertFixture(const CertFixture&) = delete;
  CertFixture& operator=(const CertFixture&) = delete;
  std::string combined_file() const { return File("combined.pem"); }
  std::string trusted_file() const { return File("trust.pem"); }
  std::string encrypted_file() const { return File("encrypted.pem"); }
  std::string pkcs12_file() const { return File("combined.p12"); }

 private:
  std::string File(const char* filename) const {
    return (directory_ / filename).generic_u8string();
  }
  void Generate(const std::string& subject_alt_names) {
    const auto key_context =
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    Check(bool(key_context), "RSA context creation failed");
    Check(EVP_PKEY_keygen_init(key_context.get()) == 1 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) == 1,
          "RSA key setup failed");
    EVP_PKEY* raw_key = nullptr;
    Check(EVP_PKEY_keygen(key_context.get(), &raw_key) == 1,
          "RSA key generation failed");
    const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
        raw_key, EVP_PKEY_free);
    const auto certificate =
        std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), X509_free);
    Check(bool(certificate), "X509 creation failed");
    Check(X509_set_version(certificate.get(), 2) == 1 &&
              ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) ==
                  1 &&
              X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) &&
              X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) &&
              X509_set_pubkey(certificate.get(), key.get()) == 1,
          "X509 setup failed");
    const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(
        X509_NAME_new(), X509_NAME_free);
    Check(subject &&
              X509_NAME_add_entry_by_txt(
                  subject.get(), "CN", MBSTRING_ASC,
                  reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
                  0) == 1 &&
              X509_set_subject_name(certificate.get(), subject.get()) == 1 &&
              X509_set_issuer_name(certificate.get(), subject.get()) == 1,
          "X509 subject setup failed");
    X509V3_CTX extensions;
    X509V3_set_ctx(&extensions, certificate.get(), certificate.get(), nullptr,
                   nullptr, 0);
    auto extension_value = subject_alt_names;
    const auto san =
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
            X509V3_EXT_conf_nid(nullptr, &extensions, NID_subject_alt_name,
                                extension_value.data()),
            X509_EXTENSION_free);
    Check(san && X509_add_ext(certificate.get(), san.get(), -1) == 1 &&
              X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0,
          "X509 signing failed");
    WritePem(combined_file(), key.get(), certificate.get(), false);
    WritePem(encrypted_file(), key.get(), certificate.get(), true);
    WritePem(trusted_file(), nullptr, certificate.get(), false);
    const auto archive = std::unique_ptr<PKCS12, decltype(&PKCS12_free)>(
        PKCS12_create(kPassword, "mw-test", key.get(), certificate.get(),
                      nullptr, 0, 0, 0, 0, 0),
        PKCS12_free);
    const auto output = std::unique_ptr<BIO, decltype(&BIO_free)>(
        BIO_new_file(pkcs12_file().c_str(), "wb"), BIO_free);
    Check(archive && output && i2d_PKCS12_bio(output.get(), archive.get()) == 1,
          "PKCS12 fixture writing failed");
  }
  static void WritePem(const std::string& path, EVP_PKEY* key,
                       X509* certificate, bool encrypted) {
    const auto output = std::unique_ptr<BIO, decltype(&BIO_free)>(
        BIO_new_file(path.c_str(), "wb"), BIO_free);
    Check(bool(output), "PEM fixture file creation failed");
    if (key)
      Check(PEM_write_bio_PrivateKey(
                output.get(), key, encrypted ? EVP_aes_256_cbc() : nullptr,
                nullptr, 0, nullptr,
                encrypted ? const_cast<char*>(kPassword) : nullptr) == 1,
            "PEM private key writing failed");
    Check(PEM_write_bio_X509(output.get(), certificate) == 1,
          "PEM certificate writing failed");
  }

  std::filesystem::path directory_;
};

class Socket final {
 public:
#if defined(_WIN32)
  using Handle = SOCKET;
  static constexpr Handle kInvalid = INVALID_SOCKET;
#else
  using Handle = int;
  static constexpr Handle kInvalid = -1;
#endif
  Socket() {
#if defined(_WIN32)
    WSADATA data;
    Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
#endif
    handle_ = socket(AF_INET, SOCK_STREAM, 0);
    if (handle_ == kInvalid) {
#if defined(_WIN32)
      WSACleanup();
#endif
      throw std::runtime_error("TLS test socket creation failed");
    }
  }
  ~Socket() {
#if defined(_WIN32)
    closesocket(handle_);
    WSACleanup();
#else
    close(handle_);
#endif
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Handle get() const { return handle_; }
  void Nonblocking() {
#if defined(_WIN32)
    u_long enabled = 1;
    Check(ioctlsocket(handle_, FIONBIO, &enabled) == 0,
          "Nonblocking socket setup failed");
#else
    Check(fcntl(handle_, F_SETFL, O_NONBLOCK) == 0,
          "Nonblocking socket setup failed");
#endif
  }

 private:
  Handle handle_ = kInvalid;
};

inline std::uint16_t ReservePort() {
  Socket socket;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Check(bind(socket.get(), reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == 0,
        "TLS test port reservation failed");
#if defined(_WIN32)
  int size = sizeof(address);
#else
  socklen_t size = sizeof(address);
#endif
  Check(getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address),
                    &size) == 0,
        "TLS test port query failed");
  return ntohs(address.sin_port);
}

inline void Wait(Socket::Handle socket, bool writing,
                 std::chrono::steady_clock::time_point deadline) {
  const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
  Check(remaining > 0, "TLS HTTP request exceeded its deadline");
  timeval timeout{static_cast<long>(remaining / 1000000),
                  static_cast<long>(remaining % 1000000)};
  fd_set descriptors;
  FD_ZERO(&descriptors);
  FD_SET(socket, &descriptors);
  Check(select(static_cast<int>(socket) + 1, writing ? nullptr : &descriptors,
               writing ? &descriptors : nullptr, nullptr, &timeout) == 1,
        "TLS socket wait failed or timed out");
}

struct Response final {
  int status = 0;
  std::string body;
};

// Uses a separate OpenSSL context, verifies the peer chain and requested name,
// and never changes the SDK's process-wide certificate or trust state.
inline Response VerifiedHttpGet(const std::string& url,
                                const std::string& trusted_file,
                                const std::string& expected_name = {}) {
  const bool tls = url.find("https://") == 0;
  Check(tls || url.find("http://") == 0, "Unsupported test HTTP URL");
  const auto authority_start = tls ? 8U : 7U;
  const auto path_start = url.find('/', authority_start);
  const auto authority =
      url.substr(authority_start, path_start - authority_start);
  const auto colon = authority.rfind(':');
  Check(colon != std::string::npos, "Test HTTP URL needs an explicit port");
  const auto host = authority.substr(0, colon);
  const auto port = std::stoi(authority.substr(colon + 1));
  Check(host == "127.0.0.1" || host == "localhost",
        "TLS test client requires a loopback URL");
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  Socket socket;
  socket.Nonblocking();
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  if (connect(socket.get(), reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    Wait(socket.get(), true, deadline);
    int error = 0;
#if defined(_WIN32)
    int size = sizeof(error);
#else
    socklen_t size = sizeof(error);
#endif
    Check(getsockopt(socket.get(), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&error), &size) == 0 &&
              error == 0,
          "TLS test TCP connection failed");
  }
  auto context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(
      tls ? SSL_CTX_new(TLS_client_method()) : nullptr, SSL_CTX_free);
  auto connection =
      std::unique_ptr<SSL, decltype(&SSL_free)>(nullptr, SSL_free);
  if (tls) {
    Check(bool(context), "TLS client context creation failed");
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    Check(SSL_CTX_load_verify_locations(context.get(), trusted_file.c_str(),
                                        nullptr) == 1,
          "TLS fixture trust loading failed");
    connection.reset(SSL_new(context.get()));
    Check(bool(connection), "TLS client connection creation failed");
    Check(SSL_set_fd(connection.get(), static_cast<int>(socket.get())) == 1,
          "TLS client socket binding failed");
    const auto name = expected_name.empty() ? host : expected_name;
    in_addr numeric{};
    if (inet_pton(AF_INET, name.c_str(), &numeric) == 1) {
      Check(X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(connection.get()),
                                          name.c_str()) == 1,
            "TLS IP verification setup failed");
    } else {
      Check(X509_VERIFY_PARAM_set1_host(SSL_get0_param(connection.get()),
                                        name.c_str(), 0) == 1 &&
                SSL_set_tlsext_host_name(connection.get(), name.c_str()) == 1,
            "TLS hostname verification setup failed");
    }
    for (;;) {
      ERR_clear_error();
      const auto result = SSL_connect(connection.get());
      if (result == 1) break;
      const auto error = SSL_get_error(connection.get(), result);
      Check(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
            "TLS handshake or certificate verification failed");
      Wait(socket.get(), error == SSL_ERROR_WANT_WRITE, deadline);
    }
    Check(SSL_get_verify_result(connection.get()) == X509_V_OK,
          "TLS certificate verification failed");
  }
  const auto path =
      path_start == std::string::npos ? "/" : url.substr(path_start);
  const auto request = "GET " + path + " HTTP/1.1\r\nHost: " + authority +
                       "\r\nConnection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    ERR_clear_error();
    const auto size = static_cast<int>(request.size() - sent);
    const auto result =
        tls ? SSL_write(connection.get(), request.data() + sent, size)
            : send(socket.get(), request.data() + sent, size, 0);
    if (result > 0) {
      sent += result;
    } else {
      const auto error =
          tls ? SSL_get_error(connection.get(), result) : SSL_ERROR_WANT_WRITE;
      Check(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
            "TLS HTTP request write failed");
      Wait(socket.get(), error == SSL_ERROR_WANT_WRITE, deadline);
    }
  }
  std::string response;
  for (;;) {
    ERR_clear_error();
    char buffer[16384];
    const auto result = tls ? SSL_read(connection.get(), buffer, sizeof(buffer))
                            : recv(socket.get(), buffer, sizeof(buffer), 0);
    if (result > 0) {
      response.append(buffer, result);
      const auto headers_end = response.find("\r\n\r\n");
      const auto content_length = response.find("Content-Length:");
      if (headers_end != std::string::npos && content_length < headers_end) {
        const auto length = std::stoull(response.substr(content_length + 15));
        if (response.size() >= headers_end + 4 + length) break;
      }
    } else if (!tls && result == 0) {
      break;
    } else {
      const auto error =
          tls ? SSL_get_error(connection.get(), result) : SSL_ERROR_WANT_READ;
      if (error == SSL_ERROR_ZERO_RETURN) break;
      Check(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
            "TLS HTTP response read failed");
      Wait(socket.get(), error == SSL_ERROR_WANT_WRITE, deadline);
    }
  }
  const auto headers_end = response.find("\r\n\r\n");
  Check(response.find("HTTP/1.") == 0 && headers_end != std::string::npos,
        "Malformed test HTTP response");
  return {std::stoi(response.substr(response.find(' ') + 1)),
          response.substr(headers_end + 4)};
}

}  // namespace tls_test

#endif  // MW_STREAMER_TESTS_REMUXER_TLS_TEST_SUPPORT_H_
