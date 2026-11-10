# New code lives in its own folder, reached through two do-nothing hooks

This fork keeps pulling in updates from the original SumatraPDF, so every line changed in an original file is a future merge conflict. All new code therefore lives in **`src/skeuomorph/`**. Original files get only about **46 added lines and 1 changed line**, in 13 files, listed in the spec's hook table, and most of them are one-line calls into the new code.

The shared page code (`src/shared/`) is also compiled into "ng", the other SumatraPDF app in this repo. It reaches our code through **two new methods on SumatraPDF's existing `DocControllerCallback` that do nothing by default**. Classic SumatraPDF fills them in; ng never does, so ng behaves exactly as before and none of ng's own files change.

## Considered options

- **The repo's "each app implements `FooCommon.h`" pattern:** that would force code into ng.
- **A global function pointer:** adds a global and a startup line, and the callback already knows the window.
- **Changing the original drawing and layout code directly:** simpler to write, but painful on every upstream update.

## Consequences

- **Watch what the new code reads.** Many things are worked out from what the new code can see instead of being hooked: for example, a "fingerprint" of the view decides when not to animate. Changes upstream to those details can break them silently.
- **A line budget per build step.** Going over it means asking first.
- **Generated files** (`src/Settings.*`, `src/shared/Commands.*`) still conflict on merges. Fix them by re-running the generator, not by hand.

Source: [Where does the new code hook in, and how few original lines change?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/17).
