#pragma once

#include <windows.h>
#include <unknwn.h>

namespace sr {

// AddRef returns the post-increment count. Callers pass the references owned
// by the pool before this probe; the probe itself contributes one more ref.
inline bool has_external_com_references(IUnknown* object,
                                        ULONG owned_references) noexcept {
    if (!object) return false;
    const ULONG references_after_probe = object->AddRef();
    object->Release();
    return references_after_probe > owned_references + 1;
}

} // namespace sr
