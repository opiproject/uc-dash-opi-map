#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 The dash-opi-map Authors
#
# Compatibility wrapper. The lab controller is scripts/labctl.sh.
exec "$(cd "$(dirname "$0")" && pwd)/labctl.sh" "$@"
