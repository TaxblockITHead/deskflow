/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

// Certificate fingerprint helpers that don't need the OpenSSL headers,
// so code outside the net library can use them.

#include "Fingerprint.h"

#include <QString>

namespace deskflow {

/**
 * @brief SHA256 fingerprint of the certificate in a PEM file
 * @param path PEM file, may also contain the private key
 * @return the fingerprint, invalid if the file has no readable certificate
 */
Fingerprint certFileFingerprint(const QString &path);

/**
 * @brief Remember the server this client connected to, so it can be found again
 * when its address changes. Writes Settings::tlsLastServerDb() when it changed.
 */
void rememberServerFingerprint(const Fingerprint &fingerprint);

} // namespace deskflow
