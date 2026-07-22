# FunKey OS Port

This directory contains the experimental native OPK port for FunKey OS
firmware, targeting the FunKey S and RG Nano with the FunKey SDK 2.3
toolchain and SDL 1.2.

## Build

```bash
FUNKEY_SDK_DIR=/path/to/FunKey-sdk-2.3.0 ./funkey/package-opk.sh
```

The package script builds the texture baker, cross-compiles ThumbyCraft, and
writes the OPK to `dist/funkey/ThumbyCraft.opk` by default. Set `OPK_OUT` to
override the output path.

## Install

Copy `dist/funkey/ThumbyCraft.opk` to the SD card folder your launcher scans
for OPKs, then launch **ThumbyCraft**.

Saves, thumbnails, logs, and edited chunk data are stored under
`/mnt/FunKey/.thumbycraft`. Set `THUMBYCRAFT_HOME` in the launcher environment
to override that directory.

## Controls

```text
D-pad       move / turn
A           jump
B           look modifier / use modifier
X or R      mine / attack
Y or L      place / use
START       menu
SELECT      cycle hotbar
POWER/Q     save and quit
```
