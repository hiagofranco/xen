# SPDX-License-Identifier: GPL-2.0-only
"""Unit tests for the temp_dir scratch-directory singleton."""

from __future__ import annotations

import paths


def test_temp_dir_exists_and_prefixed():
    d = paths.temp_dir()
    assert d.is_dir()
    assert d.name.startswith("qtb-")


def test_temp_dir_is_singleton():
    assert paths.temp_dir() == paths.temp_dir()


def test_temp_dir_handle_kept_alive():
    h = paths._temp_dir_handle()
    assert h is paths._temp_dir_handle()
    assert h.name == str(paths.temp_dir())


def test_cleanup_temp_dir_removes_and_resets():
    d = paths.temp_dir()
    assert d.is_dir()
    paths.cleanup_temp_dir()
    assert not d.exists()
    # Cache reset: next call builds a fresh, existing dir, not the gone one.
    fresh = paths.temp_dir()
    assert fresh.is_dir()
    assert fresh != d
