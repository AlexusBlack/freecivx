#!/bin/bash
#
# Build a native (non-emscripten) Freeciv server, plus the GTK3 client if the
# client sources are present.
#
# This is the quick-iteration build used for testing the map generator.
#
# NOTE ON GTK: this fork does not ship a client/ directory - it is a
# server-only tree (freeciv-web supplies its own JavaScript client), so
# 'client/luascript/tolua_client.pkg' and everything else meson needs for a GUI
# client is absent. The script therefore detects whether client sources exist:
#   * present -> builds freeciv-gtk3.22 as well
#   * absent  -> builds the server only, and says so
# For map-generator work the server alone is enough: it can render the
# generated map straight to a PNG via the built-in 'mapimg' command, with no
# client involved (see the instructions printed at the end).
#
# Deliberately different from prepare_freeciv.sh / prepare_freeciv_emscripten.sh:
#   * its own build and install directories, so it never clobbers the web build
#   * server=enabled, not freeciv-web
#   * buildtype=debugoptimized  -> defines FREECIV_DEBUG (assertions, log_debug,
#     and the '-d d' server log level) while still compiling -O2
#   * no LTO, so incremental relinks stay fast
#
# Usage:
#   ./prepare_freeciv_gtk.sh              # configure (if needed), build, install
#   ./prepare_freeciv_gtk.sh -r           # force meson reconfigure
#   ./prepare_freeciv_gtk.sh -c           # wipe the build dir first
#   ./prepare_freeciv_gtk.sh -j 4         # limit parallelism
#   ./prepare_freeciv_gtk.sh --no-install # build only, skip 'ninja install'

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd)"
cd "${DIR}"

SRC_DIR="${DIR}/freeciv"
BUILD_DIR="${DIR}/build-gtk"
INSTALL_DIR="${FREECIV_GTK_PREFIX:-${HOME}/freeciv-gtk}"
# Scratch dir for map screenshots + autosaves from the test loop below. Its
# contents are gitignored by mapimg/.gitignore ('*' with '!.gitignore').
MAPIMG_DIR="${DIR}/mapimg"
# The space tileset lives in its own repo, pulled in as a git submodule so its
# history stays separate from this one. It is never copied into the install
# tree; instead it is prepended to FREECIV_DATA_PATH so the client picks it up
# straight from the working copy and edits take effect without reinstalling.
TILESETS_DIR="${DIR}/tilesets"
NUM_CORES="$(nproc)"
RECONFIGURE=0
CLEAN=0
DO_INSTALL=1

while [ $# -gt 0 ]; do
  case "$1" in
    -r|--reconfigure) RECONFIGURE=1 ;;
    -c|--clean)       CLEAN=1 ;;
    -j|--jobs)        NUM_CORES="$2"; shift ;;
    --no-install)     DO_INSTALL=0 ;;
    -h|--help)        sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1 (try --help)" >&2; exit 1 ;;
  esac
  shift
done

# --- sanity checks -----------------------------------------------------------

if [ ! -f "${SRC_DIR}/meson.build" ]; then
  echo "ERROR: no meson.build in ${SRC_DIR}" >&2
  exit 1
fi

for tool in meson ninja pkg-config; do
  command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found" >&2; exit 1; }
done

# Can we build a GUI client at all? Two things must hold: the client sources
# must be in the tree, and GTK3 dev files must be installed.
BUILD_GTK=0
if [ -f "${SRC_DIR}/client/luascript/tolua_client.pkg" ]; then
  if pkg-config --exists gtk+-3.0; then
    BUILD_GTK=1
  else
    echo "NOTE: client sources found, but gtk+-3.0 development files are not."
    echo "      Building server only. To get the GTK client:"
    echo "        sudo apt install libgtk-3-dev   # Debian/Ubuntu"
    echo "      ...then re-run with -r."
  fi
else
  echo "NOTE: no client sources in ${SRC_DIR}/client - this is a server-only"
  echo "      tree, so no GTK client can be built. Building the server only."
  echo "      The server can still render generated maps to PNG via 'mapimg';"
  echo "      see the instructions printed when the build finishes."
