#pragma once

// libmpv is loaded at runtime (QLibrary) rather than linked, so the app builds
// with any toolchain from just the libmpv headers, and the libmpv DLL/dylib can
// be swapped for a newer build without recompiling.

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <QString>

#define SNP_MPV_FUNCTIONS(X)                                                   \
    X(mpv_create)                                                              \
    X(mpv_initialize)                                                          \
    X(mpv_terminate_destroy)                                                   \
    X(mpv_set_option_string)                                                   \
    X(mpv_set_property)                                                        \
    X(mpv_set_property_string)                                                 \
    X(mpv_command)                                                             \
    X(mpv_observe_property)                                                    \
    X(mpv_wait_event)                                                          \
    X(mpv_set_wakeup_callback)                                                 \
    X(mpv_error_string)                                                        \
    X(mpv_request_log_messages)                                                \
    X(mpv_render_context_create)                                               \
    X(mpv_render_context_set_update_callback)                                  \
    X(mpv_render_context_update)                                               \
    X(mpv_render_context_render)                                               \
    X(mpv_render_context_report_swap)                                          \
    X(mpv_render_context_free)

namespace Mpv {

struct Api {
#define SNP_DECLARE(name) decltype(&::name) name = nullptr;
    SNP_MPV_FUNCTIONS(SNP_DECLARE)
#undef SNP_DECLARE
};

// Loads libmpv once. Returns nullptr (and fills *error) if it can't be found.
const Api *api(QString *error = nullptr);

} // namespace Mpv
