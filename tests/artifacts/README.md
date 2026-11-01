# PPC64 Phase 17 checkpoint v6 fixture

`ppc64-phase17-v6.gxcp` is an authentic binary emitted by the Phase 17-era writer at source commit `265657170cebd7e93c26c4739055310112253a18`. The historical serializer in `src/vm/VMCheckpoint.cpp` was not modified. The isolated archive received only the additional generator source and one CMake executable target.

- Header magic: `GXPPC64V`
- Header/checkpoint format: v6
- Historical build identity: `guideXOS-Hypervisor/PPC64-VM-checkpoint-v6`
- File size: 2,109,857 bytes
- SHA-256: `86EB3B924DCBA3F5DA48562387BAD026EDFCD0A8CF7D34B2205A3F2241832196`
- Generator source: [generate_ppc64_phase17_v6_fixture.cpp](generate_ppc64_phase17_v6_fixture.cpp)

## Generation procedure

1. From a clean or otherwise preserved checkout containing commit `265657170cebd7e93c26c4739055310112253a18`, extract without changing branches or worktrees:

   ```powershell
   git archive 265657170cebd7e93c26c4739055310112253a18 | tar -x -C <isolated-source-dir>
   ```

2. Copy this repository's `tests/artifacts/generate_ppc64_phase17_v6_fixture.cpp` into the extracted archive as `tools/generate_ppc64_v6_fixture.cpp`. Add a CMake target named `generate_ppc64_v6_fixture` that compiles that file, `${ALL_SOURCES}`, and `${HEADERS}`; link `ws2_32` on Windows. The target and copied generator are added only to the extracted archive.
3. Configure and build in a separate directory with the project toolchain:

   ```powershell
   cmake -S <isolated-source-dir> -B <isolated-build-dir> -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
   cmake --build <isolated-build-dir> --target generate_ppc64_v6_fixture -j 6
   ```

4. Give the generator a new, nonexistent output path. It runs the historical VM and calls that revision's `VMCheckpointCodec::write`; it does not synthesize or patch checkpoint bytes.

The fixture configuration is a 2 MiB PPC64 VM with XICS compatibility, the built-in `hvterm1`, guest-declared reservations for a partition table and its radix structures, a known RAM pattern, XICS source `0x1111` at priority `0x33`, registered partition 7, DEC value 125, terminal output `V6OK`, and a continuation at guest real address `0x10010`. Active LPID is 0; partition 7 is registered but intentionally inactive. The current test checks the serialized v6 header, restores the image with the v8 reader, checks CPU/RAM/terminal/XICS/DEC/partition state, and executes the next guest instruction.

The `.gxcp` file is intentionally checked in as test data. Do not regenerate it in place; use a new output path, verify the header and hash, then make a deliberate fixture update if required.