fi
echo

# --- configure ---------------------------------------------------------------

if [ "${CLEAN}" -eq 1 ]; then
  rm -rf "${BUILD_DIR}"
fi

MESON_ARGS=(
  # A real server, not the freeciv-web variant: we need the stock map
  # generator entry points and the normal client/server protocol.
  -Dserver=enabled
  -Dfcmp=cli

  # MagickWand backs the server's 'mapimg' PNG output - the main way to
  # eyeball a generated map when there is no GUI client. 'try' degrades to
  # the built-in PPM/text writers if the library is missing.
  -Dmwand=try

  # debugoptimized => meson's debug=true, which meson.build turns into
  # FREECIV_DEBUG (see meson.build:89). That enables fc_assert, log_debug,
  # and the '-d d' server log level - all wanted when writing a generator.
  -Dbuildtype=debugoptimized

  # Trimmed to cut dependencies and build time; none affect map generation.
  -Djson-protocol=false
  -Dnls=false
  -Daudio=false
  -Druledit=false
  -Dgitrev=false

  # Static internal libs avoid rpath/LD_LIBRARY_PATH fiddling when running
  # the installed binaries. No LTO: it dominates incremental relink time.
  -Ddefault_library=static
  -Db_lto=false

  -Dprefix="${INSTALL_DIR}"
)

if [ "${BUILD_GTK}" -eq 1 ]; then
  MESON_ARGS+=( -Dclients=gtk3.22 )
else
  MESON_ARGS+=( -Dclients=[] )
fi

if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
  meson setup "${BUILD_DIR}" "${SRC_DIR}" "${MESON_ARGS[@]}"
elif [ "${RECONFIGURE}" -eq 1 ]; then
  meson setup --reconfigure "${BUILD_DIR}" "${SRC_DIR}" "${MESON_ARGS[@]}"
else
  echo "Using existing configuration in ${BUILD_DIR} (use -r to reconfigure)."
fi

# --- build -------------------------------------------------------------------

ninja -C "${BUILD_DIR}" -j "${NUM_CORES}"

if [ "${DO_INSTALL}" -eq 1 ]; then
  ninja -C "${BUILD_DIR}" install
fi

# Present even in a fresh clone (mapimg/.gitignore is tracked), but recreate it
# in case it was wiped by hand.
mkdir -p "${MAPIMG_DIR}"

