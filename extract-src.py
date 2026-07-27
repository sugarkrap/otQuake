#!/usr/bin/env python3
"""Extract head revisions from the CVS/RCS vault into src/.

Usage:  python3 extract-src.py [VAULT_DIR] [OUT_DIR]
Defaults: VAULT_DIR = handheldquake/   OUT_DIR = src/
"""

import os, re, sys

VAULT = sys.argv[1] if len(sys.argv) > 1 else "handheldquake"
OUT   = sys.argv[2] if len(sys.argv) > 2 else "src"

# Files from the SOURCES list in Makefile.in plus all .h headers.
# vid_qt.cpp is intentionally excluded; vid_fb.c is the replacement.
WANTED = {
    "FixedPointMath.c", "FixedPointMath.h",
    "console.c",  "d_sky.c",    "menu.c",      "pr_edict.c", "r_misc.c",
    "snd_mix.c",  "wad.c",      "cd_null.c",   "crc.c",      "d_sprite.c",
    "model.c",    "pr_exec.c",  "r_part.c",    "snd_sun.c",  "world.c",
    "chase.c",    "cvar.c",     "d_surf.c",    "net_bsd.c",  "r_aclip.c",
    "r_sky.c",    "sv_main.c",  "zone.c",      "cl_demo.c",  "d_edge.c",
    "d_vars.c",   "net_dgrm.c", "r_alias.c",   "r_sprite.c", "sv_move.c",
    "cl_input.c", "d_fill.c",   "d_zpoint.c",  "net_loop.c", "r_bsp.c",
    "r_surf.c",   "sv_phys.c",  "cl_main.c",   "d_init.c",   "draw.c",
    "net_main.c", "r_draw.c",   "r_vars.c",    "sv_user.c",  "cl_parse.c",
    "d_modech.c", "host.c",     "net_udp.c",   "r_edge.c",   "sbar.c",
    "sys_linux.c","cl_tent.c",  "d_part.c",    "host_cmd.c", "net_vcr.c",
    "r_efrag.c",  "screen.c",   "vid.c",       "cmd.c",      "d_polyse.c",
    "keys.c",     "nonintel.c", "r_light.c",   "snd_dma.c",  "common.c",
    "d_scan.c",   "mathlib.c",  "pr_cmds.c",   "r_main.c",   "snd_mem.c",
    "view.c",
    # headers
    "adivtab.h",  "anorm_dots.h","anorms.h",   "bspfile.h",  "cdaudio.h",
    "client.h",   "cmd.h",       "common.h",   "conproc.h",  "console.h",
    "crc.h",      "cvar.h",      "d_iface.h",  "d_local.h",  "draw.h",
    "input.h",    "keys.h",      "mathlib.h",  "menu.h",     "model.h",
    "modelgen.h", "net.h",       "net_dgrm.h", "net_loop.h", "net_udp.h",
    "net_vcr.h",  "progs.h",     "progdefs.h", "pr_comp.h",  "protocol.h",
    "quakedef.h", "render.h",    "r_local.h",  "r_shared.h", "sbar.h",
    "screen.h",   "server.h",    "sound.h",    "spritegn.h", "sys.h",
    "vid.h",      "view.h",      "wad.h",      "winquake.h", "world.h",
    "zone.h",
    # data headers included by progdefs.h
    "progdefs.q1", "progdefs.q2",
}


def extract_head(path):
    """Parse RCS/CVS ,v file and return the text of the head revision.

    Handles the @-string encoding correctly: @@ inside the body is a
    literal @; a lone @ terminates the block.
    """
    with open(path, "r", errors="replace") as f:
        content = f.read()

    m = re.search(r"^head\s+(\S+);", content, re.MULTILINE)
    if not m:
        return None
    head = m.group(1)

    # Locate:  <rev>\nlog\n@<log>@\ntext\n@<body>@
    search = head + "\nlog\n@"
    pos = content.find(search)
    if pos < 0:
        return None
    pos += len(search)

    # Skip log @-string
    while True:
        at = content.find("@", pos)
        if at < 0:
            return None
        if at + 1 < len(content) and content[at + 1] == "@":
            pos = at + 2
        else:
            pos = at + 1
            break

    tag = "\ntext\n@"
    if content[pos : pos + len(tag)] != tag:
        return None
    pos += len(tag)

    # Decode body @-string
    body = []
    while True:
        at = content.find("@", pos)
        if at < 0:
            return None
        body.append(content[pos:at])
        if at + 1 < len(content) and content[at + 1] == "@":
            body.append("@")
            pos = at + 2
        else:
            break

    return "".join(body)


os.makedirs(OUT, exist_ok=True)
extracted, skipped = 0, 0

for fname in sorted(os.listdir(VAULT)):
    if not fname.endswith(",v"):
        continue
    base = fname[:-2]
    if base not in WANTED:
        skipped += 1
        continue
    text = extract_head(os.path.join(VAULT, fname))
    if text is None:
        print(f"WARN: could not parse {fname}")
        continue
    out_path = os.path.join(OUT, base)
    with open(out_path, "w") as f:
        f.write(text)
    print(f"  {base}")
    extracted += 1

print(f"\nExtracted {extracted} files → {OUT}/  (skipped {skipped} unwanted)")
