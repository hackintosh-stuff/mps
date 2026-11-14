#!/usr/bin/env python3
"""Check kext build inputs without compiling, installing, or touching hardware."""

import pathlib
import argparse
import ast
import plistlib
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument('--skip-reference', action='store_true', help='explicitly skip upstream PCI-table comparison in a source-only build package')
    args = parser.parse_args()
    root = args.root.resolve()
    errors = []
    sources = sorted((root / "MPS").glob("*.cpp"))
    if not sources:
        errors.append("MPS contains no C++ translation units")

    inputs = set((root / "MPS").rglob("*.h"))
    inputs.update(sources)
    inputs.update((root / "tools").glob("*.sh"))
    inputs.update((root / "tools").glob("*.py"))
    inputs.update((root / "tools").glob("*.cpp"))
    inputs.add(root / "MPS/Info.plist")
    inputs.add(root / "MPS.xcodeproj/project.pbxproj")
    for path in sorted(inputs):
        if b"\r" in path.read_bytes():
            errors.append("CR line endings: " + str(path.relative_to(root)))

    with (root / "MPS/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    version = info.get("CFBundleVersion")
    if not version or info.get("CFBundleShortVersionString") != version:
        errors.append("Info.plist bundle versions are missing or differ")
    if info.get("CFBundleExecutable") != "MPS" or info.get("CFBundleIdentifier") != "com.maz1.MPS":
        errors.append("Info.plist executable or identifier differs from the build entry points")

    build = (root / "tools/build-noxcode.sh").read_text(encoding="utf-8")
    for label, pattern in (
        ("CLI version", r"(?m)^VERSION=([^\s]+)$"),
        ("kmod version", r'KMOD_EXPLICIT_DECL\(com\.maz1\.MPS,\s*"([^"]+)"'),
    ):
        values = re.findall(pattern, build)
        if values != [version]:
            errors.append(label + " does not match Info.plist")

    controller = (root / "MPS/MPSController.cpp").read_text(encoding="utf-8")
    registry_versions = re.findall(
        r'(?:setProperty|MPS_DEBUG_PROP)\("MPSBuild",\s*"([^"]+)"\)',
        controller,
    )
    if registry_versions != [version]:
        errors.append("IORegistry MPSBuild does not match Info.plist")

    project = (root / "MPS.xcodeproj/project.pbxproj").read_text(encoding="utf-8")
    for setting in ("CURRENT_PROJECT_VERSION", "MARKETING_VERSION", "MODULE_VERSION"):
        values = re.findall(r"\b" + setting + r"\s*=\s*([^;\s]+);", project)
        if len(values) != 2 or any(value != version for value in values):
            errors.append("Xcode " + setting + " does not match Info.plist in both configurations")

    file_refs = dict(re.findall(
        r"([A-Fa-f0-9]{24})\s*/\*[^*]*\*/\s*=\s*\{isa = PBXFileReference;[^}]*?path = ([^;]+);",
        project,
    ))
    build_files = dict(re.findall(
        r"([A-Fa-f0-9]{24})\s*/\*[^*]*\*/\s*=\s*\{isa = PBXBuildFile;\s*fileRef = ([A-Fa-f0-9]{24})",
        project,
    ))
    compiled = []
    for block in re.findall(r"isa = PBXSourcesBuildPhase;([^}]+)", project):
        for build_id in re.findall(r"([A-Fa-f0-9]{24})\s*/\*", block):
            file_id = build_files.get(build_id)
            name = file_refs.get(file_id, "").strip('"')
            if not name:
                errors.append("Xcode source reference cannot be resolved: " + build_id)
            else:
                compiled.append(name)
    for source in sources:
        count = compiled.count(source.name)
        if count != 1:
            errors.append("Xcode Sources must compile {} exactly once (found {})".format(source.name, count))
    for source in compiled:
        if not (root / "MPS" / source).is_file():
            errors.append("Xcode compiles a missing source: " + source)

    for name in file_refs.values():
        name = name.strip('"')
        if name.endswith(('.h', '.cpp')) and not (root / 'MPS' / name).is_file():
            errors.append('Xcode references a missing driver input: ' + name)

    # Native administrator channels must not accidentally open an adjacent MPR.
    cli_source = (root / 'tools/mpsctl.py').read_text(encoding='utf-8')
    connect = None
    for statement in ast.parse(cli_source).body:
        if isinstance(statement, ast.Assign) and any(
            isinstance(target, ast.Tuple) and any(isinstance(item, ast.Name) and item.id == 'CONNECT_TYPE' for item in target.elts)
            for target in statement.targets
        ):
            target = statement.targets[0]
            index = [item.id for item in target.elts].index('CONNECT_TYPE')
            connect = ast.literal_eval(statement.value.elts[index])
    abi = (root / 'MPS/MPSControlABI.h').read_text(encoding='utf-8')
    native = re.search(r'kMPSControlConnectType\s*=\s*(0x[0-9a-fA-F]+)', abi)
    if connect != 0x4D505301 or not native or int(native.group(1), 16) != connect:
        errors.append('MPS IOUserClient/CLI connection type must agree at independent 0x4D505301')
    for path in sources + list((root / 'MPS').rglob('*.h')) + [root / 'tools/mpsctl.py']:
        content = path.read_text(encoding='utf-8')
        if re.search(r'\bMPR(?:Controller|BlockDev|UserClient)\b|com\.maz1\.MPR|["\']mpr_', content):
            errors.append('MPR runtime identifier remains in ' + str(path.relative_to(root)))
        if path.suffix in ('.h', '.cpp') and re.search(r'\bMPI(?:25|26)_|\bmpsBuild(?:Ieee|Nvme)Sgl\b', content):
            errors.append('SAS3 protocol symbol remains in ' + str(path.relative_to(root)))
    personalities = info.get('IOKitPersonalities', {})
    if set(personalities) != {'MPSController'} or personalities.get('MPSController', {}).get('IOClass') != 'MPSController':
        errors.append('MPS personality/class must be independent')

    # Parse the fixed reference's symbolic PCI table, including duplicate Intel
    # subsystem descriptions, and compare its unique vendor/device pairs.
    pci_match = re.search(r'kMPSPciIds\[\]\s*=\s*\{(.*?)\n\};', controller, re.S)
    driver_ids = re.findall(r'\{\s*(0x[0-9A-Fa-f]+)U?\s*,', pci_match.group(1)) if pci_match else []
    driver_ids = [int(value, 16) for value in driver_ids]
    plist_ids = [int(value, 16) for value in personalities.get('MPSController', {}).get('IOPCIPrimaryMatch', '').split()]
    if len(driver_ids) != 17 or len(set(driver_ids)) != 17 or set(driver_ids) != set(plist_ids) or len(plist_ids) != 17:
        errors.append('Runtime/Info.plist PCI tables must agree with exactly 17 unique SAS2 IDs')
    upstream = root / 'reference/freebsd/sys/dev/mps'
    if not args.skip_reference:
        pci = (upstream / 'mps_pci.c').read_text(encoding='utf-8')
        config = (upstream / 'mpi/mpi2_cnfg.h').read_text(encoding='utf-8')
        values = {name: int(value, 16) for name, value in re.findall(
            r'#define\s+(MPI2_MFGPAGE_(?:DEVID_[A-Za-z0-9_]+|VENDORID_LSI))\s+\((0x[0-9A-Fa-f]+)\)', config)}
        table = re.search(r'\}\s*mps_identifiers\[\]\s*=\s*\{(.*?)\n\};', pci, re.S)
        names = re.findall(r'\{\s*MPI2_MFGPAGE_VENDORID_LSI\s*,\s*(MPI2_MFGPAGE_DEVID_\w+)', table.group(1)) if table else []
        expected = {(values[name] << 16) | values['MPI2_MFGPAGE_VENDORID_LSI'] for name in names}
        if len(expected) != 17 or set(driver_ids) != expected:
            errors.append('Runtime PCI IDs differ from FreeBSD mps_pci.c unique IDs')

    # A CONFIG payload is one complete MPI buffer. LAST/EOB without EOL does
    # not end a list. Enforce the shared helper or explicit END_OF_LIST at all
    # core assignment sites, and require the management serializer's terminal flag.
    config_sites = 0
    for path in sources:
        content = path.read_text(encoding='utf-8')
        for assignment in re.findall(r'config->SgeFlagsLength\s*=\s*(.*?);', content, re.S):
            config_sites += 1
            if 'MPI2_SGE_FLAGS_END_OF_LIST' not in assignment:
                errors.append('CONFIG SGE lacks END_OF_LIST in ' + path.name)
        for call in re.findall(r'mpsMpiSimple\(\(UInt8\s*\*\)\s*config\s*\+\s*0x1[cC]\s*,(.*?)\);', content, re.S):
            config_sites += 1
            # Arguments after the SGE pointer: address, length, write,
            # optional terminal. Default terminal=true is part of the helper.
            if len(call.split(',')) > 3 and call.split(',')[-1].strip() != 'true':
                errors.append('CONFIG helper must terminate its SGL in ' + path.name)
    if config_sites != 2:
        errors.append('Expected two checked core CONFIG SGE sites, found ' + str(config_sites))
    control = (root / 'MPS/MPSControl.cpp').read_text(encoding='utf-8')
    if not re.search(r'mpsManagementSimple64\(frame\s*\+\s*prefix,\s*address,\s*length,\s*write,\s*true\)', control):
        errors.append('Management CONFIG SGE must use the terminal legacy MPI serializer')

    if errors:
        for error in errors:
            print("error: " + error, file=sys.stderr)
        return 1
    reference_status = 'explicitly SKIPPED' if args.skip_reference else '17 IDs compared with local FreeBSD reference'
    print("Build inputs OK: {} translation units, version {}, LF text, independent bundle/class/control IDs, {}.".format(len(sources), version, reference_status))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        print("error: " + str(error), file=sys.stderr)
        sys.exit(1)
