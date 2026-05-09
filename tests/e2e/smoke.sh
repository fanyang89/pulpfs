#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PULPFS_BIN="${1:-$ROOT_DIR/build/src/pulpfs}"
COMPOSE_FILE="$ROOT_DIR/tests/e2e/compose.yml"

WORK_DIR="${PULPFS_E2E_WORK_DIR:-$(mktemp -d)}"
MOUNT_DIR="$WORK_DIR/mnt"
META_DIR="$WORK_DIR/meta"
MC_CONFIG_DIR="$WORK_DIR/mc"
META_LOG="$WORK_DIR/meta.log"
FUSE_LOG="$WORK_DIR/fuse.log"
GC_LOG="$WORK_DIR/gc.log"
META_PID=""
FUSE_PID=""

MINIO_ENDPOINT="127.0.0.1:9000"
MINIO_HTTP_ENDPOINT="http://127.0.0.1:9000"
MINIO_ACCESS_KEY="minioadmin"
MINIO_SECRET_KEY="minioadmin"
BUCKET="pulpfs"
PREFIX="pulpfs"

compose() {
    docker compose -f "$COMPOSE_FILE" "$@"
}

cleanup() {
    set +e
    if mountpoint -q "$MOUNT_DIR"; then
        fusermount3 -u "$MOUNT_DIR"
    fi
    if [[ -n "$FUSE_PID" ]]; then
        kill "$FUSE_PID" 2>/dev/null || true
        wait "$FUSE_PID" 2>/dev/null || true
    fi
    if [[ -n "$META_PID" ]]; then
        kill "$META_PID" 2>/dev/null || true
        wait "$META_PID" 2>/dev/null || true
    fi
    compose down -v --remove-orphans >/dev/null 2>&1 || true
    if [[ -z "${PULPFS_E2E_KEEP_WORK_DIR:-}" ]]; then
        rm -rf "$WORK_DIR"
    else
        printf 'kept e2e work dir: %s\n' "$WORK_DIR"
    fi
}

fail() {
    printf 'error: %s\n' "$*" >&2
    if [[ -f "$META_LOG" ]]; then
        printf '\n--- meta.log ---\n' >&2
        tail -n 200 "$META_LOG" >&2 || true
    fi
    if [[ -f "$FUSE_LOG" ]]; then
        printf '\n--- fuse.log ---\n' >&2
        tail -n 200 "$FUSE_LOG" >&2 || true
    fi
    if [[ -f "$GC_LOG" ]]; then
        printf '\n--- gc.log ---\n' >&2
        tail -n 200 "$GC_LOG" >&2 || true
    fi
    exit 1
}

wait_http_ready() {
    local url="$1"
    local deadline=$((SECONDS + 90))
    until curl -fsS "$url" >/dev/null; do
        if (( SECONDS >= deadline )); then
            fail "timed out waiting for $url"
        fi
        sleep 1
    done
}

wait_tcp_ready() {
    local host="$1"
    local port="$2"
    local deadline=$((SECONDS + 60))
    until (exec 3<>"/dev/tcp/$host/$port") >/dev/null 2>&1; do
        if (( SECONDS >= deadline )); then
            fail "timed out waiting for $host:$port"
        fi
        sleep 1
    done
}

wait_mount_ready() {
    local deadline=$((SECONDS + 30))
    until mountpoint -q "$MOUNT_DIR"; do
        if (( SECONDS >= deadline )); then
            fail "timed out waiting for FUSE mount"
        fi
        sleep 1
    done
}

assert_eq() {
    local expected="$1"
    local actual="$2"
    local message="$3"
    if [[ "$actual" != "$expected" ]]; then
        fail "$message: expected '$expected', got '$actual'"
    fi
}

trap cleanup EXIT

[[ -x "$PULPFS_BIN" ]] || fail "pulpfs binary is not executable: $PULPFS_BIN"
[[ -e /dev/fuse ]] || fail "/dev/fuse is not available"

mkdir -p "$MOUNT_DIR" "$META_DIR" "$MC_CONFIG_DIR"

compose up -d
wait_http_ready "$MINIO_HTTP_ENDPOINT/minio/health/ready"

docker run --rm --network host -v "$MC_CONFIG_DIR:/root/.mc" pgsty/mc \
    alias set local "$MINIO_HTTP_ENDPOINT" "$MINIO_ACCESS_KEY" "$MINIO_SECRET_KEY" >/dev/null
docker run --rm --network host -v "$MC_CONFIG_DIR:/root/.mc" pgsty/mc \
    mb -p "local/$BUCKET" >/dev/null || true
docker run --rm -v "$MC_CONFIG_DIR:/root/.mc" --entrypoint chown pgsty/mc \
    -R "$(id -u):$(id -g)" /root/.mc >/dev/null || true

