#!/usr/bin/env bash
#
# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Exercises the AltaStor backup library through the btest utility.
#
# The file backend is tested against a throwaway repository in a temporary
# directory. The S3 backend is only tested when ABR_TEST_S3_BUCKET names a
# bucket the current credentials may write to, otherwise it is skipped.
#
# Usage: scripts/tests.sh [--build] [--keep] [--verbose]

set -u -o pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BTEST="${BTEST:-}"
DO_BUILD=0
KEEP=0
VERBOSE=0

TESTS_RUN=0
TESTS_FAILED=0
CURRENT=""
CURRENT_ERRORS=()

# Output of the last run_btest call, and its exit status.
BTEST_OUT=""
BTEST_RC=0

if [ -t 1 ]; then
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_OFF=$'\033[0m'
else
    C_RED=""; C_GREEN=""; C_YELLOW=""; C_OFF=""
fi

usage()
{
    cat <<EOF
Usage: ${0##*/} [options]

Options:
  -b, --build     Configure and build the project before running the tests
  -k, --keep      Keep the temporary backup repository instead of deleting it
  -v, --verbose   Print the output of every btest invocation
  -h, --help      Show this help

Environment:
  BTEST                Path to the btest binary (default: search the build tree)
  BUILD_DIR            Build directory to use or create (default: ./build)
  ABR_TEST_S3_BUCKET   Bucket to run the S3 backend tests against (default: skip)
  ABR_TEST_S3_PATH     Path within that bucket (default: abr-selftest)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -b|--build)   DO_BUILD=1 ;;
        -k|--keep)    KEEP=1 ;;
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

#
# Locating and building the utility
#

build_project()
{
    echo "Building in ${BUILD_DIR} ..."
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug >/dev/null || {
        echo "${C_RED}CMake configuration failed.${C_OFF}" >&2
        exit 1
    }
    cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null || {
        echo "${C_RED}Build failed.${C_OFF}" >&2
        exit 1
    }
}

find_btest()
{
    local candidate
    for candidate in "${BUILD_DIR}/test/btest/btest" "${BUILD_DIR}/test/btest"; do
        if [ -x "${candidate}" ] && [ -f "${candidate}" ]; then
            echo "${candidate}"
            return 0
        fi
    done
    # Fall back to any build tree left in the project folder.
    find "${ROOT_DIR}" -type f -name btest -perm -u+x -print -quit 2>/dev/null
}

[ "${DO_BUILD}" -eq 1 ] && build_project

if [ -z "${BTEST}" ]; then
    BTEST="$(find_btest)"
fi

if [ -z "${BTEST}" ] || [ ! -x "${BTEST}" ]; then
    echo "${C_RED}btest binary not found.${C_OFF} Build it first:" >&2
    echo "  ${0##*/} --build" >&2
    echo "or point BTEST at an existing binary." >&2
    exit 1
fi

#
# Test harness
#

begin()
{
    CURRENT="$1"
    CURRENT_ERRORS=()
    TESTS_RUN=$((TESTS_RUN + 1))
}

fail()
{
    CURRENT_ERRORS+=("$1")
}