# Make the submodule tilesets visible to a plain './freeciv-gtk3.22' launch.
#
# The client's search path is "." + "data" + ~/.local/share/freeciv + the
# compiled-in datadir, so a tileset sitting in tilesets/ is invisible unless
# FREECIV_DATA_PATH is exported for every single run. Symlinking into the
# install tree instead means the tileset shows up in the normal tileset menu
# with no environment set up at all.
#
# These are symlinks, not copies, on purpose: they resolve back to the
# submodule working copy, so editing a .spec takes effect on the next client
# start with no reinstall, and the tileset's git history stays entirely in its
# own repo. 'ninja install' does not remove them, but it also never creates
# them, so we (re)make them here on every install.
if [ "${DO_INSTALL}" -eq 1 ] && [ -d "${TILESETS_DIR}" ]; then
  for tsdir in "${TILESETS_DIR}"/*/; do
    [ -d "${tsdir}" ] || continue
    tsname="$(basename "${tsdir}")"
    # An unpopulated submodule is an empty dir - skip it rather than making
    # dangling links that would show a broken tileset in the menu.
    if [ ! -f "${tsdir}/${tsname}.tilespec" ]; then
      echo "note: tilesets/${tsname} has no ${tsname}.tilespec (submodule not"
      echo "      checked out? try 'git submodule update --init'); skipping."
      continue
    fi
    ln -sfn "${tsdir}/${tsname}.tilespec" \
            "${INSTALL_DIR}/share/freeciv/${tsname}.tilespec"
    ln -sfn "${tsdir}/${tsname}" \
            "${INSTALL_DIR}/share/freeciv/${tsname}"
    echo "linked tileset '${tsname}' into ${INSTALL_DIR}/share/freeciv"
  done
fi

# --- done --------------------------------------------------------------------

cat <<EOF

Build complete.
  Build dir  : ${BUILD_DIR}
  Install dir: ${INSTALL_DIR}
  Map images : ${MAPIMG_DIR} (gitignored)
  Tilesets   : ${TILESETS_DIR} $( [ -f "${TILESETS_DIR}/space/space.tilespec" ] && echo "(space: ok)" || echo "(space: MISSING - run 'git submodule update --init')" )
  GTK client : $( [ "${BUILD_GTK}" -eq 1 ] && echo "yes" || echo "no (server-only tree)" )

Quick map-generator test loop:

  # rebuild after editing server/generator/*.c
  ninja -C ${BUILD_DIR} && ninja -C ${BUILD_DIR} install

  # Generate a map with no GUI and dump it to a PNG. Verified working:
  {
    printf 'set minplayers 0\nset aifill 2\n'
    printf 'set generator FRACTAL\nset mapseed 12345\n'
    printf 'mapimg define show=all:zoom=3:format=magick|png\n'
    printf 'start\n'
    sleep 8
    printf 'mapimg create 0\n'
    sleep 4
    printf 'quit\n'
  } | ${INSTALL_DIR}/bin/freeciv-server -d v --saves ${MAPIMG_DIR}
  # -> writes ${MAPIMG_DIR}/freeciv.map.png

Four things that will bite you (all found the hard way):

  1. 'format=png' is silently WRONG - it falls back to GIF. The toolkit must
     be named explicitly: 'format=magick|png'. (common/mapimg.c:702 documents
     the syntax as 'format=<[tool|]format>'.)
  2. Commands placed after 'start' in a --read script are NEVER executed, so
     'mapimg create' has to arrive on stdin after the game is running - hence
     the printf/sleep pipeline above rather than a .serv file.
  3. 'set minplayers 0' is required, or an AI-only game refuses to start
     ("Not enough human players").
  4. mapimg writes into the --saves directory, not the cwd - which is why
     the loop above points it at ${MAPIMG_DIR}. That directory ignores
     its own contents, so screenshots and autosaves never reach git.

  '-d v' is needed: print_mapgen_map() logs terrain statistics at LOG_VERBOSE
  (server/generator/mapgen.c:1232). '-d d' additionally enables log_debug,
  which works because this build defines FREECIV_DEBUG.

  Known-broken generator: FRACTURE segfaults on any map without WRAP_X
  (server/generator/fracture_map.c:194 uses 'x > xsize' where it means
  '>=', so fmfill() derefs the NULL that native_pos_to_tile() returns for an
  out-of-range column). Use RANDOM/FRACTAL/ISLAND/FAIR, or 'set wrap WRAPX'.
EOF

if [ "${BUILD_GTK}" -eq 1 ]; then
  cat <<EOF

  # visual: launch the GTK client and let it start a local server
  ${INSTALL_DIR}/bin/freeciv-gtk3.22

  # visual, with the space tileset. No environment needed: the install step
  # symlinked tilesets/space into share/freeciv, so 'space' is in the tileset
  # menu and the links point back at the submodule working copy.
  ${INSTALL_DIR}/bin/freeciv-gtk3.22 --tiles space

  # headless check that the tileset still loads (exit 124 from timeout = it
  # stayed up = good; any '0:' line in the output is a fatal):
  timeout 30 xvfb-run -a ${INSTALL_DIR}/bin/freeciv-gtk3.22 --tiles space -- -d 2
EOF
else
  cat <<EOF

  No GTK client was built - ${SRC_DIR}/client does not exist in this fork.
  To get one you would need to import the client/ tree from upstream freeciv
  (see version.txt: FCREV pins the upstream commit this fork is based on).
EOF
fi
echo
