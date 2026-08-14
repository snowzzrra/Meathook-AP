#!/usr/bin/env bash
printf 'Meathook CMake: resource compiler invoked, but no llvm-rc was available and source inventory has no .rc files\n' >&2
exit 1
