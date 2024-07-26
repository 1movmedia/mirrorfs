#!/bin/bash

which fusermount3 > /dev/null

# setup
mkdir -p mnt a b c
trap 'rm -rf mnt a b c' EXIT

set -ex

./mirrorfs -f -d a b c mnt &
mirrorfs_pid=$!
trap 'fusermount3 -q -u mnt; rm -rf mnt a b c; wait $mirrorfs_pid' EXIT

# Wait for mount with timeout
mount_timeout=30
start_time=$(date +%s)
while ! mountpoint mnt; do
    current_time=$(date +%s)
    if [ $((current_time - start_time)) -ge $mount_timeout ]; then
        echo "Timeout waiting for mount"
        kill $mirrorfs_pid
        wait $mirrorfs_pid
        exit 1
    fi
    echo "Waiting for mount..."
    sleep 1
done

echo "Mount successful"

# test write
echo foo > mnt/foo
test "$(cat a/foo)" == foo
test "$(cat b/foo)" == foo
test "$(cat c/foo)" == foo

# test read
echo bar > a/bar
echo bar > b/bar
echo bar > c/bar
test "$(cat mnt/bar)" == bar

# test rename
mv mnt/foo mnt/bar
test $(cat a/bar) == foo
test $(cat b/bar) == foo
test $(cat c/bar) == foo

# test metadata
chmod +x mnt/bar
test -x a/bar
test -x b/bar
test -x c/bar

echo All tests passed
