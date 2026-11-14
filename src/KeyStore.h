#pragma once

#include <QString>

// Where Argonaut keeps the Anthropic API key (core code, no widgets).
//
//  1. ANTHROPIC_API_KEY in the environment wins, and is never stored.
//  2. Otherwise the desktop's keyring through the freedesktop Secret
//     Service over D-Bus (GNOME Keyring, KWallet), the same place browsers
//     and Nautilus keep passwords. No extra library needed.
//  3. Without a keyring (Windows, macOS, a bare session): a file only you
//     can read (0600) in Argonaut's config folder, and the UI says so.
// ARGONAUT_KEYSTORE=file forces 3 (tests).
namespace KeyStore {

enum class Where { None, Environment, Keyring, File };

QString load(Where *where = nullptr);
// Saves the key; false (with `error`) if it couldn't be stored.
bool save(const QString &key, Where *where = nullptr, QString *error = nullptr);
bool remove(QString *error = nullptr);

// "GNOME Keyring" / "a private file" / "ANTHROPIC_API_KEY", for the UI.
QString describe(Where where);

} // namespace KeyStore
