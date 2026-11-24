#pragma once

#include <string>

namespace xst::bcrypt {

// bcrypt ($2a$/$2b$/$2y$), as golang.org/x/crypto/bcrypt, which Syncthing uses for the GUI password.
// Returns "" if salt isn't a valid bcrypt setting/hash prefix ("$2a$10$" + 22 salt characters).
std::string hash(const std::string &password, const std::string &salt_setting);
// A new hash at the given cost (Syncthing uses 10), with a random salt.
std::string generate(const std::string &password, int cost = 10);
// Constant-time check of password against a stored hash.
bool verify(const std::string &password, const std::string &stored);
bool looks_like_hash(const std::string &s);

} // namespace xst::bcrypt
