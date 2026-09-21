#pragma once

/* Test-only control for fake_mcfg_store.c's mcfg_commit(): forces the
 * NVS/mutex-failure path (-2) regardless of whether the config would
 * otherwise validate, so a host test can exercise mcfg_ops_edit()'s -2 vs -3
 * split without a real NVS to fail. Off by default; a test that turns it on
 * must turn it back off before returning, since the flag is process-wide
 * static state shared with every other test in the same binary. */
void fake_mcfg_store_force_storage_fail(int on);
