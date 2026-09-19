/**
 * @file hpu_signal.h
 * @brief Platform contract: SIGHUP registration for config hot reload.
 *
 * The handler only sets an atomic flag (async-signal-safe); the watcher
 * thread polls the flag and performs the actual reload (spec 10.3).
 */

#ifndef HPU_SIGNAL_H
#define HPU_SIGNAL_H

/**
 * @brief Install the library's SIGHUP handler.
 *
 * Fails (returns -1) when the process already has a SIGHUP handler other
 * than SIG_DFL, so applications keep ownership of their signals. The
 * handler only raises an internal atomic flag.
 *
 * @return 0 on success, -1 when registration failed or the platform has no
 *         SIGHUP concept (Windows: always -1).
 */
int hpu_signal_install_hup(void);

/**
 * @brief Atomically fetch-and-clear the pending SIGHUP flag.
 * @return Non-zero when a SIGHUP arrived since the previous call.
 */
int hpu_signal_take_hup(void);

#endif /* HPU_SIGNAL_H */
