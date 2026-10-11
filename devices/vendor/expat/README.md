# Expat 2.8.5

Unmodified `lib/*.c`, `lib/*.h` and `COPYING` from the MIT-licensed upstream
release, privately statically linked by `devices/cmake/expat`. The application
does not install Expat headers or expose its symbols as SDK API.

- Source: https://github.com/libexpat/libexpat/releases/tag/R_2_8_5
- Archive: `expat-2.8.5.tar.gz`
- SHA-256: `920dde485e15eda0cce8d2310b41d492c534e5e3d89ad407a0b4176dd2ff88fe`

Only the parser and platform entropy source are compiled. DTDs are rejected by
the caller before processing; external entities are never resolved. XML input
and nesting are bounded independently of Expat.
