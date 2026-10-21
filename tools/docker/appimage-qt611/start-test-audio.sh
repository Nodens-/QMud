#!/usr/bin/env bash

# QMud Project
# Copyright (c) 2026 Panagiotis Kalogiratos (Nodens)
#
# File: start-test-audio.sh
# Role: Starts the AppImage test image's private PulseAudio null output.

set -euo pipefail

if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
	echo "XDG_RUNTIME_DIR must identify a writable private runtime directory." >&2
	exit 1
fi

mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"

pulseaudio --daemonize=yes --exit-idle-time=-1
pactl load-module module-null-sink sink_name=qmud_ci >/dev/null
pactl set-default-sink qmud_ci
