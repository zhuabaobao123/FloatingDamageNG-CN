# FloatingDamageNG API

LGPL-3.0-or-later client API for other SKSE plugins. Vendor
`FloatingDamageNGAPI.h` and `FloatingDamageNGAPI.cpp` (the stub must compile
inside your plugin so the SKSE messaging types share your CommonLib) plus
`COPYING.LESSER`. FloatingDamageNG itself stays GPL-3.0-or-later with its modding
exception; only this directory is LGPL, so linking these two files into your
plugin does not require your plugin to be GPL (modifications to the files
themselves must stay LGPL). See the repository README's License section.

## Use

```cpp
#include "FloatingDamageNGAPI.h"
namespace FDNG = FloatingDamageNGPluginAPI;

FDNG::IFloatingDamageNGInterface001* g_fdng = nullptr;

// kPostPostLoad (retry later if null: the host may not be ready yet)
g_fdng = FDNG::GetFloatingDamageNGInterface001();

// On a locational hit, replace FloatingDamageNG's own number with yours:
if (g_fdng) {
    g_fdng->ClaimHit(victim->GetFormID(), FDNG::kClaim_SuppressPopup);

    FDNG::PopupRequest req;
    req.text = "HEADSHOT 142";
    req.x = hit.x; req.y = hit.y; req.z = hit.z;  // world space
    req.victimFormID = victim->GetFormID();
    req.style = FDNG::PopupStyle::kCritical;
    if (!g_fdng->SpawnPopup(&req)) {
        // floating numbers are off, or the request was invalid
    }
}
```

## Contract

- **Projection is ours.** Pass world coordinates; FloatingDamageNG places the popup
  in VR (world quad) or on the flat HUD, using the user's font, motion preset,
  size, and distance settings. The popup stays at the given point for its lifetime.
- **`ClaimHit`** applies to the next weapon hit on that victim, once. Call it from
  the code that decides the hit's outcome, within about 500 ms of the hit, in
  either order relative to FloatingDamageNG seeing the hit. An unconsumed claim
  expires and never touches a later hit.
  - `kClaim_SuppressPopup` withholds the number; combat analytics still records
    the hit.
  - `kClaim_SuppressLocation` keeps the number but drops the HEADSHOT-style tag
    and the implied-multiplier subtext.
- **`SpawnPopup` returns `false`** when the user disabled floating numbers or the
  request is invalid; fall back to your own rendering if you want.
- Both calls are safe from any thread.
- Compatibility: the interface is versioned (`Interface001`, later revisions
  extend by inheritance) and `PopupRequest::size` lets later revisions append
  fields without breaking older clients.
