# nlohmann/json (vendored)

`json.hpp` v3.11.3, the single-header amalgamation from
https://github.com/nlohmann/json — MIT. Unmodified. To update, overwrite the
file; there is nothing else here.

## Why it is here

`photoncore` reads and writes three JSON files that a user's install depends on:

- **`photon_manifest.json`** — the install manifest in `Photon Backup Files/`.
  ⚠ Its **schema does not change** in the C++ port, and reads stay TOLERANT
  exactly as the Python `read_manifest` is: missing or corrupt is treated as
  *absent*, never as an error. Manifests written by every prior Python installer
  version have to keep working, because the manifest is what an uninstall reads
  to know what to put back.
- **`installer_config.json`** — the remembered X-Plane root.
- **`realwings_patch.json`** — the RealWings replacement parameters, resolved
  from the DSL at bundle time (see `installer/realwings.py`).

Hand-rolling a parser for a file an uninstall depends on is false economy
(`docs/installer_cpp_plan.md` §10 decision 4). The repo already vendors imgui and
stb this way, so this is the established shape rather than a new dependency
style.

## How it is compiled

`photoncore` includes it as `third_party/nlohmann/json.hpp`. Two things are set
where it is used rather than here, so a version bump stays a straight file copy:

- **Exceptions are ON** and `photoncore` catches `nlohmann::json::exception` at
  every read boundary. The alternative — `JSON_NOEXCEPTION` plus discarded-value
  checks at every access — is more code and one forgotten check away from reading
  a default where the file said something else.
- It is **not** included by `plugin.cpp`. The plugin links `photoncore` but does
  not parse JSON itself; keeping the header out of the `.xpl`'s translation units
  keeps its compile time where it was.