end()
{
    if [ ${#CURRENT_ERRORS[@]} -eq 0 ]; then
        printf '%s  ok  %s%s\n' "${C_GREEN}" "${CURRENT}" "${C_OFF}"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        printf '%sFAIL  %s%s\n' "${C_RED}" "${CURRENT}" "${C_OFF}"
        local err
        for err in "${CURRENT_ERRORS[@]}"; do
            printf '        %s\n' "${err}"
        done
        printf '      --- btest output ---\n'
        printf '%s\n' "${BTEST_OUT}" | sed 's/^/      | /'
    fi
    CURRENT_ERRORS=()
}

# Runs btest with the given arguments, capturing stdout and stderr together.
run_btest()
{
    BTEST_OUT="$("${BTEST}" "$@" 2>&1)"
    BTEST_RC=$?

    if [ "${VERBOSE}" -eq 1 ]; then
        printf '      $ btest %s (rc=%d)\n' "$*" "${BTEST_RC}"
        printf '%s\n' "${BTEST_OUT}" | sed 's/^/      | /'
    fi
}

assert_rc()
{
    [ "${BTEST_RC}" -eq "$1" ] || fail "expected exit code $1, got ${BTEST_RC}"
}

assert_out()
{
    grep -qF -- "$1" <<<"${BTEST_OUT}" || fail "expected output to contain: $1"
}

assert_no_out()
{
    grep -qF -- "$1" <<<"${BTEST_OUT}" && fail "expected output NOT to contain: $1"
    return 0
}

# btest reports some failures on stdout while still exiting 0 (a failed range
# load, a failed removal, an encryption mismatch), so a successful case has to
# be checked for error markers as well as for its exit code.
assert_no_errors()
{
    local marker
    for marker in "Failed" "does NOT match" "Error:"; do
        grep -qF -- "${marker}" <<<"${BTEST_OUT}" && fail "unexpected error in output: ${marker}"
    done
    return 0
}

assert_file()
{
    [ -f "$1" ] || fail "expected file to exist: $1"
}

assert_no_file()
{
    [ -e "$1" ] && fail "expected file to be gone: $1"
    return 0
}

assert_count()
{
    local pattern="$1" expected="$2" actual
    actual="$(grep -cE -- "${pattern}" <<<"${BTEST_OUT}")"
    [ "${actual}" -eq "${expected}" ] ||
        fail "expected ${expected} line(s) matching /${pattern}/, got ${actual}"
}

#
# The repository under test
#

REPO="$(mktemp -d "${TMPDIR:-/tmp}/abr-tests.XXXXXX")"

cleanup()
{
    if [ "${KEEP}" -eq 1 ]; then
        echo "Backup repository kept at ${REPO}"
    else
        chmod -R u+rwX "${REPO}" 2>/dev/null
        rm -rf "${REPO}"
    fi
}
trap cleanup EXIT

echo "btest:      ${BTEST}"
echo "repository: ${REPO}"
echo

#
# Argument handling
#

echo "-- command line --"

begin "no arguments prints the usage"
run_btest
assert_rc 1
assert_out "Usage:"
assert_out "btest [options] <bucket/dir> <command> [parameters]"
end

begin "an unknown option is rejected"
run_btest -f -Z "${REPO}" list
assert_rc 1
assert_out "Unknown option: -Z"
end

begin "an unknown command prints the usage"
run_btest -f "${REPO}" frobnicate
assert_rc 1
assert_out "Usage:"
end

begin "a missing command parameter prints the usage"
run_btest -f "${REPO}" backup vm1
assert_rc 1
assert_out "Usage:"
end

#
# Opening the repository
#

echo
echo "-- file backend: repository --"

begin "a missing folder is refused without --create"
run_btest -f "${REPO}/absent" list vm1
assert_rc 1
assert_out "Directory does not exist"
end

begin "--create opens a missing folder"
run_btest -f -c "${REPO}" list vm1
assert_rc 0
assert_out "No backups found"
end

begin "a world-readable folder is refused"
chmod 755 "${REPO}"
run_btest -f "${REPO}" list vm1
assert_rc 1
assert_out "expected 750 or stricter"
chmod 750 "${REPO}"
end

#
# Backup, list, restore
#

echo
echo "-- file backend: backup and restore --"

begin "a backup is created"
run_btest -f "${REPO}" backup vm1 full
assert_rc 0
assert_no_errors
assert_out "Backup 'full' created."
assert_file "${REPO}/vm1/full.abri"
assert_file "${REPO}/vm1/full.abrd"
end

begin "the new backup is listed"
run_btest -f "${REPO}" list vm1
assert_rc 0
assert_no_errors
assert_out "1 backup(s) found:"
assert_out "  full"
end

begin "an existing name is not overwritten"
run_btest -f "${REPO}" backup vm1 full
assert_rc 1
assert_out "already exists"
end

# The utility writes four chunks of 0x2000 bytes whose second half is zeroed,
# so empty block elimination must leave exactly four 8-sector ranges.
begin "the backup restores with the expected ranges"
run_btest -f "${REPO}" restore vm1 full
assert_rc 0
assert_no_errors
assert_out "Backup sectors: 268435456, sectorSize 512."
assert_count '^[0-9]+\) offset: ' 4
assert_out "1) offset: 0, length 8"
assert_out "2) offset: 128, length 8"
assert_out "3) offset: 2048, length 8"
assert_out "4) offset: 32768, length 8"
end

begin "a second backup lands beside the first"
run_btest -f "${REPO}" backup vm1 second
assert_rc 0
assert_no_errors
run_btest -f "${REPO}" list vm1
assert_rc 0
assert_out "2 backup(s) found:"
assert_out "  full"
assert_out "  second"
end

begin "backups live under nested paths"
run_btest -f "${REPO}" backup site/rack/vm2 nested
assert_rc 0
assert_no_errors
assert_file "${REPO}/site/rack/vm2/nested.abri"
run_btest -f "${REPO}" list site/rack/vm2
assert_rc 0
assert_out "1 backup(s) found:"
assert_out "  nested"
end

begin "an unrelated path lists nothing"
run_btest -f "${REPO}" list vm-does-not-exist
assert_rc 0
assert_out "No backups found"
assert_no_out "full"
end

#
# Encryption
#

echo
echo "-- file backend: encryption --"

begin "an encrypted backup round-trips"
run_btest -f "${REPO}" crypto vm1 sealed
assert_rc 0
assert_no_errors
assert_out "Backup sectors: 268435456, sectorSize 512."
assert_out "Range 1 data matches original."
assert_no_out "does NOT match"
end

begin "the encrypted payload is not stored in the clear"
if grep -qa "1234567890" "${REPO}/vm1/sealed.abrd"; then
    fail "plaintext marker found in ${REPO}/vm1/sealed.abrd"
fi
end

#
# Errors on missing backups
#

echo
echo "-- file backend: missing backups --"

begin "restoring an unknown backup fails"
run_btest -f "${REPO}" restore vm1 ghost
assert_rc 1
assert_out "Failed to open backup!"
end

begin "removing an unknown backup fails"
run_btest -f "${REPO}" remove vm1 ghost
assert_rc 1
assert_out "Failed to open backup!"
end

#
# Removal
#

echo
echo "-- file backend: removal --"

begin "a backup is removed with its data"
run_btest -f "${REPO}" remove vm1 full
assert_rc 0
assert_out "Backup removed."
assert_no_out "Failed to remove"
assert_no_file "${REPO}/vm1/full.abri"
assert_no_file "${REPO}/vm1/full.abrd"
end

begin "removal leaves the other backups alone"
run_btest -f "${REPO}" list vm1
assert_rc 0
assert_no_out "  full"
assert_out "  second"
assert_out "  sealed"
run_btest -f "${REPO}" restore vm1 second
assert_rc 0
assert_no_errors
end

begin "the last backup of a path can be removed"
run_btest -f "${REPO}" remove site/rack/vm2 nested
assert_rc 0
assert_out "Backup removed."
run_btest -f "${REPO}" list site/rack/vm2
assert_rc 0
assert_out "No backups found"
end

#
# Failure reporting
#
# These cases cover the paths where a failure is only visible in the exit code
# once the operation has already started, which is what makes the exit codes of
# restore and remove worth checking at all.
#

echo
echo "-- file backend: failure reporting --"

begin "a corrupted backup fails to restore"
run_btest -f "${REPO}" backup vm-broken corrupt
assert_rc 0
assert_no_errors
dd if=/dev/urandom of="${REPO}/vm-broken/corrupt.abrd" bs=512 count=1 conv=notrunc status=none 2>/dev/null
run_btest -f "${REPO}" restore vm-broken corrupt
assert_rc 1
assert_out "Failed to load range"
end

begin "a removal that cannot delete is reported"
run_btest -f "${REPO}" backup vm-locked pinned
assert_rc 0
assert_no_errors
chmod 550 "${REPO}/vm-locked"
run_btest -f "${REPO}" remove vm-locked pinned
assert_rc 1
assert_out "Failed to remove backup!"
assert_no_out "Backup removed."
chmod 750 "${REPO}/vm-locked"
end

#
# S3 backend, only when a bucket has been provided
#

echo
echo "-- s3 backend --"

S3_BUCKET="${ABR_TEST_S3_BUCKET:-}"
S3_PATH="${ABR_TEST_S3_PATH:-abr-selftest}"

if [ -z "${S3_BUCKET}" ]; then
    printf '%s skip  set ABR_TEST_S3_BUCKET to test the S3 backend%s\n' "${C_YELLOW}" "${C_OFF}"
else
    S3_NAME="run-$$-$(date +%s)"

    begin "s3: the bucket can be listed"
    run_btest "${S3_BUCKET}" list "${S3_PATH}"
    assert_rc 0
    assert_no_errors
    end

    begin "s3: a backup is created"
    run_btest "${S3_BUCKET}" backup "${S3_PATH}" "${S3_NAME}"
    assert_rc 0
    assert_no_errors
    assert_out "Backup '${S3_NAME}' created."
    end

    begin "s3: the backup is listed"
    run_btest "${S3_BUCKET}" list "${S3_PATH}"
    assert_rc 0
    assert_out "  ${S3_NAME}"
    end

    begin "s3: the backup restores with the expected ranges"
    run_btest "${S3_BUCKET}" restore "${S3_PATH}" "${S3_NAME}"
    assert_rc 0
    assert_no_errors
    assert_out "Backup sectors: 268435456, sectorSize 512."
    assert_count '^[0-9]+\) offset: ' 4
    end

    begin "s3: an encrypted backup round-trips"
    run_btest "${S3_BUCKET}" crypto "${S3_PATH}" "${S3_NAME}-enc"
    assert_rc 0
    assert_no_errors
    assert_out "Range 1 data matches original."
    end

    begin "s3: the backups are removed"
    run_btest "${S3_BUCKET}" remove "${S3_PATH}" "${S3_NAME}"
    assert_rc 0
    assert_out "Backup removed."
    run_btest "${S3_BUCKET}" remove "${S3_PATH}" "${S3_NAME}-enc"
    assert_rc 0
    assert_out "Backup removed."
    end
fi

#
# Summary
#

echo
if [ "${TESTS_FAILED}" -eq 0 ]; then
    printf '%s%d test(s) passed.%s\n' "${C_GREEN}" "${TESTS_RUN}" "${C_OFF}"
    exit 0
fi

printf '%s%d of %d test(s) failed.%s\n' "${C_RED}" "${TESTS_FAILED}" "${TESTS_RUN}" "${C_OFF}"
exit 1
