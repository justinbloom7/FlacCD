# Windows Explorer registration testing

Use Windows 10/11 with MSYS2 UCRT64 and the existing working ImgDrive setup.
Keep the deployment smoke-test implementation unchanged: CMake directly executes
`Smoke.cmd`. Configure **Release**, not `Releas`.

```bash
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
cmake --build build-windows --target flaccd-smoke flaccd-package
```

The existing 26 Windows tests remain registered. Two additional tests are
`shell-registration` (portable policy/quoting with a registry mock) and
`windows-shell-registration` (native HKCU lifecycle and ShellExecute argument
round trips). The native test creates an exclusively owned, unique ProgID under
`HKCU\Software\Classes\FlacCD.ShellTests.<unique ID>` and selects it explicitly
with `SEE_MASK_CLASSNAME`/`lpClass`. It does **not** associate that ProgID with an
extension, install the real Explorer verb, or change `.flac`, UserChoice, or
SystemFileAssociations. Cleanup removes only the newly created test key.
A harmless probe receives `mount` and the selected filename, records UTF-16
arguments, and exits. It never contacts ImgDrive or the agent. It tests executable
folders and filenames containing spaces, Unicode, shell metacharacters, literal
`%1`, `%L`, `%PATH%`, and `%%`. No shell interprets `&`, `^`, `!`, or parentheses.

### Isolated ShellExecute scope and diagnostics

The earlier fixture passed a generic key outside `Software\Classes` through
`SEE_MASK_CLASSKEY`. That fixture failed on the developer's Windows system with
the reported value `2147746293` (`0x800401F5`, `CO_E_APPNOTFOUND`, Application not
found). It assumed the arbitrary registry tree behaved like a registered file
class. The replacement uses a genuine, unassociated test ProgID visible through
HKCR and explicitly verifies that the merged class command equals the command
written by the production registration implementation before launching it.
[Microsoft documents](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)
`lpClass` selection of a ProgID or a shell-verb key under HKCR.

The old assertion also combined FALSE return and missing `hProcess`, reporting
`GetLastError()` for both. Success does not make that last-error value meaningful,
and ShellExecute can legitimately succeed without returning a process handle.
The updated test reports those cases separately. It still requires a new process
because this test's fresh static command has no DDE or delegate. Launch failures
include numeric/hex error values, system text, ProgID, registry root, verb, file,
registered command, hInstApp and process-handle presence. Probe exit, wait, output
and argument mismatches have separate diagnostics, including escaped expected
and actual UTF-16 bytes for a mismatch.

This validates native command lookup/expansion, argument delivery and registry
lifecycle; it does not validate the Explorer menu UI, the user's actual FLAC
association, or ImgDrive mounting/ejecting. Those remain manual integration
checks. The old combined assertion cannot establish whether Windows returned
FALSE or TRUE without a handle, so the precise original internal failure path
cannot be reconstructed from that message alone. The new diagnostics preserve
that distinction if a native launch still fails.

Run on Windows:

```bash
cmake --build build-windows
ctest --test-dir build-windows -R '^windows-shell-registration$' --output-on-failure
ctest --test-dir build-windows --output-on-failure
```

## Manual Explorer checks

1. Keep the packaged output in a permanent folder containing spaces and Unicode.
   From a normal, non-elevated CMD or PowerShell terminal, run that folder's
   `flaccd.exe shell status`. Note the current default `.flac` application.
2. Run `flaccd.exe shell install` twice, then `shell status`. Inspect only
   `HKCU\Software\Classes\SystemFileAssociations\.flac\shell\FlacCD.MountAudioCD`.
   The label is **Mount as Audio CD**; `MultiSelectModel` is `Single`. The command
   is `"<absolute flaccd.exe path>" mount "%1" --explorer`. Literal percent signs in the
   executable path are doubled for the ShellExecute command template. Values
   use Unicode `REG_SZ`, not environment-expanding `REG_EXPAND_SZ`.
3. Right-click a known-good FLAC album, such as your actual Zelda.flac path.
   Use **Show more options** on Windows 11. Confirm the audio CD and expected
   tracks appear on the actual ImgDrive drive; test playback and skipping.
4. Repeat with files/folders containing spaces, Unicode, `&`, `^`, `!`, `(`,
   `)`, apostrophes, semicolons, and literal `%1`/`%PATH%`. Use Explorer to invoke
   the menu so CMD does not expand the test filename before FlacCD receives it.
   Quotes and NUL are not legal Windows filename characters.
5. Verify double-click still opens the original default FLAC application. Verify
   other extensions have no new menu command. Select multiple files: this verb
   is intentionally single-selection only.
6. Eject and verify the existing agent cleanup behavior. Check `flaccd status`
   and `flaccd agent stop` still work. A registration command must not start an
   agent or require ImgDrive discovery; it should work even without ImgDrive.
7. Run `flaccd.exe shell uninstall` twice, then `shell status`. Confirm the verb
   disappeared and the default application is unchanged. Active mounts and their
   agent/resources must remain untouched by registration removal.
8. Install again, change the dedicated verb's command or add a test value, then
   try install/uninstall/status. Each must report a conflict without modifying
   that entry. Restore exactly the original values and uninstall normally.
9. Move the deployment only after uninstalling; reinstall from its new folder.
   An existing registration pointing elsewhere is preserved: uninstall it before
   reinstalling. If registration is interrupted or cannot be read, inspect the
   reported registry error rather than deleting the association parent key.
10. Mount a FLAC with no embedded CUE and no adjacent matching CUE. Explorer
    must display a persistent error dialog: **No embedded CUE sheet and [the
    expected full CUE path] does not exist. A cue sheet is required.** It also
    names the FLAC. Dismiss it normally; no backing files should be created.
    The same CLI mount without `--explorer` must report the error on stderr
    without a dialog. Test an unrelated external FILE reference and a malformed
    external CUE too.
11. Mount a valid external single-file audio CUE beside a FLAC without embedded
    CUE metadata. Confirm expected tracks and eject cleanup, with the external
    user CUE unchanged. Test spaces and Unicode. Add a conflicting external CUE
    beside a FLAC with valid embedded metadata: embedded layout retains
    precedence and the existing isolated staging fallback remains available.
12. Upgrade an installation whose Explorer registration lacks `--explorer`.
    `shell status` must recognize the legacy owned entry; `shell install` should
    migrate it. Foreign/modified entries must still be preserved.

Installation uses Unicode registry APIs, a per-user registration mutex, exclusive
creation of the dedicated verb, strict ownership/value checks, and nonrecursive
removal. Explorer receives the existing mount command directly; no CMD,
PowerShell, script, shell extension DLL, HKLM entry, or installer is involved.
Association-change notification requests an Explorer refresh; close and reopen
the context menu if necessary. Windows 11's classic menu placement is expected
for this static verb ([Microsoft context-menu guidance](https://blogs.windows.com/blog/2021/07/19/extending-the-context-menu-and-share-dialog-in-windows-11/)).
Quoted filename placeholders follow
[Microsoft's static-verb guidance](https://learn.microsoft.com/en-us/windows/win32/shell/fa-verbs).

Linux tests verify policy and construction only. Native registry operations,
ShellExecute expansion, Explorer refresh/menu placement and actual mounting must
be verified on Windows; they have not been executed by the Linux development agent.
