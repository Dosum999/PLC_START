#include "util/password.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <charconv>
#include <format>
#include <stdexcept>
#include <vector>

namespace logict::core {

namespace {

constexpr std::string_view kScheme = "pbkdf2-sha256";
constexpr int kSaltLen = 16;
constexpr int kHashLen = 32;

std::string b64(const unsigned char* data, int len) {
  std::string out(static_cast<std::size_t>(4 * ((len + 2) / 3)), '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, len);
  out.resize(static_cast<std::size_t>(n));
  return out;
}

std::vector<unsigned char> unb64(std::string_view s) {
  if (s.empty() || s.size() % 4 != 0) return {};
  std::vector<unsigned char> out(3 * s.size() / 4);
  int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(s.data()), static_cast<int>(s.size()));
  if (n < 0) return {};
  std::size_t pad = (s.end()[-1] == '=') + (s.end()[-2] == '=');
  out.resize(static_cast<std::size_t>(n) - pad);
  return out;
}

bool derive(std::string_view password, const unsigned char* salt, int salt_len, int iterations, unsigned char* out,
            int out_len) {
  return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, salt_len, iterations,
                           EVP_sha256(), out_len, out) == 1;
}

}  // namespace

std::string hash_password(std::string_view password, int iterations) {
  unsigned char salt[kSaltLen];
  unsigned char hash[kHashLen];
  if (RAND_bytes(salt, kSaltLen) != 1 || !derive(password, salt, kSaltLen, iterations, hash, kHashLen))
    throw std::runtime_error("password hashing failed");
  return std::format("{}${}${}${}", kScheme, iterations, b64(salt, kSaltLen), b64(hash, kHashLen));
}

bool verify_password(std::string_view password, std::string_view encoded) {
  std::vector<std::string_view> parts;
  for (std::size_t start = 0;;) {
    auto pos = encoded.find('$', start);
    parts.push_back(encoded.substr(start, pos - start));
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  if (parts.size() != 4 || parts[0] != kScheme) return false;
  int iterations = 0;
  auto r = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
  if (r.ec != std::errc{} || iterations < 1000) return false;
  auto salt = unb64(parts[2]);
  auto expected = unb64(parts[3]);
  if (salt.empty() || expected.size() != kHashLen) return false;
  unsigned char hash[kHashLen];
  if (!derive(password, salt.data(), static_cast<int>(salt.size()), iterations, hash, kHashLen)) return false;
  return CRYPTO_memcmp(hash, expected.data(), kHashLen) == 0;
}

}  // namespace logict::core