"$PULPFS_BIN" \
    --service=meta \
    --listen_address=127.0.0.1:3000 \
    --raft_listen_address=127.0.0.1:9001 \
    --raft_data_dir="$META_DIR" \
    >"$META_LOG" 2>&1 &
META_PID=$!
wait_tcp_ready 127.0.0.1 3000

if "$PULPFS_BIN" --service=fuse --mountpoint="$MOUNT_DIR" --meta_address=127.0.0.1:3000 \
    --s3_bucket="$BUCKET" >"$WORK_DIR/fuse-missing-endpoint.log" 2>&1; then
    fail "fuse without --s3_endpoint unexpectedly succeeded"
fi
if "$PULPFS_BIN" --service=fuse --mountpoint="$MOUNT_DIR" --meta_address=127.0.0.1:3000 \
    --s3_endpoint="$MINIO_ENDPOINT" >"$WORK_DIR/fuse-missing-bucket.log" 2>&1; then
    fail "fuse without --s3_bucket unexpectedly succeeded"
fi

AWS_ACCESS_KEY_ID="$MINIO_ACCESS_KEY" AWS_SECRET_ACCESS_KEY="$MINIO_SECRET_KEY" \
"$PULPFS_BIN" \
    --service=fuse \
    --mountpoint="$MOUNT_DIR" \
    --meta_address=127.0.0.1:3000 \
    --s3_endpoint="$MINIO_ENDPOINT" \
    --s3_bucket="$BUCKET" \
    --s3_prefix="$PREFIX" \
    --s3_use_https=false \
    --s3_verify_ssl=false \
    >"$FUSE_LOG" 2>&1 &
FUSE_PID=$!
wait_mount_ready

mkdir "$MOUNT_DIR/dir"
printf 'hello' >"$MOUNT_DIR/dir/file"
assert_eq "hello" "$(cat "$MOUNT_DIR/dir/file")" "read after write"

python3 - "$MOUNT_DIR/read-before-close" <<'PY'
import os
import sys

path = sys.argv[1]
fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_TRUNC, 0o644)
try:
    os.write(fd, b"pending")
    os.lseek(fd, 0, os.SEEK_SET)
    data = os.read(fd, 7)
    if data != b"pending":
        raise SystemExit(f"read before close: expected b'pending', got {data!r}")
finally:
    os.close(fd)
PY

printf 'base' >"$MOUNT_DIR/append-before-close"
python3 - "$MOUNT_DIR/append-before-close" <<'PY'
import os
import sys

path = sys.argv[1]
fd = os.open(path, os.O_RDWR | os.O_APPEND)
try:
    os.write(fd, b"-tail")
    os.lseek(fd, 0, os.SEEK_SET)
    data = os.read(fd, 9)
    if data != b"base-tail":
        raise SystemExit(f"append read before close: expected b'base-tail', got {data!r}")
finally:
    os.close(fd)
PY

printf 'old-value' >"$MOUNT_DIR/trunc"
printf 'new' >"$MOUNT_DIR/trunc"
assert_eq "new" "$(cat "$MOUNT_DIR/trunc")" "O_TRUNC write"

printf '1' >"$MOUNT_DIR/append"
printf '2' >>"$MOUNT_DIR/append"
printf '3' >>"$MOUNT_DIR/append"
assert_eq "123" "$(cat "$MOUNT_DIR/append")" "O_APPEND writes"

truncate -s 2 "$MOUNT_DIR/append"
assert_eq "12" "$(cat "$MOUNT_DIR/append")" "truncate file"

chmod 600 "$MOUNT_DIR/append"
touch -m -t 202001010000 "$MOUNT_DIR/append"

mv "$MOUNT_DIR/dir/file" "$MOUNT_DIR/dir/renamed"
assert_eq "hello" "$(cat "$MOUNT_DIR/dir/renamed")" "rename file"

mkdir "$MOUNT_DIR/old-dir"
mv "$MOUNT_DIR/old-dir" "$MOUNT_DIR/new-dir"
rmdir "$MOUNT_DIR/new-dir"

rm "$MOUNT_DIR/dir/renamed"
rmdir "$MOUNT_DIR/dir"

df "$MOUNT_DIR" >/dev/null

AWS_ACCESS_KEY_ID="$MINIO_ACCESS_KEY" AWS_SECRET_ACCESS_KEY="$MINIO_SECRET_KEY" \
"$PULPFS_BIN" \
    --service=gc \
    --meta_address=127.0.0.1:3000 \
    --s3_endpoint="$MINIO_ENDPOINT" \
    --s3_bucket="$BUCKET" \
    --s3_prefix="$PREFIX" \
    --s3_use_https=false \
    --s3_verify_ssl=false \
    --gc_grace_seconds=0 \
    --gc_dry_run=true \
    >"$GC_LOG" 2>&1

printf 'pulpfs e2e smoke passed\n'
