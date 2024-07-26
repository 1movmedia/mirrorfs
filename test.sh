#!/bin/bash

which fusermount3 > /dev/null

# setup
mkdir -p mnt a b c
trap 'rm -rf mnt a b c' EXIT

set -ex

valgrind --leak-check=full ./mirrorfs -d a b c mnt
trap 'fusermount3 -q -u mnt; rm -rf mnt a b c' EXIT

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
