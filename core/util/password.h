#pragma once

#include <string>
#include <string_view>

namespace logict::core {

// Password hashes are stored as "pbkdf2-sha256$<iterations>$<salt-b64>$<hash-b64>".
std::string hash_password(std::string_view password, int iterations = 200000);
bool verify_password(std::string_view password, std::string_view encoded);

}  // namespace logict::core
