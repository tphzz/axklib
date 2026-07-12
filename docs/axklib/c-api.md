# C API

The installed `axklib/c/axk.h` header exposes ABI version 1 for C and foreign
language hosts. Link the `axklib::c` CMake target or the `axklib` pkg-config
module. The shared library is named `axklib_c`.

The C ABI is the only shared axklib target. Its C++ engine and audio
dependencies are linked into it statically, so installing the C SDK does not
require separate `axk_core` or `axk_audio` runtime libraries.

```cmake
find_package(axklib CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE axklib::c)
```

All text arguments are UTF-8 `axk_string_view` values. They need not be
null-terminated. Returned views belong to the context or result handle named by
the function and remain valid until that owner is changed or destroyed.

## Versioned Structures

Every public input or output structure starts with `struct_size` and
`abi_version`. Initialize it before calling the library:

```c
axk_validation_summary summary;
AXK_INIT_STRUCT(summary);
```

Version 1 accepts a larger structure from a compatible ABI major version, which
allows fields to be appended later. A smaller structure returns
`AXK_STATUS_STRUCT_TOO_SMALL`; a different major version returns
`AXK_STATUS_UNSUPPORTED_ABI`.

## Ownership

Create functions write an opaque handle through a pointer-to-pointer. The output
must initially be null. Each handle has a matching close or destroy function.
Destroying a null handle, or passing a null pointer to a destroy function, is a
successful no-op. A handle must not be used after destruction.

Result handles own their nested strings and arrays. PCM bytes belong to
`axk_buffer`; content rows belong to `axk_node_result`; object rows belong to
`axk_object_result`; preview bins belong to `axk_preview_result`.

## Threads And Callbacks

An immutable snapshot can be read concurrently when each caller uses its own
context and result handles. Build plans and transactions belong to the thread
that created them. Using either from another thread returns
`AXK_STATUS_WRONG_THREAD`.

Set the progress callback before the first context operation. Context callback
configuration cannot change afterward. Callbacks run synchronously on the
calling operation's thread. They may request cancellation, but must not start an
export, image creation, or transaction mutation with the same context. Such a
call returns `AXK_STATUS_INVALID_ARGUMENT`. Callback code must not retain event
or label views after returning.

## Errors And Cancellation

Every function returns an `axk_status` or a neutral value for handle accessors.
For a failed context operation, `axk_context_last_error_info` returns the stable
status, native error code, user-facing message, and available object location.
These strings remain owned by the context until its next operation.

`axk_context_cancel` is thread-safe. Long operations observe the request and
return `AXK_STATUS_CANCELLED`; call `axk_context_reset_cancel` before reusing the
context.

## Examples

Plain-C examples are installed in the source distribution under
`cpp/examples/c`: `inventory.c`, `exact_export.c`, `create.c`, and `alter.c`.
They demonstrate deterministic cleanup for each maintained workflow.
