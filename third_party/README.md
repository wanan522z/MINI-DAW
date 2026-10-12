# Dependencies

Mini DAW uses JUCE 8.0.12 from https://github.com/juce-framework/JUCE, pinned to
commit `29396c22c93392d6738e021b83196283d6e4d850`.

The local `JUCE/` source directory is intentionally ignored by Git. If it is
absent, the root CMake configuration fetches the exact commit. The ASIO and VST3
host interfaces come from JUCE; this repository does not depend on files in the
separate Aggregate Audio driver project.

JUCE modules are available under AGPLv3 or the commercial JUCE licence. See
`JUCE-LICENSE.md`, the original `JUCE/LICENSE.md`, and the third-party notices
inside JUCE's individual modules. Dependency notices are preserved during the
migration. Any distribution must satisfy the applicable licensing terms.
