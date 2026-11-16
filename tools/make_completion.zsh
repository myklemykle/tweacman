# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Mykle Hansen
# Specified by Mykle Hansen, implemented by Claude (Anthropic).

# zsh tab completion for this project's Makefile: completes SKETCH= with
# every sketch in the repo - the top-level demos and each library's
# examples/ - instead of just the current directory's names.
#
# To use it, add this line to ~/.zshrc (after compinit), adjusting the path:
#
#   source /path/to/tweacman.git/tools/make_completion.zsh
#
# Then, from the repo's root:  make run SKETCH=<Tab>
#
# Anything other than SKETCH= falls through to zsh's normal make completion.

_tweacman_make() {
  if [[ $words[CURRENT] == SKETCH=* ]]; then
    # A sketch is a folder holding an .ino of the same name, as Arduino
    # requires; build/ holds copies of them, so it's skipped.
    local -a sketches
    local f
    for f in */*.ino(N) */examples/*/*.ino(N); do
      [[ $f == build/* ]] && continue
      [[ ${f:h:t} == ${f:t:r} ]] && sketches+=(${f:h})
    done
    if (( $#sketches )); then
      compset -P 'SKETCH='
      _wanted sketches expl sketch compadd -a sketches
      return
    fi
  fi
  _make "$@"
}

compdef _tweacman_make make
