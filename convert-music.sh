#!/usr/bin/env bash
#
# convert-music.sh -- turn a folder of Quake CD-track .ogg files into the
# raw 16-bit PCM .wav files otQuake's cd_wav.c streams as background music.
#
# otQuake has no on-device decoder (the Zaurus's PXA250 has no FPU), so
# music/trackNN.wav files must already be plain PCM at the exact
# rate/channels the sound mixer is running -- this script does that
# conversion with ffmpeg, ahead of time, on your build machine.
#
# Usage:
#   ./convert-music.sh <src-dir> <dst-dir> [-r RATE] [-c 1|2] [--mono|--stereo]
#
# Example (after unzipping a Quake install somewhere, e.g. id1/music/ or
# a mod's own music dir, both full of trackNN.ogg files):
#   ./convert-music.sh /tmp/quake/id1/music id1/music
#
# The defaults (11025 Hz, stereo) match what quake-fb negotiates with
# /dev/dsp out of the box (see tryrates[] in src/snd_sun.c). If you launch
# quake-fb with -sndspeed/-sndmono/-sndstereo, pass matching -r/-c here so
# the converted tracks actually match what the mixer is running --
# otherwise otQuake will refuse to play them rather than play them back at
# the wrong pitch or garbled.

set -euo pipefail

RATE=11025
CHANNELS=2

usage() {
	cat <<EOF
Usage: $(basename "$0") <src-dir> <dst-dir> [options]

Converts every *.ogg in <src-dir> into a 16-bit PCM *.wav of the same name
in <dst-dir>, suitable for otQuake's music/trackNN.wav CD-audio emulation.

Options:
  -r, --rate HZ      Output sample rate (default: $RATE)
  -c, --channels N    Output channel count, 1 or 2 (default: $CHANNELS)
      --mono          Shorthand for --channels 1
      --stereo        Shorthand for --channels 2
  -h, --help          Show this help
EOF
}

SRC=""
DST=""

while [ $# -gt 0 ]; do
	case "$1" in
		-r|--rate)
			RATE="$2"; shift 2 ;;
		-c|--channels)
			CHANNELS="$2"; shift 2 ;;
		--mono)
			CHANNELS=1; shift ;;
		--stereo)
			CHANNELS=2; shift ;;
		-h|--help)
			usage; exit 0 ;;
		-*)
			echo "Unknown option: $1" >&2
			usage >&2
			exit 1 ;;
		*)
			if [ -z "$SRC" ]; then
				SRC="$1"
			elif [ -z "$DST" ]; then
				DST="$1"
			else
				echo "Unexpected argument: $1" >&2
				usage >&2
				exit 1
			fi
			shift ;;
	esac
done

if [ -z "$SRC" ] || [ -z "$DST" ]; then
	usage >&2
	exit 1
fi

if [ "$CHANNELS" != 1 ] && [ "$CHANNELS" != 2 ]; then
	echo "error: --channels must be 1 or 2 (got: $CHANNELS)" >&2
	exit 1
fi

command -v ffmpeg >/dev/null 2>&1 || { echo "error: ffmpeg is required but not found in PATH" >&2; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "error: ffprobe is required but not found in PATH" >&2; exit 1; }

if [ ! -d "$SRC" ]; then
	echo "error: source directory not found: $SRC" >&2
	exit 1
fi

shopt -s nullglob
oggs=("$SRC"/*.ogg)
shopt -u nullglob

if [ ${#oggs[@]} -eq 0 ]; then
	echo "error: no .ogg files found in $SRC" >&2
	exit 1
fi

mkdir -p "$DST"

echo "Converting ${#oggs[@]} track(s) to ${RATE}Hz / ${CHANNELS}ch / 16-bit PCM..."

fail=0
for src_file in "${oggs[@]}"; do
	name="$(basename "$src_file" .ogg)"
	dst_file="$DST/$name.wav"

	echo "  $name.ogg -> $dst_file"
	if ! ffmpeg -nostdin -y -loglevel error -i "$src_file" \
		-ac "$CHANNELS" -ar "$RATE" -sample_fmt s16 -acodec pcm_s16le \
		"$dst_file"; then
		echo "  ffmpeg failed on $src_file" >&2
		fail=1
		continue
	fi

	# Sanity-check the file we just wrote against what cd_wav.c requires:
	# an exact rate/channel match, or otQuake will refuse to play it.
	info="$(ffprobe -v error -select_streams a:0 \
		-show_entries stream=codec_name,sample_rate,channels \
		-of csv=p=0 "$dst_file")"
	got_codec="${info%%,*}"
	rest="${info#*,}"
	got_rate="${rest%%,*}"
	got_channels="${rest#*,}"

	if [ "$got_codec" != "pcm_s16le" ] || [ "$got_rate" != "$RATE" ] || [ "$got_channels" != "$CHANNELS" ]; then
		echo "  warning: $dst_file came out as $got_codec ${got_rate}Hz ${got_channels}ch," \
			"expected pcm_s16le ${RATE}Hz ${CHANNELS}ch" >&2
		fail=1
	fi
done

if [ "$fail" -ne 0 ]; then
	echo "Done, with errors -- see above." >&2
	exit 1
fi

echo "Done. Drop $DST next to your game data as .../music/ (e.g. id1/music/) so otQuake can find it."
